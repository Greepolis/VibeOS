/* The Rtl runtime's process-facing parts on the host (docs/abi/windows.md W2):
 * handle tables, PE headers, environment blocks, critical sections, DOS-to-NT
 * path conversion and DbgPrint. The process parameters they read are a byte block
 * (ntrtl_host.c) laid out with include/vibeos/nt_layout.h's offsets. */

#define _GNU_SOURCE
#include <stdarg.h>
#include <stddef.h>
#include <stdlib.h>

#include "ntrtl_test.h"

#define ST_BAD ((vibeos_ntstatus_t)VIBEOS_STATUS_INVALID_PARAMETER)

/* ---- handle tables ------------------------------------------------------------------------ */

static void t_handles(void) {
    size_t before = heap_free_bytes();
    vibeos_rtl_handle_table_t t;
    vibeos_rtl_handle_t *h[40], *got;
    uint32_t idx, i;

    CHECK(sizeof(t) == 0x30, "RTL_HANDLE_TABLE is 0x30 bytes");
    memset(&t, 0xCC, sizeof(t));
    RtlInitializeHandleTable(40, 24, &t);
    CHECK(t.MaxHandleCount == 40 && t.HandleSize == 24 && t.FreeHandles == 0 && t.CommittedHandles == 0 &&
              t.UnCommittedHandles == 0 && t.MaxReservedHandles == 0 && t.Reserved[0] == 0,
          "initialising fills in the counts and clears everything else");
    CHECK(heap_free_bytes() == before, "an unused table owns no memory");
    CHECK(RtlIsValidIndexHandle(&t, 0, &got) == 0, "nothing is valid in a table with no entries");

    for (i = 0; i < 40; i++) {
        idx = 0xFFFF;
        h[i] = RtlAllocateHandle(&t, &idx);
        CHECK(h[i] != 0 && idx == i, "allocation %u gives index %u (got %u)", i, i, idx);
        if (!h[i]) {
            return;
        }
        CHECK((uint8_t *)h[i] == (uint8_t *)t.CommittedHandles + (size_t)i * 24u,
              "entry %u is at base + %u * HandleSize", i, i);
    }
    CHECK(RtlAllocateHandle(&t, &idx) == 0, "the 41st allocation of a 40-entry table fails");
    CHECK(RtlAllocateHandle(&t, 0) == 0, "and again, without an index");

    /* The caller marks an entry allocated; until then it is not valid. */
    CHECK(RtlIsValidIndexHandle(&t, 3, &got) == 0, "an entry nobody marked is not valid");
    CHECK(RtlFreeHandle(&t, h[3]) == 0, "and cannot be freed");
    h[3]->Next = (vibeos_rtl_handle_t *)(uintptr_t)VIBEOS_RTL_HANDLE_ALLOCATED;
    got = 0;
    CHECK(RtlIsValidIndexHandle(&t, 3, &got) == 1 && got == h[3], "marked allocated: valid, and the entry is returned");
    CHECK(RtlIsValidIndexHandle(&t, 3, 0) == 1, "the handle pointer is optional");
    CHECK(RtlIsValidIndexHandle(&t, 40, &got) == 0 && RtlIsValidIndexHandle(&t, 0xFFFFFFFFu, &got) == 0,
          "an index past the end is not valid");
    CHECK(RtlFreeHandle(&t, (vibeos_rtl_handle_t *)((uint8_t *)h[3] + 8)) == 0,
          "a pointer into the middle of an entry cannot be freed");
    {
        vibeos_rtl_handle_t outside;

        outside.Next = (vibeos_rtl_handle_t *)(uintptr_t)1;
        CHECK(RtlFreeHandle(&t, &outside) == 0, "an entry of another table cannot be freed");
    }

    memset((uint8_t *)h[3] + 8, 0xAA, 16);
    CHECK(RtlFreeHandle(&t, h[3]) == 1, "freeing an allocated entry returns TRUE");
    CHECK(RtlIsValidIndexHandle(&t, 3, &got) == 0, "which is then not valid");
    CHECK(RtlFreeHandle(&t, h[3]) == 0, "and cannot be freed twice");
    {
        int clean = 1;

        for (i = 8; i < 24; i++) {
            clean &= ((uint8_t *)h[3])[i] == 0;
        }
        CHECK(clean, "a freed entry's contents are cleared");
    }
    got = RtlAllocateHandle(&t, &idx);
    CHECK(got == h[3] && idx == 3, "the freed entry is the next one allocated (index %u)", idx);
    CHECK(((uint8_t *)got)[8] == 0 && ((uintptr_t)got->Next & 1) == 0, "and comes back clean and not allocated");

    CHECK(RtlDestroyHandleTable(&t) == 0, "destroy returns STATUS_SUCCESS");
    CHECK(t.CommittedHandles == 0 && heap_free_bytes() == before, "and gives the reservation back");

    /* An entry that cannot hold the free-list link, and a table of none. */
    RtlInitializeHandleTable(8, 4, &t);
    CHECK(RtlAllocateHandle(&t, &idx) == 0, "entries too small to chain are refused");
    RtlInitializeHandleTable(0, 16, &t);
    CHECK(RtlAllocateHandle(&t, &idx) == 0, "a table of no entries gives none");
    /* More entries than one commit run, in a table too big to be full at once. */
    RtlInitializeHandleTable(100000, 16, &t);
    for (i = 0; i < 1000; i++) {
        got = RtlAllocateHandle(&t, &idx);
        if (!got || idx != i) {
            break;
        }
    }
    CHECK(i == 1000, "1000 allocations from a big table run on in order (stopped at %u)", i);
    RtlDestroyHandleTable(&t);
    RtlInitializeHandleTable(0x10000000u, 0x1000, &t);
    CHECK(RtlAllocateHandle(&t, &idx) == 0, "a reservation larger than the arena fails cleanly");
    RtlDestroyHandleTable(&t);
    CHECK(heap_free_bytes() == before, "nothing leaked");
}

/* ---- PE images ----------------------------------------------------------------------------- */

/* A minimal image written here: headers, a .text and a .data section, and two
 * data directories whose RVAs lie in different places. */
static uint8_t g_img[0x2000];

static void put16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }
static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

