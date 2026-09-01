/* Host tests for the program loader's data transformations. Phase X-P1 of
 * docs/exec/.
 *
 * A correction to the plan, recorded here because the next person will look for
 * the file the plan named. X-P1 said to create `kernel/exec/image.c` holding a
 * pure segment-layout function. It already exists: `kernel/core/elf.c` is
 * portable, takes headers and a bias, and produces exactly the list of
 * (vaddr, file offset, filesz, memsz, flags) a loader must satisfy. Writing a
 * second one would have been a duplicate parser, which is worse than a long
 * file - two things that must agree about a security boundary and no build that
 * checks they do.
 *
 * What was genuinely missing is this: the layout was reachable only through a
 * boot. The cases below are the ones that have actually bitten a loader here or
 * are one edit away from doing so, and each says which.
 */

#include <stdio.h>
#include <string.h>

#include "vibeos/elf.h"

int test_loader(void);

static int g_fail;

static int expect(int cond, const char *what) {
    if (!cond) {
        printf("FAIL:loader %s\n", what);
        g_fail = 1;
    }
    return cond;
}

/* ---- a minimal ELF64 builder --------------------------------------------
 *
 * Hand-built rather than checked in as a binary: a test whose input is an
 * opaque blob cannot say which byte it is exercising, and every case here is
 * about one specific field. */

#define EHDR_SIZE 64u
#define PHDR_SIZE 56u
#define PT_LOAD 1u
#define PF_X 1u
#define PF_W 2u
#define PF_R 4u

static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v) {
    uint32_t i;
    for (i = 0; i < 4u; i++) { p[i] = (uint8_t)(v >> (8u * i)); }
}

static void put64(uint8_t *p, uint64_t v) {
    uint32_t i;
    for (i = 0; i < 8u; i++) { p[i] = (uint8_t)(v >> (8u * i)); }
}

/* Write an ELF64 header for `phnum` program headers placed straight after it.
 * `etype` is 2 for ET_EXEC, 3 for ET_DYN. */
static void build_ehdr(uint8_t *img, uint16_t etype, uint64_t entry,
                       uint16_t phnum) {
    memset(img, 0, EHDR_SIZE);
    img[0] = 0x7F; img[1] = 'E'; img[2] = 'L'; img[3] = 'F';
    img[4] = 2;    /* ELFCLASS64      */
    img[5] = 1;    /* ELFDATA2LSB     */
    img[6] = 1;    /* EV_CURRENT      */
    put16(img + 16, etype);
    put16(img + 18, 0x3E);            /* EM_X86_64 */
    put32(img + 20, 1);
    put64(img + 24, entry);
    put64(img + 32, EHDR_SIZE);       /* e_phoff */
    put16(img + 52, (uint16_t)EHDR_SIZE);
    put16(img + 54, (uint16_t)PHDR_SIZE);
    put16(img + 56, phnum);
}

static void build_phdr(uint8_t *img, uint32_t idx, uint32_t flags,
                       uint64_t off, uint64_t vaddr, uint64_t filesz,
                       uint64_t memsz, uint64_t align) {
    uint8_t *p = img + EHDR_SIZE + (uint64_t)idx * PHDR_SIZE;

    memset(p, 0, PHDR_SIZE);
    put32(p + 0, PT_LOAD);
    put32(p + 4, flags);
    put64(p + 8, off);
    put64(p + 16, vaddr);
    put64(p + 24, vaddr);             /* p_paddr, unused */
    put64(p + 32, filesz);
    put64(p + 40, memsz);
    put64(p + 48, align);
}

#define IMG_BYTES 65536u
#define BASE 0x400000ull

static uint8_t g_img[IMG_BYTES];

static const vibeos_elf_segment_t *seg_at(const vibeos_elf_image_t *im,
                                          uint64_t vaddr) {
    uint32_t i;
    for (i = 0; i < im->count; i++) {
        if (im->seg[i].vaddr == vaddr) {
            return &im->seg[i];
        }
    }
    return 0;
}

/* ---- layout cases -------------------------------------------------------- */

