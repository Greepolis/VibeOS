/* Host tests for the PE32+ parser (kernel/core/pe.c, docs/abi/windows.md W1).
 *
 * Two kinds of image. Ones this file writes, to reach every refusal - and those
 * can only prove the parser agrees with this file's reading of the format, the
 * ISO9660 lesson in CLAUDE.md. So one image is mingw-w64's own
 * (tests/win/pe_fixture.c), and what the parser says of it is compared with
 * what mingw's objdump says of it: the description neither side of the test
 * wrote. CMake builds both when the cross compiler is present; when it is not,
 * the test says so on a line of its own rather than passing quietly. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vibeos/pe.h"
#include "vibeos/elf.h"
#include "vibeos/mbz.h"

int test_pe(void);

static int g_fail;

static void expect(int cond, const char *what) {
    if (!cond) {
        printf("FAIL:pe %s\n", what);
        g_fail = 1;
    }
}

static void w16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void w32(uint8_t *p, uint32_t v) { w16(p, v); w16(p + 2, v >> 16); }
static void w64(uint8_t *p, uint64_t v) { w32(p, (uint32_t)v); w32(p + 4, (uint32_t)(v >> 32)); }

/* ---- an image written here ----------------------------------------------------- */

#define T_BASE 0x8000000000ull
#define T_OPT 0x58u              /* PE signature at 0x40, COFF header after it  */
#define T_OPT_SIZE 240u          /* the fixed part and sixteen directories      */
#define T_SECT (T_OPT + T_OPT_SIZE)
#define T_HDRS 0x200u
#define T_FILE 0x800u

typedef struct {
    uint32_t vsize, va, raw, raw_at, ch;
} t_sec_t;

static uint8_t g_img[T_FILE];

/* Headers for `n` sections, every field what a linker writes; a test breaks the
 * one it is about afterwards. */
static void t_build(const t_sec_t *s, uint32_t n) {
    uint32_t i;

    memset(g_img, 0xE7, sizeof(g_img));   /* hostile background, not zeros */
    memset(g_img, 0, T_HDRS);
    g_img[0] = 'M';
    g_img[1] = 'Z';
    w32(g_img + 0x3C, 0x40);
    memcpy(g_img + 0x40, "PE\0\0", 4);
    w16(g_img + 0x44, 0x8664);              /* machine                   */
    w16(g_img + 0x46, n);                   /* sections                  */
    w16(g_img + 0x54, T_OPT_SIZE);          /* optional header size      */
    w16(g_img + 0x56, 0x0022);              /* executable, large address */
    w16(g_img + T_OPT, 0x20B);              /* PE32+                     */
    w32(g_img + T_OPT + 16, 0x1000);        /* entry                     */
    w64(g_img + T_OPT + 24, T_BASE);
    w32(g_img + T_OPT + 32, 0x1000);        /* section alignment         */
    w32(g_img + T_OPT + 36, 0x200);         /* file alignment            */
    w32(g_img + T_OPT + 56, 0x4000);        /* size of image             */
    w32(g_img + T_OPT + 60, T_HDRS);
    w16(g_img + T_OPT + 68, 3);             /* console                   */
    w64(g_img + T_OPT + 72, 0x200000);
    w64(g_img + T_OPT + 80, 0x1000);
    w32(g_img + T_OPT + 108, 16);
    w32(g_img + T_OPT + 112 + 8 * VIBEOS_PE_DIR_IMPORT, 0x2010);
    w32(g_img + T_OPT + 112 + 8 * VIBEOS_PE_DIR_IMPORT + 4, 0x28);
    for (i = 0; i < n; i++) {
        uint8_t *e = g_img + T_SECT + i * 40u;
        memcpy(e, i == 0 ? ".text\0\0\0" : ".data\0\0\0", 8);
        w32(e + 8, s[i].vsize);
        w32(e + 12, s[i].va);
        w32(e + 16, s[i].raw);
        w32(e + 20, s[i].raw_at);
        w32(e + 36, s[i].ch);
    }
}