static void build_image(uint32_t lfanew, int pe32plus) {
    uint8_t *nt = g_img + lfanew, *opt = nt + 24, *sec;

    memset(g_img, 0, sizeof(g_img));
    put16(g_img, 0x5A4D);
    put32(g_img + 0x3C, lfanew);
    put32(nt, 0x4550);
    put16(nt + 4, 0x8664);
    put16(nt + 6, 2);                      /* sections */
    put16(nt + 20, pe32plus ? 240 : 224);  /* SizeOfOptionalHeader */
    put16(opt, pe32plus ? 0x20B : 0x10B);
    put32(opt + 60, 0x200);                /* SizeOfHeaders */
    put32(opt + (pe32plus ? 108 : 92), 16);
    /* directory 1: in .text's range, size 0x30; directory 2: in the headers;
     * directory 3: in .data; directory 4: points nowhere any section covers. */
    {
        uint8_t *dir = opt + (pe32plus ? 112 : 96);

        put32(dir + 1 * 8, 0x1100);
        put32(dir + 1 * 8 + 4, 0x30);
        put32(dir + 2 * 8, 0x80);
        put32(dir + 2 * 8 + 4, 0x10);
        put32(dir + 3 * 8, 0x3010);
        put32(dir + 3 * 8 + 4, 0x20);
        put32(dir + 4 * 8, 0x9000);
        put32(dir + 4 * 8 + 4, 0x8);
    }
    sec = opt + (pe32plus ? 240 : 224);
    memcpy(sec, ".text", 6);
    put32(sec + 8, 0x400);                 /* VirtualSize */
    put32(sec + 12, 0x1000);               /* VirtualAddress */
    put32(sec + 16, 0x200);                /* SizeOfRawData */
    put32(sec + 20, 0x400);                /* PointerToRawData */
    sec += 40;
    memcpy(sec, ".data", 6);
    put32(sec + 8, 0x100);
    put32(sec + 12, 0x3000);
    put32(sec + 16, 0x200);
    put32(sec + 20, 0x800);
}

static void t_image_synthetic(void) {
    vibeos_image_nt_headers64_t *h;
    uint32_t sz;

    build_image(0x80, 1);
    h = RtlImageNtHeader(g_img);
    CHECK(h == (vibeos_image_nt_headers64_t *)(g_img + 0x80), "RtlImageNtHeader returns base + e_lfanew");
    CHECK(h && h->Signature == 0x4550 && h->OptionalHeader.Magic == 0x20B && h->FileHeader.NumberOfSections == 2,
          "and the structure reads through it");

    CHECK(RtlImageNtHeader(0) == 0, "NULL: no headers");
    g_img[0] = 'Z';
    CHECK(RtlImageNtHeader(g_img) == 0, "no MZ: no headers");
    g_img[0] = 'M';
    g_img[0x80] = 'X';
    CHECK(RtlImageNtHeader(g_img) == 0, "no PE signature: no headers");
    g_img[0x80] = 'P';
    CHECK(RtlImageNtHeader(g_img) != 0, "restored, they are found again");
    put32(g_img + 0x3C, 0x10000000u);
    CHECK(RtlImageNtHeader(g_img) == 0, "an e_lfanew of 256 MiB or more is refused");
    put32(g_img + 0x3C, 0xFFFFFFFFu);
    CHECK(RtlImageNtHeader(g_img) == 0, "and so is -1");
    build_image(0x1E00, 1);
    CHECK(RtlImageNtHeader(g_img) == (vibeos_image_nt_headers64_t *)(g_img + 0x1E00), "a far header is found");

    build_image(0x80, 1);
    sz = 0xDEAD;
    CHECK(RtlImageDirectoryEntryToData(g_img, 1, 1, &sz) == g_img + 0x1100 && sz == 0x30,
          "mapped as an image: base + RVA, and the size");
    sz = 0xDEAD;
    CHECK(RtlImageDirectoryEntryToData(g_img, 0, 1, &sz) == g_img + 0x400 + 0x100 && sz == 0x30,
          "as a file: RVA 0x1100 is 0x100 into .text, whose bytes start at 0x400");
    CHECK(RtlImageDirectoryEntryToData(g_img, 0, 3, &sz) == g_img + 0x800 + 0x10 && sz == 0x20,
          "as a file, in the second section");
    CHECK(RtlImageDirectoryEntryToData(g_img, 0, 2, &sz) == g_img + 0x80 && sz == 0x10,
          "as a file, an RVA in the headers is an offset from the start");
    sz = 0xDEAD;
    CHECK(RtlImageDirectoryEntryToData(g_img, 0, 4, &sz) == 0, "as a file, an RVA no section covers has no address");
    CHECK(RtlImageDirectoryEntryToData(g_img, 1, 4, &sz) == (void *)((uintptr_t)g_img + 0x9000), "though mapped it is just base + RVA");
    sz = 0xDEAD;
    CHECK(RtlImageDirectoryEntryToData(g_img, 1, 0, &sz) == 0 && sz == 0xDEAD,
          "an empty directory has no address, and the size is left alone");
    CHECK(RtlImageDirectoryEntryToData(g_img, 1, 16, &sz) == 0 && RtlImageDirectoryEntryToData(g_img, 1, 0xFFFF, &sz) == 0,
          "a directory index past the table has none");
    CHECK(RtlImageDirectoryEntryToData(g_img, 1, 1, 0) == g_img + 0x1100, "the size pointer is optional");
    put32(g_img + 0x80 + 24 + 108, 3);
    CHECK(RtlImageDirectoryEntryToData(g_img, 1, 3, &sz) == 0, "NumberOfRvaAndSizes bounds the directories");
    CHECK(RtlImageDirectoryEntryToData(g_img, 1, 2, &sz) != 0, "inside the bound they are found");
    g_img[0] = 0;
    CHECK(RtlImageDirectoryEntryToData(g_img, 1, 1, &sz) == 0, "not an image: no directories");

    build_image(0x80, 0);
    CHECK(RtlImageDirectoryEntryToData(g_img, 1, 1, &sz) == g_img + 0x1100 && sz == 0x30,
          "a PE32 image keeps its directories 16 bytes earlier, and is read there");
    CHECK(RtlImageDirectoryEntryToData(g_img, 0, 3, &sz) == g_img + 0x810, "also as a file");
    put32(g_img + 0x80 + 24 + 92, 3);
    CHECK(RtlImageDirectoryEntryToData(g_img, 1, 3, &sz) == 0 && RtlImageDirectoryEntryToData(g_img, 1, 2, &sz) != 0,
          "a PE32 image's NumberOfRvaAndSizes is read at its own offset and bounds the directories");
    put16(g_img + 0x80 + 24, 0x1234);
    CHECK(RtlImageDirectoryEntryToData(g_img, 1, 1, &sz) == 0, "an optional header of an unknown kind has none");
}