static void case_two_segments_with_a_hole(void) {
    vibeos_elf_image_t im;
    const vibeos_elf_segment_t *s;

    /* Text at BASE, data a long way above it. The gap is not part of the
     * image and must not be mapped: a loader that fills from min_vaddr to
     * end_vaddr hands the program writable pages it never asked for, and the
     * span is what gets reserved, not what gets backed. */
    memset(g_img, 0, sizeof(g_img));
    build_ehdr(g_img, 2, BASE + 0x100ull, 2);
    build_phdr(g_img, 0, PF_R | PF_X, 0x1000, BASE, 0x800, 0x800, 0x1000);
    build_phdr(g_img, 1, PF_R | PF_W, 0x2000, BASE + 0x10000ull, 0x40, 0x40, 0x1000);

    if (!expect(vibeos_elf_parse_ex(g_img, sizeof(g_img), 0, BASE,
                                    BASE + 0x100000ull, 0, &im) == VIBEOS_ELF_OK,
                "a two-segment image with a hole was refused")) {
        return;
    }
    expect(im.count == 2u, "the segment list lost a segment");
    expect(im.min_vaddr == BASE, "min_vaddr is not the first segment's page");
    expect(im.end_vaddr == BASE + 0x11000ull, "end_vaddr does not cover the last segment");
    expect(im.image_span == 0x11000ull, "image_span is not end - min");

    /* The hole itself: a page in the gap belongs to no segment. */
    s = seg_at(&im, BASE + 0x10000ull);
    expect(s != 0, "the second segment is not in the list");
    expect(vibeos_elf_page_flags(&im, BASE + 0x8000ull) == 0u,
           "a page in the hole between two segments claims permissions");
}

static void case_bss_tail(void) {
    vibeos_elf_image_t im;
    const vibeos_elf_segment_t *s;

    /* memsz > filesz: the .bss tail. The bytes past filesz are not in the file
     * and must be zero, not whatever the staging buffer held - which is how a
     * short read once produced a plausible ELF made of two programs. */
    memset(g_img, 0, sizeof(g_img));
    build_ehdr(g_img, 2, BASE, 1);
    build_phdr(g_img, 0, PF_R | PF_W, 0x1000, BASE, 0x30, 0x3000, 0x1000);

    if (!expect(vibeos_elf_parse_ex(g_img, sizeof(g_img), 0, BASE,
                                    BASE + 0x100000ull, 0, &im) == VIBEOS_ELF_OK,
                "a segment with a bss tail was refused")) {
        return;
    }
    s = seg_at(&im, BASE);
    if (!expect(s != 0, "the segment is not in the list")) {
        return;
    }
    expect(s->filesz == 0x30ull, "filesz was rounded");
    expect(s->memsz == 0x3000ull, "the bss tail was lost - memsz collapsed to filesz");
    expect(im.end_vaddr == BASE + 0x3000ull,
           "end_vaddr covers only the file part, so the bss would be unmapped");
}

static void case_fill_page_zeroes_beyond_the_file(void) {
    vibeos_elf_image_t im;
    uint8_t page[VIBEOS_ELF_PAGE_SIZE];
    uint32_t i;
    int clean = 1;

    memset(g_img, 0, sizeof(g_img));
    build_ehdr(g_img, 2, BASE, 1);
    build_phdr(g_img, 0, PF_R | PF_W, 0x1000, BASE, 0x10, 0x2000, 0x1000);
    for (i = 0; i < 0x10u; i++) {
        g_img[0x1000u + i] = (uint8_t)(0xA0u + i);
    }
    if (!expect(vibeos_elf_parse_ex(g_img, sizeof(g_img), 0, BASE,
                                    BASE + 0x100000ull, 0, &im) == VIBEOS_ELF_OK,
                "parse refused a bss image")) {
        return;
    }

    /* The page is filled with a pattern first: a filler that only *copies* and
     * forgets to zero passes against a buffer that was already zero, which is
     * the shape of bug that survives a careless test. */
    memset(page, 0x5A, sizeof(page));
    vibeos_elf_fill_page(&im, g_img, BASE, page);
    for (i = 0; i < 0x10u; i++) {
        if (page[i] != (uint8_t)(0xA0u + i)) { clean = 0; }
    }
    expect(clean, "the file-backed bytes of the first page are wrong");
    clean = 1;
    for (i = 0x10u; i < VIBEOS_ELF_PAGE_SIZE; i++) {
        if (page[i] != 0u) { clean = 0; }
    }
    expect(clean, "the bytes past filesz kept the previous contents of the page");

    /* And the second page, which the file does not reach at all. */
    memset(page, 0x5A, sizeof(page));
    vibeos_elf_fill_page(&im, g_img, BASE + VIBEOS_ELF_PAGE_SIZE, page);
    clean = 1;
    for (i = 0; i < VIBEOS_ELF_PAGE_SIZE; i++) {
        if (page[i] != 0u) { clean = 0; }
    }
    expect(clean, "a page entirely inside the bss was not zeroed");
}