/* .text: 0x30 bytes of code at 0x1000. .data: 0x1800 bytes in memory of which
 * 0x200 are in the file, from 0x2000 - so it runs a page and a half into what
 * is .bss to a linker. */
static const t_sec_t g_two[2] = {
    { 0x30u, 0x1000u, 0x200u, 0x200u, 0x60000020u },
    { 0x1800u, 0x2000u, 0x200u, 0x400u, 0xC0000040u },
};

static int t_parse(uint64_t len, vibeos_pe_image_t *out) {
    return vibeos_pe_parse(g_img, len, len, 0x400000u, 0x10000000000ull, out);
}

static void t_good(void) {
    vibeos_pe_image_t pe;

    t_build(g_two, 2);
    expect(vibeos_pe_is_pe(g_img, 2) == 1, "MZ is a PE image");
    expect(vibeos_pe_header_extent(g_img, T_FILE) == T_SECT + 80u,
           "the header extent ends after the section table");
    expect(t_parse(T_FILE, &pe) == VIBEOS_PE_OK, "a linker's headers parse");
    expect(pe.image_base == T_BASE && pe.image_size == 0x4000u &&
           pe.entry == T_BASE + 0x1000u && pe.subsystem == 3u,
           "base, size, absolute entry and subsystem");
    expect(pe.dirs == 16u && pe.dir_rva[VIBEOS_PE_DIR_IMPORT] == 0x2010u &&
           pe.dir_size[VIBEOS_PE_DIR_IMPORT] == 0x28u, "the import directory");
    expect(pe.layout.count == 3u, "headers and two sections");
    expect(pe.layout.seg[0].vaddr == T_BASE && pe.layout.seg[0].filesz == T_HDRS &&
           pe.layout.seg[0].flags == VIBEOS_ELF_R, "the headers are mapped read-only");
    expect(pe.layout.seg[1].vaddr == T_BASE + 0x1000u && pe.layout.seg[1].filesz == 0x30u &&
           pe.layout.seg[1].memsz == 0x30u && pe.layout.seg[1].file_off == 0x200u &&
           pe.layout.seg[1].flags == (VIBEOS_ELF_R | VIBEOS_ELF_X),
           "code: its virtual size, not the padded raw size, and executable");
    expect(pe.layout.seg[2].filesz == 0x200u && pe.layout.seg[2].memsz == 0x1800u &&
           pe.layout.seg[2].flags == (VIBEOS_ELF_R | VIBEOS_ELF_W),
           "data: the file's part and the zero tail");
    expect(pe.layout.min_vaddr == T_BASE && pe.layout.end_vaddr == T_BASE + 0x4000u &&
           pe.layout.is_dyn == 0u && pe.layout.has_interp == 0u, "the span, fixed");
}

/* The mapper's own fill, on the parser's layout: the bytes land where the
 * sections say and the tail is zero, over a background that is not. */
static void t_fill(void) {
    vibeos_pe_image_t pe;
    static uint8_t page[4096];
    uint32_t k;
    int ok = 1;

    t_build(g_two, 2);
    for (k = 0; k < 0x200u; k++) {
        g_img[0x400u + k] = (uint8_t)(k * 7u + 1u);
    }
    expect(t_parse(T_FILE, &pe) == VIBEOS_PE_OK, "parse for the fill");
    vibeos_elf_fill_page(&pe.layout, g_img, T_BASE + 0x2000u, page);
    for (k = 0; k < 0x200u; k++) {
        ok &= page[k] == (uint8_t)(k * 7u + 1u);
    }
    for (k = 0x200u; k < 4096u; k++) {
        ok &= page[k] == 0u;
    }
    expect(ok, "a data page holds the file's bytes then zeros");
    vibeos_elf_fill_page(&pe.layout, g_img, T_BASE + 0x3000u, page);
    ok = 1;
    for (k = 0; k < 4096u; k++) {
        ok &= page[k] == 0u;
    }
    expect(ok && vibeos_elf_page_flags(&pe.layout, T_BASE + 0x3000u) ==
                     (VIBEOS_ELF_R | VIBEOS_ELF_W), "the .bss page is zero and writable");
    vibeos_elf_fill_page(&pe.layout, g_img, T_BASE, page);
    expect(page[0] == 'M' && page[1] == 'Z' && page[0x200] == 0u,
           "the header page is the headers, and zero past SizeOfHeaders");
}

