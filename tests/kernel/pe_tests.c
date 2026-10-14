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
#ifdef VIBEOS_PE_FIXTURE_DIR
    t_mingw();
#else
    printf("pe: mingw-w64 fixture absent (x86_64-w64-mingw32-gcc not found): "
           "the parser is tested only against images this file writes\n");
#endif
    return g_fail ? -1 : 0;
}