static void case_two_segments_sharing_a_page(void) {
    vibeos_elf_image_t im;
    uint32_t flags;

    /* Text ending mid-page and data starting in the same page. The page is
     * allocated once and has to carry the permissions of both - this is the
     * case that makes a page-at-a-time loader correct and a segment-at-a-time
     * one map the same page twice with the second overwriting the first. */
    memset(g_img, 0, sizeof(g_img));
    build_ehdr(g_img, 2, BASE, 2);
    build_phdr(g_img, 0, PF_R | PF_X, 0x1000, BASE, 0x80, 0x80, 0x1000);
    build_phdr(g_img, 1, PF_R | PF_W, 0x1080, BASE + 0x80ull, 0x40, 0x40, 0x1000);

    if (!expect(vibeos_elf_parse_ex(g_img, sizeof(g_img), 0, BASE,
                                    BASE + 0x100000ull, 0, &im) == VIBEOS_ELF_OK,
                "two segments sharing a page were refused")) {
        return;
    }
    flags = vibeos_elf_page_flags(&im, BASE);
    expect((flags & VIBEOS_ELF_X) != 0u,
           "the shared page lost the executable permission of the first segment");
    expect((flags & VIBEOS_ELF_W) != 0u,
           "the shared page lost the writable permission of the second segment");
}

static void case_outside_the_allowed_range(void) {
    vibeos_elf_image_t im;

    /* A file that asks to be placed on top of the kernel. Refused by range,
     * not trusted - the kernel is linked at 64 MiB and a crafted image asking
     * to land between the two user windows is the reason the bounds are
     * arguments rather than assumptions. */
    memset(g_img, 0, sizeof(g_img));
    build_ehdr(g_img, 2, 0x4000000ull, 1);
    build_phdr(g_img, 0, PF_R | PF_W, 0x1000, 0x4000000ull, 0x40, 0x40, 0x1000);

    expect(vibeos_elf_parse_ex(g_img, sizeof(g_img), 0, BASE,
                               BASE + 0x100000ull, 0, &im) == VIBEOS_ELF_ERANGE,
           "an image placed outside the allowed window was accepted");
}

static void case_truncated_is_refused_not_parsed(void) {
    vibeos_elf_image_t im;

    /* The failure that once made a short file look complete: a flaky sector
     * produced a file the directory said was whole. It must be refused, not
     * parsed as far as it goes. */
    memset(g_img, 0, sizeof(g_img));
    build_ehdr(g_img, 2, BASE, 1);
    build_phdr(g_img, 0, PF_R | PF_X, 0x1000, BASE, 0x800, 0x800, 0x1000);

    expect(vibeos_elf_parse_ex(g_img, 0x1400, 0, BASE,
                               BASE + 0x100000ull, 0, &im) == VIBEOS_ELF_ETRUNCATED,
           "a file shorter than its own headers claim was accepted");
}