static void t_refusals(void) {
    vibeos_pe_image_t pe;
    t_sec_t s[2];
    uint64_t before;

    t_build(g_two, 2);
    g_img[0] = 'X';
    expect(t_parse(T_FILE, &pe) == VIBEOS_PE_ENOTPE, "no MZ");
    t_build(g_two, 2);
    g_img[0x41] = 'X';
    expect(t_parse(T_FILE, &pe) == VIBEOS_PE_ENOTPE, "no PE signature");
    t_build(g_two, 2);
    w16(g_img + 0x44, 0x14C);
    expect(t_parse(T_FILE, &pe) == VIBEOS_PE_EMACHINE, "i386 is not this machine");
    t_build(g_two, 2);
    w16(g_img + T_OPT, 0x10B);
    expect(t_parse(T_FILE, &pe) == VIBEOS_PE_EMACHINE, "PE32 is not PE32+");
    t_build(g_two, 2);
    w32(g_img + T_OPT + 32, 0x200);
    expect(t_parse(T_FILE, &pe) == VIBEOS_PE_EALIGN, "sections below a page");
    t_build(g_two, 2);
    expect(vibeos_pe_parse(g_img, 0x100u, T_FILE, 0x400000u, 0x10000000000ull, &pe) ==
               VIBEOS_PE_ETRUNCATED, "a window shorter than the section table");
    t_build(g_two, 2);
    expect(vibeos_pe_parse(g_img, T_FILE, T_FILE, T_BASE + 0x1000u, 0x10000000000ull,
                           &pe) == VIBEOS_PE_ERANGE &&
           vibeos_pe_parse(g_img, T_FILE, T_FILE, 0x400000u, T_BASE + 0x3000u,
                           &pe) == VIBEOS_PE_ERANGE,
           "an image not inside the window is refused, not relocated");
    expect(pe.layout.count == 0u, "a refusal leaves no layout behind");

    /* The defects no linker writes: counted as must-be-zero. */
    before = vibeos_mbz_count(VIBEOS_MBZ_PE_MALFORMED);
    memcpy(s, g_two, sizeof(s));
    s[1].va = 0x1000u;   /* on top of .text */
    t_build(s, 2);
    expect(t_parse(T_FILE, &pe) == VIBEOS_PE_EMALFORMED, "overlapping sections");
    memcpy(s, g_two, sizeof(s));
    s[1].vsize = 0x3000u;   /* past SizeOfImage */
    t_build(s, 2);
    expect(t_parse(T_FILE, &pe) == VIBEOS_PE_EMALFORMED, "a section past the image");
    t_build(g_two, 2);
    w32(g_img + T_OPT + 16, 0x4000u);
    expect(t_parse(T_FILE, &pe) == VIBEOS_PE_EMALFORMED, "an entry past the image");
    t_build(g_two, 2);
    w32(g_img + T_OPT + 60, 0x100u);
    expect(t_parse(T_FILE, &pe) == VIBEOS_PE_EMALFORMED,
           "SizeOfHeaders that does not cover the section table");
    t_build(g_two, 2);
    w32(g_img + T_OPT + 108, 17u);
    expect(t_parse(T_FILE, &pe) == VIBEOS_PE_EMALFORMED, "seventeen directories");
    t_build(g_two, 2);
    w16(g_img + 0x46, 40u);
    expect(t_parse(T_FILE, &pe) == VIBEOS_PE_ETOOMANY ||
           t_parse(T_FILE, &pe) == VIBEOS_PE_ETRUNCATED, "forty sections");
    expect(vibeos_mbz_count(VIBEOS_MBZ_PE_MALFORMED) >= before + 5u,
           "each malformed image is counted as must-be-zero");
    /* Raw data past the end of the file: short, not malformed. */
    memcpy(s, g_two, sizeof(s));
    s[1].raw_at = T_FILE - 0x100u;
    t_build(s, 2);
    expect(t_parse(T_FILE, &pe) == VIBEOS_PE_ETRUNCATED, "raw data past the file");
    /* VirtualSize 0 is the raw size, and no permission at all is readable. */
    memcpy(s, g_two, sizeof(s));
    s[1].vsize = 0u;
    s[1].ch = 0u;
    t_build(s, 2);
    expect(t_parse(T_FILE, &pe) == VIBEOS_PE_OK && pe.layout.seg[2].memsz == 0x200u &&
           pe.layout.seg[2].flags == VIBEOS_ELF_R,
           "VirtualSize 0 is the raw size; a section with no permission is readable");
}