static uint8_t *slurp(const char *name, size_t *len) {
    char path[512];
    FILE *f;
    uint8_t *b;

    snprintf(path, sizeof(path), "%s/%s", VIBEOS_PE_FIXTURE_DIR, name);
    f = fopen(path, "rb");
    if (!f) {
        return 0;
    }
    fseek(f, 0, SEEK_END);
    *len = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    b = (uint8_t *)malloc(*len + 1);
    if (b && fread(b, 1, *len, f) != *len) {
        free(b);
        b = 0;
    }
    if (b) {
        b[*len] = 0;
    }
    fclose(f);
    return b;
}

/* The same functions on an image mingw-w64's linker wrote, with objdump's account
 * of it - the directories' RVAs and sizes from -p, the sections' addresses and
 * file offsets from -h - as the answer neither the code nor this test produced. */
static void t_image_mingw(void) {
#ifdef VIBEOS_PE_FIXTURE_DIR
    size_t flen = 0, hlen = 0, plen = 0;
    uint8_t *file = slurp("pe_fixture.exe", &flen);
    char *htxt = (char *)slurp("pe_fixture.h.txt", &hlen);
    char *ptxt = (char *)slurp("pe_fixture.p.txt", &plen);
    struct { uint64_t rva, size, off; } sec[16];
    struct { uint32_t rva, size; } dir[16];
    int nsec = 0, ndir = 0, i;
    uint64_t image_base = 0, entry = 0, size_of_image = 0;
    char *line, *save;
    vibeos_image_nt_headers64_t *nt;
    uint8_t *mapped;

    if (!file || !htxt || !ptxt) {
        printf("  ntrtl: the mingw-w64 fixture's files were not found; RtlImageNtHeader is tested on images "
               "written here only\n");
        free(file);
        free(htxt);
        free(ptxt);
        return;
    }
    memset(dir, 0, sizeof(dir));
    for (line = strtok_r(htxt, "\n", &save); line; line = strtok_r(0, "\n", &save)) {
        int idx;
        char name[32];
        unsigned long long size, vma, lma, off;

        if (sscanf(line, " %d %31s %llx %llx %llx %llx", &idx, name, &size, &vma, &lma, &off) == 6 && nsec < 16) {
            sec[nsec].rva = vma;          /* still with the base on */
            sec[nsec].size = size;
            sec[nsec].off = off;
            nsec++;
        }
    }
    for (line = strtok_r(ptxt, "\n", &save); line; line = strtok_r(0, "\n", &save)) {
        unsigned long long v;
        unsigned e, sz;
        unsigned long long rva;

        if (sscanf(line, "ImageBase %llx", &v) == 1) image_base = v;
        if (sscanf(line, "AddressOfEntryPoint %llx", &v) == 1) entry = v;
        if (sscanf(line, "SizeOfImage %llx", &v) == 1) size_of_image = v;
        if (sscanf(line, "Entry %x %llx %x", &e, &rva, &sz) == 3 && e < 16) {
            dir[e].rva = (uint32_t)rva;
            dir[e].size = sz;
            ndir++;
        }
    }
    CHECK(nsec >= 5 && ndir == 16 && image_base != 0, "objdump's sections and directories were read (%d, %d)", nsec, ndir);
    for (i = 0; i < nsec; i++) {
        sec[i].rva -= image_base;
    }

    nt = RtlImageNtHeader(file);
    CHECK(nt != 0, "RtlImageNtHeader finds the headers of mingw-w64's image");
    if (nt) {
        CHECK(nt->OptionalHeader.ImageBase == image_base && nt->OptionalHeader.AddressOfEntryPoint == entry &&
                  nt->OptionalHeader.SizeOfImage == size_of_image && nt->OptionalHeader.Magic == 0x20B &&
                  nt->FileHeader.NumberOfSections == nsec,
              "ImageBase, entry point, SizeOfImage and section count agree with objdump");
    }

    /* The image as the loader would map it: headers, then each section at its RVA. */
    mapped = (uint8_t *)calloc(1, (size_t)size_of_image);
    memcpy(mapped, file, 0x400);
    for (i = 0; i < nsec; i++) {
        if (sec[i].off != 0 && sec[i].off + sec[i].size <= flen) {
            memcpy(mapped + sec[i].rva, file + sec[i].off, (size_t)sec[i].size);
        }
    }
    for (i = 0; i < 16; i++) {
        uint32_t sz = 0xDEAD;
        void *flat = RtlImageDirectoryEntryToData(file, 0, (uint16_t)i, &sz);
        void *map = RtlImageDirectoryEntryToData(mapped, 1, (uint16_t)i, &sz);
        int s;

        if (dir[i].rva == 0) {
            CHECK(flat == 0 && map == 0, "directory %d is empty in the image: no address either way", i);
            continue;
        }
        CHECK(map == mapped + dir[i].rva, "directory %d mapped: base + RVA 0x%x", i, dir[i].rva);
        for (s = 0; s < nsec; s++) {
            if (dir[i].rva >= sec[s].rva && dir[i].rva < sec[s].rva + sec[s].size) {
                CHECK(flat == file + sec[s].off + (dir[i].rva - sec[s].rva),
                      "directory %d in the file: section %d's offset + %llu", i, s,
                      (unsigned long long)(dir[i].rva - sec[s].rva));
                break;
            }
        }
        sz = 0xDEAD;
        (void)RtlImageDirectoryEntryToData(file, 0, (uint16_t)i, &sz);
        CHECK(sz == dir[i].size, "directory %d: size 0x%x as objdump says (got 0x%x)", i, dir[i].size, sz);
        if (flat && map) {
            CHECK(!memcmp(flat, map, dir[i].size), "directory %d: the same bytes in the file and in the mapped image", i);
        }
    }
    free(mapped);
    free(file);
    free(htxt);
    free(ptxt);
#else
    printf("  ntrtl: mingw-w64 fixture absent (x86_64-w64-mingw32-gcc not found): PE reading is tested only "
           "on images this file writes\n");
#endif
}