static void case_dyn_is_biased_once(void) {
    vibeos_elf_image_t im;
    const uint64_t bias = 0x500000ull;
    const vibeos_elf_segment_t *s;

    /* Every address the parser reports already includes the bias. A caller
     * that adds it again places the image twice as far up as it asked - and
     * the symptom is a position-independent program faulting on its own entry,
     * which reads exactly like a bad entry point. */
    memset(g_img, 0, sizeof(g_img));
    build_ehdr(g_img, 3, 0x1000ull, 1);
    build_phdr(g_img, 0, PF_R | PF_X, 0x1000, 0x1000ull, 0x400, 0x400, 0x1000);

    if (!expect(vibeos_elf_parse_ex(g_img, sizeof(g_img), bias, BASE,
                                    BASE + 0x1000000ull, VIBEOS_ELF_ALLOW_DYN,
                                    &im) == VIBEOS_ELF_OK,
                "a position-independent image was refused")) {
        return;
    }
    expect(im.is_dyn != 0u, "an ET_DYN image was not reported as one");
    expect(im.load_bias == bias, "the bias was not reported back");
    expect(im.entry == bias + 0x1000ull, "the entry does not carry the bias");
    s = seg_at(&im, bias + 0x1000ull);
    expect(s != 0, "the segment address does not carry the bias");

    /* image_span does not depend on the bias: a caller has to know how much to
     * reserve before it can choose one. */
    expect(im.image_span == 0x1000ull, "image_span moved with the bias");
}

static void case_dyn_refused_when_not_allowed(void) {
    vibeos_elf_image_t im;

    memset(g_img, 0, sizeof(g_img));
    build_ehdr(g_img, 3, 0x1000ull, 1);
    build_phdr(g_img, 0, PF_R | PF_X, 0x1000, 0x1000ull, 0x400, 0x400, 0x1000);

    expect(vibeos_elf_parse_ex(g_img, sizeof(g_img), 0, BASE,
                               BASE + 0x1000000ull, 0, &im) == VIBEOS_ELF_EDYNAMIC,
           "ET_DYN was accepted by a caller that had not opted in");
}

static void case_not_an_elf(void) {
    vibeos_elf_image_t im;

    memset(g_img, 0, sizeof(g_img));
    build_ehdr(g_img, 2, BASE, 1);
    build_phdr(g_img, 0, PF_R | PF_X, 0x1000, BASE, 0x400, 0x400, 0x1000);
    g_img[1] = 'X';

    expect(vibeos_elf_parse_ex(g_img, sizeof(g_img), 0, BASE,
                               BASE + 0x100000ull, 0, &im) == VIBEOS_ELF_ENOTELF,
           "a file with bad magic was parsed");
}

/* ---- which pages may be mapped from the file itself ----------------------
 *
 * `vibeos_elf_page_file_offset` decides whether a page can be pointed at the
 * page cache's own copy instead of being built in a fresh frame. Every "yes"
 * it gets wrong maps file bytes where zeroes belong, and a program whose .bss
 * starts out holding whatever followed .data in the file *runs* - it just runs
 * wrong, much later. So the cases below are mostly about it saying no.
 */

static void case_file_offset_plain_page(void) {
    vibeos_elf_image_t im;
    uint64_t off = 0;

    /* Two full pages of pure text: both are the file's, at the file's offsets. */
    memset(g_img, 0, sizeof(g_img));
    build_ehdr(g_img, 2, BASE, 1);
    build_phdr(g_img, 0, PF_R | PF_X, 0x1000, BASE, 0x2000, 0x2000, 0x1000);

    if (!expect(vibeos_elf_parse_ex(g_img, sizeof(g_img), 0, BASE,
                                    BASE + 0x100000ull, 0, &im) == VIBEOS_ELF_OK,
                "a two-page text segment was refused")) {
        return;
    }
    if (expect(vibeos_elf_page_file_offset(&im, BASE, &off) == 1,
               "the first page of a whole-page segment was not file-backed")) {
        expect(off == 0x1000ull, "the first page's file offset is wrong");
    }
    if (expect(vibeos_elf_page_file_offset(&im, BASE + 0x1000ull, &off) == 1,
               "the second page of a whole-page segment was not file-backed")) {
        expect(off == 0x2000ull, "the second page's file offset is wrong");
    }
}