/* ---- base relocation (W2) ----------------------------------------------------------- */

typedef struct {
    const uint8_t *b;
    uint64_t len;
} t_mem_t;

static int t_mem_read(void *ctx, uint64_t off, uint32_t len, void *buf) {
    const t_mem_t *m = (const t_mem_t *)ctx;

    if (off + len > m->len) {
        return -1;
    }
    memcpy(buf, m->b + off, len);
    return 0;
}

/* The image `pe` describes, a page at a time as the kernel fills it, with its
 * relocations applied; into `out`, which spans the image. */
static void t_fill_image(const vibeos_pe_image_t *pe, const uint8_t *file, uint64_t flen,
                         const uint8_t *rel, uint32_t rlen, uint8_t *out) {
    t_mem_t m;
    uint64_t va;

    m.b = file;
    m.len = flen;
    for (va = pe->layout.min_vaddr; va < pe->layout.end_vaddr; va += 4096u) {
        uint8_t *page = out + (va - pe->layout.min_vaddr);
        vibeos_elf_fill_page_via(&pe->layout, t_mem_read, &m, va, page);
        vibeos_pe_relocate_page(pe, rel, rlen, t_mem_read, &m, va, page);
    }
}

static uint64_t t_rd64(const uint8_t *p) {
    uint64_t v = 0;
    int i;
    for (i = 7; i >= 0; i--) {
        v = (v << 8) | p[i];
    }
    return v;
}

/* A .reloc section at 0x3000 of this file's image: the page at RVA 0x2000 has
 * a value at 0x10, one that straddles into the next page at 0xFFC, and an
 * ABSOLUTE pad. */
static uint8_t g_rel_file[0xA00];

static uint32_t t_build_reloc_image(void) {
    t_sec_t s[2];
    uint8_t *d, *r;
    uint32_t rlen;

    s[0] = g_two[0];
    s[1].vsize = 0x2000u;   /* .data: two pages, the second all zero tail */
    s[1].va = 0x2000u;
    s[1].raw = 0x400u;
    s[1].raw_at = 0x400u;
    s[1].ch = 0xC0000040u;
    t_build(s, 2);
    memset(g_rel_file, 0, sizeof(g_rel_file));
    memcpy(g_rel_file, g_img, 0x800u);
    w32(g_rel_file + T_OPT + 56, 0x5000u);   /* the image now ends after .reloc */
    d = g_rel_file + 0x400u;
    w64(d + 0x10u, T_BASE + 0x1000u);        /* a pointer to .text */
    w64(d + 0x3F0u, 0x1122334455667788ull);  /* listed: moves                  */
    w64(d + 0x3F8u, 0x0102030405060708ull);  /* not listed: must be left alone */
    /* The directory, appended to the file and named by the header. Its
     * section is not in the table: the parser does not need it mapped. */
    r = g_rel_file + 0x800u;
    w32(r, 0x2000u);
    w32(r + 4, 8u + 3u * 2u + 2u);          /* three entries and a pad */
    w16(r + 8, (10u << 12) | 0x010u);
    w16(r + 10, (10u << 12) | 0x3F0u);       /* a value at the end of the raw part */
    w16(r + 12, (10u << 12) | 0xFFCu);       /* straddles into the next page */
    w16(r + 14, 0);                          /* ABSOLUTE */
    rlen = 16u;
    w32(g_rel_file + T_OPT + 112 + 8 * VIBEOS_PE_DIR_BASERELOC, 0x4000u);
    w32(g_rel_file + T_OPT + 112 + 8 * VIBEOS_PE_DIR_BASERELOC + 4, rlen);
    return rlen;
}