/* ---- environment ---------------------------------------------------------------------------- */

static void set_process_env(vibeos_wchar_t *e) {
    uint64_t v = (uint64_t)(uintptr_t)e;

    memcpy(g_host_params + VIBEOS_RUPP_ENVIRONMENT, &v, 8);
}

static vibeos_wchar_t *get_process_env(void) {
    uint64_t v;

    memcpy(&v, g_host_params + VIBEOS_RUPP_ENVIRONMENT, 8);
    return (vibeos_wchar_t *)(uintptr_t)v;
}

/* True if the block is exactly spec, whose '|' stand for the NULs between strings;
 * the block's own terminating NUL follows the last. */
static int block_is(const vibeos_wchar_t *e, const char *spec) {
    size_t i, n = strlen(spec);

    if (!e) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        if (e[i] != (spec[i] == '|' ? 0 : (unsigned char)spec[i])) {
            return 0;
        }
    }
    return e[n] == 0;
}

static vibeos_ntstatus_t setvar(vibeos_wchar_t **env, const char *name, const char *value) {
    vibeos_wchar_t nb[64], vb[64];
    vibeos_ustring_t n, v;

    ustr(&n, nb, name);
    if (!value) {
        return RtlSetEnvironmentVariable(env, &n, 0);
    }
    ustr(&v, vb, value);
    return RtlSetEnvironmentVariable(env, &n, &v);
}

static vibeos_ntstatus_t getvar(vibeos_wchar_t *env, const char *name, vibeos_wchar_t *out, unsigned cap_bytes,
                                uint16_t *len) {
    vibeos_wchar_t nb[64];
    vibeos_ustring_t n, v;
    vibeos_ntstatus_t st;

    ustr(&n, nb, name);
    v.Buffer = out;
    v.Length = 0xFFFF;
    v.MaximumLength = (uint16_t)cap_bytes;
    st = RtlQueryEnvironmentVariable_U(env, &n, &v);
    *len = v.Length;
    return st;
}

static void t_environment(void) {
    size_t before = heap_free_bytes();
    vibeos_wchar_t *env = 0, *old, out[32];
    vibeos_ntstatus_t st;
    uint16_t len;
    void *prev;

    memset(g_host_params, 0, sizeof(g_host_params));
    g_host_params_present = 1;

    /* The process has no block: every query comes back "not found". */
    st = getvar(0, "PATH", out, sizeof(out), &len);
    CHECK(st == (vibeos_ntstatus_t)VIBEOS_STATUS_VARIABLE_NOT_FOUND, "no environment: STATUS_VARIABLE_NOT_FOUND (%08x)",
          (unsigned)st);

    CHECK(RtlCreateEnvironment(0, &env) == 0 && env && env[0] == 0 && env[1] == 0, "a new block is empty and doubly terminated");
    CHECK(getvar(env, "A", out, sizeof(out), &len) == (vibeos_ntstatus_t)VIBEOS_STATUS_VARIABLE_NOT_FOUND,
          "an empty block has no variables");
    CHECK(RtlCreateEnvironment(0, 0) == ST_BAD, "a NULL result pointer is refused");

    CHECK(setvar(&env, "Path", "C:\\bin") == 0 && block_is(env, "Path=C:\\bin|"), "one variable");
    CHECK(setvar(&env, "TEMP", "C:\\tmp") == 0 && block_is(env, "Path=C:\\bin|TEMP=C:\\tmp|"), "a second goes on the end");
    CHECK(setvar(&env, "PATH", "D:\\x") == 0 && block_is(env, "Path=D:\\x|TEMP=C:\\tmp|"),
          "setting an existing name (any case) replaces its value where it stands, keeping the name as first written");
    CHECK(setvar(&env, "e", "") == 0 && block_is(env, "Path=D:\\x|TEMP=C:\\tmp|e=|"), "an empty value is a value");
    CHECK(setvar(&env, "=C:", "C:\\dir") == 0 && block_is(env, "Path=D:\\x|TEMP=C:\\tmp|e=|=C:=C:\\dir|"),
          "a name may begin with '='");

    memset(out, 0xEE, sizeof(out));
    st = getvar(env, "path", out, sizeof(out), &len);
    CHECK(st == 0 && len == 8 && out[0] == 'D' && out[3] == 'x' && out[4] == 0,
          "query: any case matches, Length is bytes without a terminator, a NUL follows when it fits");
    memset(out, 0xEE, sizeof(out));
    st = getvar(env, "Path", out, 8, &len);
    CHECK(st == 0 && len == 8 && out[3] == 'x' && out[4] == 0xEEEE, "a buffer of exactly Length bytes succeeds without the NUL");
    memset(out, 0xEE, sizeof(out));
    st = getvar(env, "Path", out, 6, &len);
    CHECK(st == (vibeos_ntstatus_t)VIBEOS_STATUS_BUFFER_TOO_SMALL && len == 8 && out[0] == 0xEEEE,
          "too small: STATUS_BUFFER_TOO_SMALL, Length says what is needed, nothing copied (%08x, %u)", (unsigned)st, len);
    st = getvar(env, "e", out, 0, &len);
    CHECK(st == 0 && len == 0, "an empty value fits an empty buffer");
    CHECK(getvar(env, "TE", out, sizeof(out), &len) == (vibeos_ntstatus_t)VIBEOS_STATUS_VARIABLE_NOT_FOUND &&
              getvar(env, "TEMPS", out, sizeof(out), &len) == (vibeos_ntstatus_t)VIBEOS_STATUS_VARIABLE_NOT_FOUND,
          "a name must match whole: neither a prefix nor a longer name");
    CHECK(getvar(env, "=C:", out, sizeof(out), &len) == 0 && len == 12, "the '='-named variable is found by its name");
    CHECK(getvar(env, "", out, sizeof(out), &len) == (vibeos_ntstatus_t)VIBEOS_STATUS_VARIABLE_NOT_FOUND,
          "an empty name is not found");
    {
        vibeos_ustring_t n;
        vibeos_wchar_t nb[4];

        ustr(&n, nb, "A");
        CHECK(RtlQueryEnvironmentVariable_U(env, 0, &n) == ST_BAD && RtlQueryEnvironmentVariable_U(env, &n, 0) == ST_BAD,
              "NULL name or value is refused");
    }

    CHECK(setvar(&env, "temp", 0) == 0 && block_is(env, "Path=D:\\x|e=|=C:=C:\\dir|"), "a NULL value deletes");
    CHECK(setvar(&env, "TEMP", 0) == (vibeos_ntstatus_t)VIBEOS_STATUS_VARIABLE_NOT_FOUND &&
              block_is(env, "Path=D:\\x|e=|=C:=C:\\dir|"),
          "deleting what is not there is STATUS_VARIABLE_NOT_FOUND and changes nothing");
    CHECK(setvar(&env, "A=B", "x") == ST_BAD && setvar(&env, "=", "x") == ST_BAD && setvar(&env, "AB=", "x") == ST_BAD,
          "a name with '=' in it, but not first, is refused");
    CHECK(setvar(&env, "", "x") == ST_BAD, "an empty name is refused");
    {
        vibeos_ustring_t v;
        vibeos_wchar_t vb[4];

        ustr(&v, vb, "x");
        CHECK(RtlSetEnvironmentVariable(&env, 0, &v) == ST_BAD, "a NULL name is refused");
    }
    CHECK(block_is(env, "Path=D:\\x|e=|=C:=C:\\dir|"), "refusals changed nothing");

    /* The process's own block, through the parameters. */
    set_process_env(env);
    env = 0;
    st = getvar(0, "path", out, sizeof(out), &len);
    CHECK(st == 0 && len == 8, "with Environment NULL a query reads the process's block");
    CHECK(setvar(0, "NEW", "1") == 0 && block_is(get_process_env(), "Path=D:\\x|e=|=C:=C:\\dir|NEW=1|"),
          "and a set changes the process's block, and stores the new one in the parameters");
    CHECK(setvar(0, "NEW", 0) == 0 && block_is(get_process_env(), "Path=D:\\x|e=|=C:=C:\\dir|"), "a delete too");

    /* Inheriting copies it. */
    CHECK(RtlCreateEnvironment(1, &env) == 0 && env != get_process_env() && block_is(env, "Path=D:\\x|e=|=C:=C:\\dir|"),
          "an inherited block is a copy of the process's");
    setvar(&env, "COPY", "y");
    CHECK(block_is(get_process_env(), "Path=D:\\x|e=|=C:=C:\\dir|"), "changing the copy leaves the process's alone");

    /* Swapping the process's block for another. */
    prev = 0;
    RtlSetCurrentEnvironment(env, &prev);
    CHECK(get_process_env() == env && prev != env && block_is(prev, "Path=D:\\x|e=|=C:=C:\\dir|"),
          "RtlSetCurrentEnvironment installs the block and hands back the old one");
    old = prev;
    RtlSetCurrentEnvironment(old, 0);
    CHECK(get_process_env() == old, "the previous-pointer is optional");
    CHECK(RtlDestroyEnvironment(env) == 0, "destroy returns STATUS_SUCCESS");
    CHECK(RtlDestroyEnvironment(old) == 0, "also for the other");
    set_process_env(0);

    /* A process with no parameters at all. */
    g_host_params_present = 0;
    CHECK(setvar(0, "X", "1") == ST_BAD, "no parameters: nowhere to store the process's block");
    CHECK(getvar(0, "X", out, sizeof(out), &len) == (vibeos_ntstatus_t)VIBEOS_STATUS_VARIABLE_NOT_FOUND, "and nothing to find");
    prev = (void *)1;
    RtlSetCurrentEnvironment(0, &prev);
    CHECK(prev == 0, "RtlSetCurrentEnvironment reports no previous block");
    {
        vibeos_wchar_t *e2 = 0;

        CHECK(RtlCreateEnvironment(1, &e2) == 0 && e2[0] == 0 && e2[1] == 0, "inheriting from nothing is an empty block");
        RtlDestroyEnvironment(e2);
    }
    g_host_params_present = 1;
    memset(g_host_params, 0, sizeof(g_host_params));
    CHECK(heap_free_bytes() == before, "no environment block leaked");
}