static void case_file_offset_refuses_the_bss_tail(void) {
    vibeos_elf_image_t im;
    uint64_t off = 0;

    /* filesz covers one page and a bit; memsz runs to three. The page holding
     * the boundary is part file and part .bss, and the pages past it are pure
     * .bss. Saying yes to any of them puts file bytes where zeroes belong. */
    memset(g_img, 0, sizeof(g_img));
    build_ehdr(g_img, 2, BASE, 1);
    build_phdr(g_img, 0, PF_R | PF_W, 0x1000, BASE, 0x1800, 0x3000, 0x1000);

    if (!expect(vibeos_elf_parse_ex(g_img, sizeof(g_img), 0, BASE,
                                    BASE + 0x100000ull, 0, &im) == VIBEOS_ELF_OK,
                "a segment with a bss tail was refused")) {
        return;
    }
    expect(vibeos_elf_page_file_offset(&im, BASE, &off) == 1,
           "a page wholly inside filesz was not file-backed");
    expect(vibeos_elf_page_file_offset(&im, BASE + 0x1000ull, &off) == 0,
           "the page straddling the end of the file was claimed as file-backed");
    expect(vibeos_elf_page_file_offset(&im, BASE + 0x2000ull, &off) == 0,
           "a page entirely inside the bss was claimed as file-backed");
}

static void case_file_offset_refuses_a_shared_page(void) {
    vibeos_elf_image_t im;
    uint64_t off = 0;

    /* Two segments in one page: no single file offset describes it, and the
     * page must carry the permissions of both. */
    memset(g_img, 0, sizeof(g_img));
    build_ehdr(g_img, 2, BASE, 2);
    build_phdr(g_img, 0, PF_R | PF_X, 0x1000, BASE, 0x80, 0x80, 0x1000);
    build_phdr(g_img, 1, PF_R | PF_W, 0x1080, BASE + 0x80ull, 0x40, 0x40, 0x1000);

    if (!expect(vibeos_elf_parse_ex(g_img, sizeof(g_img), 0, BASE,
                                    BASE + 0x100000ull, 0, &im) == VIBEOS_ELF_OK,
                "two segments sharing a page were refused")) {
        return;
    }
    expect(vibeos_elf_page_file_offset(&im, BASE, &off) == 0,
           "a page covered by two segments was claimed as file-backed");
}

static void case_file_offset_refuses_a_hole(void) {
    vibeos_elf_image_t im;
    uint64_t off = 0;

    memset(g_img, 0, sizeof(g_img));
    build_ehdr(g_img, 2, BASE, 2);
    build_phdr(g_img, 0, PF_R | PF_X, 0x1000, BASE, 0x800, 0x800, 0x1000);
    build_phdr(g_img, 1, PF_R | PF_W, 0x2000, BASE + 0x10000ull, 0x40, 0x40, 0x1000);

    if (!expect(vibeos_elf_parse_ex(g_img, sizeof(g_img), 0, BASE,
                                    BASE + 0x100000ull, 0, &im) == VIBEOS_ELF_OK,
                "a two-segment image was refused")) {
        return;
    }
    expect(vibeos_elf_page_file_offset(&im, BASE + 0x8000ull, &off) == 0,
           "a page in a hole was claimed as file-backed");
}

static void case_file_offset_refuses_an_unaligned_segment(void) {
    vibeos_elf_image_t im;
    uint64_t off = 0;

    /* A segment starting part-way into a page: that page also carries the
     * bytes before it, which come from somewhere else in the file or from
     * nowhere at all. And a file offset that is not page-aligned cannot be
     * asked of a cache keyed on page-aligned offsets. */
    memset(g_img, 0, sizeof(g_img));
    build_ehdr(g_img, 2, BASE + 0x200ull, 1);
    build_phdr(g_img, 0, PF_R | PF_X, 0x1200, BASE + 0x200ull, 0x400, 0x400, 0x1000);

    if (!expect(vibeos_elf_parse_ex(g_img, sizeof(g_img), 0, BASE,
                                    BASE + 0x100000ull, 0, &im) == VIBEOS_ELF_OK,
                "a segment not starting on a page was refused")) {
        return;
    }
    expect(vibeos_elf_page_file_offset(&im, BASE, &off) == 0,
           "a page holding a segment that starts part-way in was claimed as file-backed");
}