static void t_relocation(void) {
    static uint8_t img[0x5000], ref[0x5000];
    vibeos_pe_image_t pe, at;
    const uint8_t *rel;
    uint32_t rlen = t_build_reloc_image();
    /* Past four gigabytes, so the distance changes the high half of every
     * value too: the half of a straddling value that lies on the next page is
     * then something a missed tail would leave visibly wrong. */
    const uint64_t to = T_BASE + 0x100800000ull;
    const int64_t delta = (int64_t)(to - T_BASE);
    uint8_t bad[16];

    rel = g_rel_file + 0x800u;
    expect(vibeos_pe_parse(g_rel_file, sizeof(g_rel_file), sizeof(g_rel_file), 0x400000u,
                           0x10000000000ull, &pe) == VIBEOS_PE_OK && pe.delta == 0 &&
           pe.preferred_base == T_BASE, "at its own base the image does not move");
    expect(vibeos_pe_parse_at(g_rel_file, sizeof(g_rel_file), sizeof(g_rel_file), to, 0x400000u,
                              0x10000000000ull, &at) == VIBEOS_PE_OK &&
           at.image_base == to && at.delta == delta && at.preferred_base == T_BASE &&
           at.entry == to + 0x1000u && at.layout.seg[1].vaddr == to + 0x1000u &&
           at.layout.min_vaddr == to, "placed elsewhere, every address is the new base's");
    expect(vibeos_pe_check_relocs(&at, rel, rlen) == VIBEOS_PE_OK, "a linker's relocations check");

    t_fill_image(&pe, g_rel_file, sizeof(g_rel_file), rel, rlen, ref);
    t_fill_image(&at, g_rel_file, sizeof(g_rel_file), rel, rlen, img);
    expect(t_rd64(img + 0x2010u) == to + 0x1000u, "a pointer moves with the image");
    expect(t_rd64(img + 0x23F0u) == 0x1122334455667788ull + (uint64_t)delta,
           "a value at the end of the file's part moves");
    expect(t_rd64(img + 0x2FFCu) == (uint64_t)delta,
           "a value straddling two pages - the second all .bss - moves, both halves");
    expect(t_rd64(ref + 0x2FFCu) == 0u && t_rd64(ref + 0x2010u) == T_BASE + 0x1000u,
           "at its own base nothing is touched");
    {
        uint32_t k, other = 0;
        for (k = 0; k < sizeof(img); k++) {
            int inside = (k >= 0x2010u && k < 0x2018u) || (k >= 0x23F0u && k < 0x23F8u) ||
                         (k >= 0x2FFCu && k < 0x3004u);
            other += !inside && img[k] != ref[k];
        }
        expect(other == 0u, "nothing but the listed values changes");
    }

    /* What no PE32+ linker writes. */
    memcpy(bad, rel, rlen);
    w16(bad + 8, (3u << 12) | 0x10u);   /* HIGHLOW: a 32-bit image's */
    expect(vibeos_pe_check_relocs(&at, bad, rlen) == VIBEOS_PE_EMALFORMED, "a HIGHLOW entry");
    memcpy(bad, rel, rlen);
    w32(bad + 4, 64u);
    expect(vibeos_pe_check_relocs(&at, bad, rlen) == VIBEOS_PE_EMALFORMED,
           "a block longer than the directory");
    memcpy(bad, rel, rlen);
    w32(bad, 0x5000u);
    expect(vibeos_pe_check_relocs(&at, bad, rlen) == VIBEOS_PE_EMALFORMED,
           "a block for a page outside the image");
    memcpy(bad, rel, rlen);
    w32(bad, 0x4000u);
    w16(bad + 8, (10u << 12) | 0xFFCu);
    expect(vibeos_pe_check_relocs(&at, bad, rlen) == VIBEOS_PE_EMALFORMED,
           "a value whose eight bytes run past the image's end");
    memcpy(bad, rel, rlen);
    w32(bad, 0x2010u);
    expect(vibeos_pe_check_relocs(&at, bad, rlen) == VIBEOS_PE_EMALFORMED,
           "a block for an address that is not a page");

    /* An image that cannot move is refused somewhere else, not placed there. */
    w16(g_rel_file + 0x56, 0x0023u);   /* RELOCS_STRIPPED */
    expect(vibeos_pe_parse_at(g_rel_file, sizeof(g_rel_file), sizeof(g_rel_file), to, 0x400000u,
                              0x10000000000ull, &at) == VIBEOS_PE_ERANGE,
           "relocations stripped: it can only be where it asks");
    t_build(g_two, 2);
    expect(vibeos_pe_parse_at(g_img, T_FILE, T_FILE, to, 0x400000u, 0x10000000000ull, &at) ==
               VIBEOS_PE_OK && at.delta == delta &&
               vibeos_pe_check_relocs(&at, 0, 0) == VIBEOS_PE_OK,
           "no relocation directory and none stripped: it moves, with nothing to fix");
}