/* ---- critical sections ------------------------------------------------------------------------ */

static void t_critsec(void) {
    size_t before = heap_free_bytes();
    vibeos_rtl_critical_section_t cs;
    void *me = (void *)(uintptr_t)0x1234, *other = (void *)(uintptr_t)0x5678;
    vibeos_ntstatus_t st;

    CHECK(sizeof(cs) == 40 && offsetof(vibeos_rtl_critical_section_t, LockCount) == 8 &&
              offsetof(vibeos_rtl_critical_section_t, RecursionCount) == 12 &&
              offsetof(vibeos_rtl_critical_section_t, OwningThread) == 16 &&
              offsetof(vibeos_rtl_critical_section_t, LockSemaphore) == 24 &&
              offsetof(vibeos_rtl_critical_section_t, SpinCount) == 32,
          "RTL_CRITICAL_SECTION is 40 bytes with Windows' offsets");
    CHECK(sizeof(vibeos_rtl_critical_section_debug_t) == 48, "its debug record is 48 bytes");

    g_host_thread = me;
    g_host_contention = 0;
    memset(&cs, 0x77, sizeof(cs));
    CHECK(RtlInitializeCriticalSection(&cs) == 0, "initialise returns STATUS_SUCCESS");
    CHECK(cs.LockCount == -1 && cs.RecursionCount == 0 && cs.OwningThread == 0 && cs.LockSemaphore == 0 &&
              cs.SpinCount == 0,
          "a new section is free: LockCount -1, no owner");
    CHECK(cs.DebugInfo && cs.DebugInfo->CriticalSection == &cs && cs.DebugInfo->ProcessLocksList.Flink ==
              &cs.DebugInfo->ProcessLocksList,
          "it has a debug record that points back at it");

    CHECK(RtlEnterCriticalSection(&cs) == 0 && cs.LockCount == 0 && cs.RecursionCount == 1 && cs.OwningThread == me,
          "first entry: LockCount 0, RecursionCount 1, owned by the calling thread");
    CHECK(RtlEnterCriticalSection(&cs) == 0 && cs.LockCount == 1 && cs.RecursionCount == 2 && cs.OwningThread == me,
          "the owner enters again: both counts go up");
    CHECK(RtlEnterCriticalSection(&cs) == 0 && cs.LockCount == 2 && cs.RecursionCount == 3, "and again");
    CHECK(RtlLeaveCriticalSection(&cs) == 0 && cs.LockCount == 1 && cs.RecursionCount == 2 && cs.OwningThread == me,
          "leaving once leaves it owned");
    CHECK(RtlLeaveCriticalSection(&cs) == 0 && cs.LockCount == 0 && cs.RecursionCount == 1 && cs.OwningThread == me,
          "and again");

    g_host_thread = other;
    st = RtlEnterCriticalSection(&cs);
    CHECK(g_host_contention == 1, "another thread entering a held section reaches the contention hook (%d)",
          g_host_contention);
    CHECK(st == (vibeos_ntstatus_t)VIBEOS_STATUS_NOT_IMPLEMENTED, "and, were the hook to return, is told so");
    CHECK(cs.LockCount == 0 && cs.RecursionCount == 1 && cs.OwningThread == me, "without disturbing the owner's state");

    g_host_thread = me;
    CHECK(RtlLeaveCriticalSection(&cs) == 0 && cs.LockCount == -1 && cs.RecursionCount == 0 && cs.OwningThread == 0,
          "the last leave frees it: LockCount -1, no owner");
    g_host_thread = other;
    CHECK(RtlEnterCriticalSection(&cs) == 0 && cs.OwningThread == other && g_host_contention == 1,
          "a free section is entered by anyone without contention");
    CHECK(RtlLeaveCriticalSection(&cs) == 0 && cs.LockCount == -1, "and left");

    CHECK(RtlDeleteCriticalSection(&cs) == 0, "delete returns STATUS_SUCCESS");
    CHECK(cs.DebugInfo == 0 && heap_free_bytes() == before, "and releases the debug record");
    g_host_thread = (void *)(uintptr_t)0x1234;
}