/* ---- the startup block ---------------------------------------------------
 *
 * Decoded back and compared, rather than spot-checked. Every field here is one
 * a C runtime reads before the program's first line, so a builder that is
 * plausible but wrong produces a program that dies in the runtime with no
 * reference to the loader that mis-built its stack. */

#define STACK_BYTES 4096u
#define STACK_TOP 0x8000000000ull

static const uint64_t *auxv_find(const uint64_t *aux, uint64_t key) {
    while (aux[0] != VIBEOS_AT_NULL) {
        if (aux[0] == key) {
            return &aux[1];
        }
        aux += 2;
    }
    return 0;
}

static void case_startup_block(void) {
    static uint8_t buf[STACK_BYTES];
    static const uint8_t random16[16] = {
        1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16
    };
    const char *argv[] = { "/EFI/BOOT/DYN.ELF", "one", "two", 0 };
    const char *envp[] = { "PATH=/EFI/BOOT", "HOME=/", 0 };
    vibeos_elf_stack_desc_t sd;
    uint64_t sp;
    const uint64_t *words;
    const uint64_t *aux;
    const uint64_t *v;
    uint64_t argc;
    uint32_t i;

    memset(buf, 0, sizeof(buf));
    memset(&sd, 0, sizeof(sd));
    sd.argv = (const char *const *)argv;
    sd.envp = (const char *const *)envp;
    sd.entry = BASE + 0x1234ull;      /* the program's own entry */
    sd.phdr_vaddr = BASE + 0x40ull;
    sd.phnum = 9;
    sd.phentsize = (uint16_t)PHDR_SIZE;
    sd.random16 = random16;
    sd.interp_base = 0x424000ull;     /* an interpreter was mapped */

    sp = vibeos_elf_build_stack(buf, sizeof(buf), STACK_TOP, &sd);
    if (!expect(sp != 0ull, "the startup block did not fit")) {
        return;
    }
    expect((sp & 0xFull) == 0ull, "the stack pointer is not 16-byte aligned");

    /* Read it back the way a C runtime does: from the buffer, at the offset
     * the reported stack pointer names. */
    words = (const uint64_t *)(const void *)(buf + (sp - (STACK_TOP - sizeof(buf))));
    argc = words[0];
    expect(argc == 3ull, "argc is wrong");

    for (i = 0; i < 3u; i++) {
        const char *s = (const char *)(const void *)
            (buf + (words[1 + i] - (STACK_TOP - sizeof(buf))));
        if (!expect(strcmp(s, argv[i]) == 0, "an argv string came back wrong")) {
            break;
        }
    }
    expect(words[1 + argc] == 0ull, "argv is not NULL-terminated");

    for (i = 0; i < 2u; i++) {
        const char *s = (const char *)(const void *)
            (buf + (words[2 + argc + i] - (STACK_TOP - sizeof(buf))));
        if (!expect(strcmp(s, envp[i]) == 0, "an envp string came back wrong")) {
            break;
        }
    }
    expect(words[2 + argc + 2] == 0ull, "envp is not NULL-terminated");

    aux = &words[2 + argc + 3];

    /* The rule a dynamic program depends on, and which was a comment until
     * now: when an interpreter starts the process, AT_ENTRY stays the
     * *program's* entry. That is how the interpreter knows where to jump when
     * it has finished relocating. Writing the interpreter's entry here makes
     * the interpreter re-enter itself, which looks like a hang. */
    v = auxv_find(aux, VIBEOS_AT_ENTRY);
    if (expect(v != 0, "AT_ENTRY is missing")) {
        expect(*v == BASE + 0x1234ull,
               "AT_ENTRY is not the program's own entry");
    }
    v = auxv_find(aux, VIBEOS_AT_BASE);
    if (expect(v != 0, "AT_BASE is missing for a program with an interpreter")) {
        expect(*v == 0x424000ull, "AT_BASE is not where the interpreter was mapped");
    }
    v = auxv_find(aux, VIBEOS_AT_PHDR);
    if (expect(v != 0, "AT_PHDR is missing")) {
        expect(*v == BASE + 0x40ull, "AT_PHDR is wrong");
    }
    v = auxv_find(aux, VIBEOS_AT_PHNUM);
    if (expect(v != 0, "AT_PHNUM is missing")) {
        expect(*v == 9ull, "AT_PHNUM is wrong");
    }
    v = auxv_find(aux, VIBEOS_AT_PHENT);
    if (expect(v != 0, "AT_PHENT is missing")) {
        expect(*v == PHDR_SIZE, "AT_PHENT is wrong");
    }
    v = auxv_find(aux, VIBEOS_AT_RANDOM);
    if (expect(v != 0, "AT_RANDOM is missing")) {
        /* Not a diagnostic. A C library builds its stack canary from these
         * bytes, and a null pointer here kills the program in its own canary
         * setup - which is exactly how this was found. */
        const uint8_t *r = (const uint8_t *)(const void *)
            (buf + (*v - (STACK_TOP - sizeof(buf))));
        expect(memcmp(r, random16, 16) == 0, "AT_RANDOM does not point at the bytes given");
    }
}