/* ---- mingw-w64's image, against mingw-w64's objdump ------------------------------ */

#ifdef VIBEOS_PE_FIXTURE_DIR
static uint8_t *t_slurp(const char *name, long *len) {
    char path[512];
    FILE *f;
    uint8_t *b;

    snprintf(path, sizeof(path), "%s/%s", VIBEOS_PE_FIXTURE_DIR, name);
    f = fopen(path, "rb");
    if (!f) {
        return 0;
    }
    fseek(f, 0, SEEK_END);
    *len = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = (uint8_t *)malloc((size_t)*len + 1u);
    if (b && fread(b, 1, (size_t)*len, f) != (size_t)*len) {
        free(b);
        b = 0;
    }
    if (b) {
        b[*len] = 0;
    }
    fclose(f);
    return b;
}

/* objdump -p prints "Key<tabs>value" in hex. */
static uint64_t t_field(const char *text, const char *key) {
    const char *p = strstr(text, key);
    if (!p) {
        return ~0ull;
    }
    p += strlen(key);
    return strtoull(p, 0, 16);
}

static void t_mingw(void) {
    long elen = 0, hlen = 0, plen = 0;
    uint8_t *exe = t_slurp("pe_fixture.exe", &elen);
    char *h = (char *)t_slurp("pe_fixture.h.txt", &hlen);
    char *p = (char *)t_slurp("pe_fixture.p.txt", &plen);
    vibeos_pe_image_t pe;
    const char *line;
    uint32_t listed = 0, matched = 0, k;

    expect(exe && h && p, "the mingw fixture and objdump's answers were built");
    if (!exe || !h || !p) {
        return;
    }
    expect(vibeos_pe_parse(exe, (uint64_t)elen, (uint64_t)elen, 0x400000u,
                           0x10000000000ull, &pe) == VIBEOS_PE_OK, "mingw's image parses");
    expect(pe.image_base == t_field(p, "\nImageBase") &&
           pe.image_size == t_field(p, "\nSizeOfImage") &&
           pe.headers_size == t_field(p, "\nSizeOfHeaders") &&
           pe.entry - pe.image_base == t_field(p, "\nAddressOfEntryPoint") &&
           pe.subsystem == t_field(p, "\nSubsystem"),
           "base, size, headers, entry and subsystem as objdump -p reads them");
    /* Each section objdump -h lists: "  0 .text  size vma lma fileoff 2**n",
     * then a line of flags. */
    for (line = strstr(h, "\nIdx"); line && (line = strchr(line + 1, '\n')) != 0;) {
        unsigned idx;
        char name[32];
        unsigned long long size, vma, lma, off;
        const char *flags = strchr(line + 1, '\n');

        if (sscanf(line + 1, "%u %31s %llx %llx %llx %llx", &idx, name, &size, &vma,
                   &lma, &off) != 6 || !flags) {
            continue;
        }
        listed++;
        for (k = 1; k < pe.layout.count; k++) {
            const vibeos_elf_segment_t *g = &pe.layout.seg[k];
            const char *eol = strchr(flags + 1, '\n');
            int code = eol && strstr(flags, "CODE") && strstr(flags, "CODE") < eol;
            int ro = eol && strstr(flags, "READONLY") && strstr(flags, "READONLY") < eol;
            int has = eol && strstr(flags, "CONTENTS") && strstr(flags, "CONTENTS") < eol;

            if (g->vaddr != vma) {
                continue;
            }
            if (g->memsz == size && (!has || g->file_off == off) &&
                !!(g->flags & VIBEOS_ELF_X) == code && (!(g->flags & VIBEOS_ELF_W)) == ro) {
                matched++;
            } else {
                printf("FAIL:pe mingw section %s: memsz=%llx off=%llx flags=%u\n", name,
                       (unsigned long long)g->memsz, (unsigned long long)g->file_off,
                       g->flags);
            }
        }
    }
    expect(listed >= 4u, "objdump listed the fixture's sections");

    /* Moved, every value objdump says the linker listed - and nothing else -
     * changes by exactly the distance moved. */
    {
        vibeos_pe_image_t at;
        static uint8_t ref[0x40000], img[0x40000];
        uint64_t rva = pe.dir_rva[VIBEOS_PE_DIR_BASERELOC], k;
        uint32_t rlen = pe.dir_size[VIBEOS_PE_DIR_BASERELOC];
        static uint8_t rel[4096];
        const uint64_t to = pe.image_base + 0x800000u;
        uint32_t dir64 = 0, moved = 0, other = 0;
        const char *r;

        expect(rlen != 0u && rlen <= sizeof(rel) && pe.image_size <= sizeof(img),
               "the fixture has a relocation directory");
        if (rlen == 0u || rlen > sizeof(rel) || pe.image_size > sizeof(img)) {
            goto done;
        }
        /* The directory, read out of the image as the kernel will. */
        t_fill_image(&pe, exe, (uint64_t)elen, 0, 0, ref);
        memcpy(rel, ref + rva, rlen);
        expect(vibeos_pe_parse_at(exe, (uint64_t)elen, (uint64_t)elen, to, 0x400000u,
                                  0x10000000000ull, &at) == VIBEOS_PE_OK &&
               vibeos_pe_check_relocs(&at, rel, rlen) == VIBEOS_PE_OK,
               "mingw's image moves, and its relocations check");
        t_fill_image(&at, exe, (uint64_t)elen, rel, rlen, img);
        for (r = strstr(p, "\treloc"); r; r = strstr(r + 1, "\treloc")) {
            unsigned idx, off;
            unsigned long long where;
            char kind[16];

            if (sscanf(r, "\treloc %u offset %x [%llx] %15s", &idx, &off, &where, kind) != 4 ||
                strcmp(kind, "DIR64") != 0) {
                continue;
            }
            dir64++;
            moved += t_rd64(img + where) == t_rd64(ref + where) + (uint64_t)at.delta;
            memset(img + where, 0, 8);
            memset(ref + where, 0, 8);
        }
        for (k = 0; k < pe.image_size; k++) {
            other += img[k] != ref[k];
        }
        expect(dir64 >= 3u && moved == dir64,
               "every DIR64 objdump lists moved by exactly the distance");
        expect(other == 0u, "and no other byte of the image changed");
    }
done:
    expect(matched == listed && pe.layout.count == listed + 1u,
           "every section objdump lists, at its address, size, offset and permissions");
    free(exe);
    free(h);
    free(p);
}
#endif

int test_pe(void) {
    g_fail = 0;
    t_good();
    t_fill();
    t_refusals();
    t_relocation();
#ifdef VIBEOS_PE_FIXTURE_DIR
    t_mingw();
#else
    printf("pe: mingw-w64 fixture absent (x86_64-w64-mingw32-gcc not found): "
           "the parser is tested only against images this file writes\n");
#endif
    return g_fail ? -1 : 0;
}