/* ---- DOS paths to NT paths ---------------------------------------------------------------------- */

static vibeos_wchar_t g_cwd_buf[512];

static void set_cwd(const char *s) {
    uint16_t len = 0, max = 0;
    uint64_t buf = 0;

    if (s) {
        wide(g_cwd_buf, s);
        len = (uint16_t)(strlen(s) * 2u);
        max = (uint16_t)(len + 2u);
        buf = (uint64_t)(uintptr_t)g_cwd_buf;
    }
    memcpy(g_host_params + VIBEOS_RUPP_CURDIR_PATH, &len, 2);
    memcpy(g_host_params + VIBEOS_RUPP_CURDIR_PATH + 2, &max, 2);
    memcpy(g_host_params + VIBEOS_RUPP_CURDIR_PATH + 8, &buf, 8);
}

static int ws_is(const vibeos_wchar_t *w, size_t n, const char *s) {
    size_t i;

    if (strlen(s) != n) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        if (w[i] != (unsigned char)s[i]) {
            return 0;
        }
    }
    return 1;
}

typedef struct {
    const char *cwd;       /* NULL: no current directory */
    const char *dos;
    const char *nt;        /* NULL: the conversion fails */
    const char *file;      /* the file part, or NULL if there is none */
} pathcase_t;