static void case_no_interpreter_omits_at_base(void) {
    static uint8_t buf[STACK_BYTES];
    const char *argv[] = { "prog", 0 };
    vibeos_elf_stack_desc_t sd;
    uint64_t sp;
    const uint64_t *words;

    /* AT_BASE is omitted for a static program rather than written as zero:
     * zero is a legal load address and a runtime would believe it. */
    memset(buf, 0, sizeof(buf));
    memset(&sd, 0, sizeof(sd));
    sd.argv = (const char *const *)argv;
    sd.entry = BASE;
    sd.interp_base = 0;

    sp = vibeos_elf_build_stack(buf, sizeof(buf), STACK_TOP, &sd);
    if (!expect(sp != 0ull, "a minimal startup block did not fit")) {
        return;
    }
    words = (const uint64_t *)(const void *)(buf + (sp - (STACK_TOP - sizeof(buf))));
    expect(auxv_find(&words[1 + 1 + 1 + 1], VIBEOS_AT_BASE) == 0,
           "AT_BASE was written for a program with no interpreter");
}

static void case_arguments_that_do_not_fit(void) {
    static uint8_t buf[256];
    static char big[512];
    const char *argv[] = { big, 0 };
    vibeos_elf_stack_desc_t sd;

    /* Refused, not truncated. A stack built past the end of the page is a
     * write into whatever is mapped below it. */
    memset(big, 'x', sizeof(big) - 1u);
    big[sizeof(big) - 1u] = 0;
    memset(buf, 0, sizeof(buf));
    memset(&sd, 0, sizeof(sd));
    sd.argv = (const char *const *)argv;
    sd.entry = BASE;

    expect(vibeos_elf_build_stack(buf, sizeof(buf), STACK_TOP, &sd) == 0ull,
           "a startup block larger than its buffer was built anyway");
}

int test_loader(void) {
    g_fail = 0;

    case_two_segments_with_a_hole();
    case_bss_tail();
    case_fill_page_zeroes_beyond_the_file();
    case_two_segments_sharing_a_page();
    case_outside_the_allowed_range();
    case_truncated_is_refused_not_parsed();
    case_dyn_is_biased_once();
    case_dyn_refused_when_not_allowed();
    case_not_an_elf();
    case_file_offset_plain_page();
    case_file_offset_refuses_the_bss_tail();
    case_file_offset_refuses_a_shared_page();
    case_file_offset_refuses_a_hole();
    case_file_offset_refuses_an_unaligned_segment();
    case_startup_block();
    case_no_interpreter_omits_at_base();
    case_arguments_that_do_not_fit();

    return g_fail ? -1 : 0;
}