static void t_paths(void) {
    static const pathcase_t cases[] = {
        {"C:\\Dir\\Sub", "C:\\x", "\\??\\C:\\x", "x"},
        {"C:\\Dir\\Sub", "c:\\a\\b.txt", "\\??\\c:\\a\\b.txt", "b.txt"},
        {"C:\\Dir\\Sub", "C:/a/b", "\\??\\C:\\a\\b", "b"},
        {"C:\\Dir\\Sub", "C:\\a\\.\\b\\..\\c", "\\??\\C:\\a\\c", "c"},
        {"C:\\Dir\\Sub", "C:\\a\\..\\..\\b", "\\??\\C:\\b", "b"},
        {"C:\\Dir\\Sub", "C:\\..", "\\??\\C:\\", 0},
        {"C:\\Dir\\Sub", "C:\\a\\\\b\\\\\\c", "\\??\\C:\\a\\b\\c", "c"},
        {"C:\\Dir\\Sub", "C:\\a\\", "\\??\\C:\\a\\", 0},
        {"C:\\Dir\\Sub", "C:\\a\\.", "\\??\\C:\\a", "a"},
        {"C:\\Dir\\Sub", "C:\\", "\\??\\C:\\", 0},
        {"C:\\Dir\\Sub", "C:\\a b\\c d.e", "\\??\\C:\\a b\\c d.e", "c d.e"},
        {"C:\\Dir\\Sub", "C:", "\\??\\C:\\Dir\\Sub", "Sub"},
        {"C:\\Dir\\Sub", "c:rel\\f", "\\??\\C:\\Dir\\Sub\\rel\\f", "f"},
        {"C:\\Dir\\Sub", "D:x", "\\??\\D:\\x", "x"},
        {"C:\\Dir\\Sub", "x.txt", "\\??\\C:\\Dir\\Sub\\x.txt", "x.txt"},
        {"C:\\Dir\\Sub\\", "x.txt", "\\??\\C:\\Dir\\Sub\\x.txt", "x.txt"},
        {"C:\\Dir\\Sub\\", "..\\y", "\\??\\C:\\Dir\\y", "y"},
        {"C:\\Dir\\Sub\\", "..\\..\\..\\y", "\\??\\C:\\y", "y"},
        {"C:\\Dir\\Sub", ".", "\\??\\C:\\Dir\\Sub", "Sub"},
        {"C:\\Dir\\Sub", "sub/dir/", "\\??\\C:\\Dir\\Sub\\sub\\dir\\", 0},
        {"C:\\Dir\\Sub", "\\z", "\\??\\C:\\z", "z"},
        {"C:\\Dir\\Sub", "/z/w", "\\??\\C:\\z\\w", "w"},
        {"E:\\", "x", "\\??\\E:\\x", "x"},
        {"\\\\srv\\sh\\d", "\\x", "\\??\\UNC\\srv\\sh\\x", "x"},
        {"\\\\srv\\sh\\d", "y\\z", "\\??\\UNC\\srv\\sh\\d\\y\\z", "z"},
        {"\\\\srv\\sh\\d", "..\\..\\..\\q", "\\??\\UNC\\srv\\sh\\q", "q"},
        {"C:\\Dir", "\\\\server\\share\\dir\\f", "\\??\\UNC\\server\\share\\dir\\f", "f"},
        {"C:\\Dir", "//server/share/f", "\\??\\UNC\\server\\share\\f", "f"},
        {"C:\\Dir", "\\\\server\\share", "\\??\\UNC\\server\\share", "share"},
        {"C:\\Dir", "\\\\server\\share\\a\\..\\..\\b", "\\??\\UNC\\server\\share\\b", "b"},
        {"C:\\Dir", "\\\\?\\C:\\a\\..\\b", "\\??\\C:\\a\\..\\b", "b"},
        {"C:\\Dir", "\\\\?\\C:\\a/b\\.", "\\??\\C:\\a/b\\.", "."},
        {"C:\\Dir", "\\\\?\\UNC\\s\\sh\\f", "\\??\\UNC\\s\\sh\\f", "f"},
        {"C:\\Dir", "\\\\?\\", "\\??\\", 0},
        {"C:\\Dir", "\\??\\C:\\x\\y", "\\??\\C:\\x\\y", "y"},
        {"C:\\Dir", "\\\\.\\COM1", "\\??\\COM1", 0},
        {"C:\\Dir", "\\\\.\\PIPE\\a\\..\\b", "\\??\\PIPE\\b", "b"},
        {0, "a", "\\??\\C:\\a", "a"},
        {0, "..\\a", "\\??\\C:\\a", "a"},
        {0, "\\a", "\\??\\C:\\a", "a"},
        {0, "C:", "\\??\\C:\\", 0},
        {"", "a", "\\??\\C:\\a", "a"},
        {"C:\\Dir", "", 0, 0},
        {"C:\\Dir", "x", "\\??\\C:\\Dir\\x", "x"},
    };
    unsigned i;
    size_t before = heap_free_bytes();

    g_host_params_present = 1;
    memset(g_host_params, 0, sizeof(g_host_params));
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const pathcase_t *c = &cases[i];
        vibeos_wchar_t in[512];
        vibeos_ustring_t nt;
        vibeos_wchar_t *fp = (vibeos_wchar_t *)(uintptr_t)1;
        vibeos_rtl_relative_name_u_t rel;
        vibeos_boolean_t ok;

        set_cwd(c->cwd);
        memset(&nt, 0xEE, sizeof(nt));
        memset(&rel, 0xFF, sizeof(rel));
        ok = RtlDosPathNameToNtPathName_U(wide(in, c->dos), &nt, &fp, &rel);
        if (!c->nt) {
            CHECK(ok == 0, "\"%s\": the conversion fails", c->dos);
            continue;
        }
        CHECK(ok == 1, "\"%s\" (cwd %s): converts", c->dos, c->cwd ? c->cwd : "none");
        if (!ok) {
            continue;
        }
        CHECK(ws_is(nt.Buffer, nt.Length / 2u, c->nt), "\"%s\" (cwd %s) -> expected \"%s\", got length %u first %04x",
              c->dos, c->cwd ? c->cwd : "none", c->nt, nt.Length, nt.Buffer[0]);
        if (!ws_is(nt.Buffer, nt.Length / 2u, c->nt)) {
            char got[300];
            size_t k;

            for (k = 0; k < nt.Length / 2u && k < 299; k++) got[k] = (char)nt.Buffer[k];
            got[k] = 0;
            printf("    got \"%s\"\n", got);
        }
        CHECK(nt.Length % 2 == 0 && nt.MaximumLength == nt.Length + 2 && nt.Buffer[nt.Length / 2u] == 0,
              "\"%s\": Length %u, MaximumLength %u, and a NUL after the text", c->dos, nt.Length, nt.MaximumLength);
        if (c->file) {
            CHECK(fp && fp >= nt.Buffer && fp < nt.Buffer + nt.Length / 2u &&
                      ws_is(fp, (size_t)(nt.Buffer + nt.Length / 2u - fp), c->file),
                  "\"%s\": the file part is \"%s\"", c->dos, c->file);
        } else {
            CHECK(fp == 0, "\"%s\": there is no file part", c->dos);
        }
        CHECK(rel.RelativeName.Buffer == 0 && rel.RelativeName.Length == 0 && rel.ContainingDirectory == 0 &&
                  rel.CurDirRef == 0,
              "\"%s\": the relative name is zeroed", c->dos);
        RtlFreeUnicodeString(&nt);
    }
    CHECK(heap_free_bytes() == before, "RtlFreeUnicodeString gives every result buffer back");

    {
        vibeos_wchar_t in[40];
        vibeos_ustring_t nt;
        static vibeos_wchar_t huge[40000];
        size_t k;

        set_cwd("C:\\Dir");
        CHECK(RtlDosPathNameToNtPathName_U(0, &nt, 0, 0) == 0, "NULL: fails");
        CHECK(RtlDosPathNameToNtPathName_U(wide(in, "C:\\a"), &nt, 0, 0) == 1, "FilePart and RelativeName are optional");
        RtlFreeUnicodeString(&nt);
        for (k = 0; k < 39999; k++) huge[k] = 'a';
        huge[0] = 'C'; huge[1] = ':'; huge[2] = '\\';
        huge[39999] = 0;
        CHECK(RtlDosPathNameToNtPathName_U(huge, &nt, 0, 0) == 0, "a path that cannot be a UNICODE_STRING fails");
        for (k = 0; k < 16370; k++) huge[k] = 'a';
        huge[0] = 'C'; huge[1] = ':'; huge[2] = '\\';
        huge[16370] = 0;
        CHECK(RtlDosPathNameToNtPathName_U(huge, &nt, 0, 0) == 1 && nt.Length == (16370 + 4) * 2,
              "one that just fits does (%u)", nt.Length);
        RtlFreeUnicodeString(&nt);
    }
    g_host_params_present = 0;
    {
        vibeos_wchar_t in[40];
        vibeos_ustring_t nt;

        CHECK(RtlDosPathNameToNtPathName_U(wide(in, "q"), &nt, 0, 0) == 1 && ws_is(nt.Buffer, nt.Length / 2u, "\\??\\C:\\q"),
              "a process with no parameters is on C:\\");
        RtlFreeUnicodeString(&nt);
    }
    g_host_params_present = 1;
    memset(g_host_params, 0, sizeof(g_host_params));
    CHECK(heap_free_bytes() == before, "nothing leaked");
}

/* ---- DbgPrint --------------------------------------------------------------------------------- */

static void dbg_reset(void) {
    g_host_dbg_len = 0;
    g_host_dbg[0] = 0;
    g_host_dbg_writes = 0;
}

static int dbg_is(const char *s) {
    return strcmp(g_host_dbg, s) == 0;
}

static uint32_t call_v(uint32_t comp, uint32_t lvl, const char *fmt, ...) {
    va_list ap;
    uint32_t st;

    va_start(ap, fmt);
    st = vDbgPrintEx(comp, lvl, fmt, ap);
    va_end(ap);
    return st;
}

static uint32_t call_vp(const char *prefix, const char *fmt, ...) {
    va_list ap;
    uint32_t st;

    va_start(ap, fmt);
    st = vDbgPrintExWithPrefix(prefix, 3, 1, fmt, ap);
    va_end(ap);
    return st;
}

#define DBG(expect, ...)                                                      \
    do {                                                                      \
        dbg_reset();                                                          \
        DbgPrint(__VA_ARGS__);                                                \
        CHECK(dbg_is(expect), "%s -> \"%s\", want \"%s\"", #__VA_ARGS__, g_host_dbg, expect); \
    } while (0)

static void t_dbgprint(void) {
    vibeos_wchar_t w1[8], w2[8];
    int i;

    dbg_reset();
    CHECK(DbgPrint("hello %s %d\n", "w", -5) == 0, "DbgPrint returns STATUS_SUCCESS");
    CHECK(dbg_is("hello w -5\n") && g_host_dbg_writes == 1, "one write holding the formatted text");

    DBG("42", "%d", 42);
    DBG("-2147483648", "%d", (int)0x80000000);
    DBG("4294967295", "%u", 0xFFFFFFFFu);
    DBG("ff FF 000000ff 0xff", "%x %X %08x %#x", 255, 255, 255, 255);
    DBG("deadbeef", "%lx", 0xDEADBEEFu);
    DBG("4294967295", "%lu", 0xFFFFFFFFu);
    DBG("-9000000000", "%lld", (long long)-9000000000ll);
    DBG("18446744073709551615", "%I64u", (unsigned long long)~0ull);
    DBG("5000000000", "%zu", (size_t)5000000000ull);
    DBG("-5000000000", "%Id", (long long)-5000000000ll);
    DBG("123456789abcdef0", "%llx", 0x123456789abcdef0ull);
    DBG("0000000000001234", "%p", (void *)(uintptr_t)0x1234);
    DBG("0000000000000000", "%p", (void *)0);
    DBG("FFFFFFFFFFFFFFFF", "%p", (void *)(uintptr_t)~(uintptr_t)0);
    DBG("ab", "%c%c", 'a', 'b');
    DBG("Z", "%C", (int)L'Z');
    DBG("?", "%lc", 0x4E2D);
    DBG("\xE9", "%c", 0xE9);
    DBG("hi", "%s", "hi");
    DBG("(null)", "%s", (char *)0);
    DBG("(null)", "%S", (vibeos_wchar_t *)0);
    for (i = 0; i < 4; i++) {
        w1[i] = (vibeos_wchar_t)"wide"[i];
    }
    w1[4] = 0;
    DBG("wide", "%S", w1);
    DBG("wide", "%ls", w1);
    DBG("wide", "%ws", w1);
    DBG("narrow", "%hs", "narrow");
    DBG("narrow", "%hS", "narrow");
    w2[0] = 'a';
    w2[1] = 0x4E2D;
    w2[2] = 0xE9;
    w2[3] = 0;
    DBG("a?\xE9", "%ls", w2);
    DBG("[   42]", "[%5d]", 42);
    DBG("[42   ]", "[%-5d]", 42);
    DBG("[-0042]", "[%05d]", -42);
    DBG("[+7]", "[%+d]", 7);
    DBG("[ 7]", "[% d]", 7);
    DBG("[abc]", "[%.3s]", "abcdef");
    DBG("[     abc]", "[%8.3s]", "abcdef");
    DBG("[abc     ]", "[%-8.3s]", "abcdef");
    DBG("[     7]", "[%*d]", 6, 7);
    DBG("[7     ]", "[%*d]", -6, 7);
    DBG("[abc]", "[%.*s]", 3, "abcdef");
    DBG("[00042]", "[%.5d]", 42);
    DBG("[   00042]", "[%8.5d]", 42);
    DBG("[]", "[%.0d]", 0);
    DBG("[4464]", "[%hd]", 70000);
    DBG("[1]", "[%hhu]", 257);
    DBG("[10]", "[%o]", 8);
    wide(w2, "ab");
    DBG("[ab    ]", "[%-6ls]", w2);
    DBG("[    ab]", "[%6ls]", w2);
    DBG("100%", "100%%");
    DBG("%q", "%q");
    DBG("a   b", "a%3sb", "");
    DBG("[0]", "[%#x]", 0);

    /* The 512-byte limit: 511 characters and the terminator. */
    {
        char longfmt[700];

        memset(longfmt, 'x', 600);
        longfmt[600] = 0;
        dbg_reset();
        DbgPrint(longfmt);
        CHECK(g_host_dbg_len == 511, "a long message is cut at 511 characters (%zu)", g_host_dbg_len);
        dbg_reset();
        DbgPrint("%s", longfmt);
        CHECK(g_host_dbg_len == 511, "also when it comes from an argument (%zu)", g_host_dbg_len);
    }

    dbg_reset();
    CHECK(DbgPrintEx(77, 3, "c=%d l=%u", 77, 3) == 0 && dbg_is("c=77 l=3"),
          "DbgPrintEx writes whatever the component and level");
    dbg_reset();
    CHECK(call_v(0xFFFFFFFFu, 0xFFFFFFFFu, "v %s %d", "args", 9) == 0 && dbg_is("v args 9"), "vDbgPrintEx takes a va_list");
    dbg_reset();
    CHECK(call_vp("PFX: ", "n=%d", 4) == 0 && dbg_is("PFX: n=4"), "vDbgPrintExWithPrefix puts the prefix first");
    dbg_reset();
    CHECK(call_vp(0, "n=%d", 4) == 0 && dbg_is("n=4"), "and a NULL prefix is none");
    dbg_reset();
    CHECK(DbgPrint(0) == 0 && g_host_dbg_len == 0, "a NULL format writes nothing");
}

void ntrtl_run_system(void) {
    t_handles();
    t_image_synthetic();
    t_image_mingw();
    t_environment();
    t_critsec();
    t_paths();
    t_dbgprint();
}
