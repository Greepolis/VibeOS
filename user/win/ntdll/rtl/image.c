/* Reading a PE image that is already in memory (docs/abi/windows.md W2).
 *
 * Both functions trust the memory at Base to be at least as large as the headers
 * they walk, as Windows' do: the image is the caller's own mapping, and there is
 * no size to check it against. What they do check is every field they follow. */

#include "rtl_internal.h"

static uint16_t rd16(const uint8_t *p) {
    uint16_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static uint32_t rd32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

/* The NT headers' address, or NULL: the DOS header must say MZ, e_lfanew must
 * stay below 256 MiB (Windows refuses a larger offset as it refuses a missing
 * signature) and the signature there must say PE\0\0. A PE32 image is accepted
 * and its headers returned too - the pointer is valid, though the optional header
 * behind it is the 32-bit one. */
vibeos_image_nt_headers64_t *RtlImageNtHeader(void *base) {
    const uint8_t *b = (const uint8_t *)base;
    uint32_t lfanew;

    if (!b || rd16(b) != VIBEOS_IMAGE_DOS_SIGNATURE) {
        return 0;
    }
    lfanew = rd32(b + 0x3C);
    if (lfanew >= 0x10000000u) {
        return 0;
    }
    if (rd32(b + lfanew) != VIBEOS_IMAGE_NT_SIGNATURE) {
        return 0;
    }
    return (vibeos_image_nt_headers64_t *)(uintptr_t)(b + lfanew);
}

/* An RVA to an address in the image, or NULL. Mapped as an image, an RVA is an
 * offset from the base. As a file read flat into memory it is not: the section
 * holding it says where its bytes are, and an RVA in the headers (before the
 * first section) is still an offset from the start. */
static void *rva_to_va(const uint8_t *b, const uint8_t *nt, uint32_t size_of_headers, uint32_t rva,
                       int mapped) {
    uint16_t nsec = rd16(nt + 6);
    const uint8_t *sec = nt + 4 + 20 + rd16(nt + 20);
    uint16_t i;

    if (mapped) {
        return (void *)(uintptr_t)(b + rva);
    }
    if (rva < size_of_headers) {
        return (void *)(uintptr_t)(b + rva);
    }
    for (i = 0; i < nsec; i++, sec += sizeof(vibeos_image_section_header_t)) {
        uint32_t va = rd32(sec + 12), vsize = rd32(sec + 8), raw = rd32(sec + 16);
        uint32_t ptr = rd32(sec + 20);
        uint32_t span = vsize > raw ? vsize : raw;

        if (rva >= va && rva - va < span) {
            return (void *)(uintptr_t)(b + ptr + (rva - va));
        }
    }
    return 0;
}

void *RtlImageDirectoryEntryToData(void *base, vibeos_boolean_t mapped_as_image,
                                   uint16_t directory_entry, uint32_t *size) {
    const uint8_t *b = (const uint8_t *)base;
    const uint8_t *nt = (const uint8_t *)RtlImageNtHeader(base);
    const uint8_t *opt, *dir;
    uint32_t count, rva, hdrs;
    uint16_t magic;

    if (!nt) {
        return 0;
    }
    opt = nt + 4 + 20;
    magic = rd16(opt);
    if (magic == VIBEOS_IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        count = rd32(opt + 108);
        dir = opt + 112;
    } else if (magic == VIBEOS_IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        count = rd32(opt + 92);
        dir = opt + 96;
    } else {
        return 0;
    }
    hdrs = rd32(opt + 60);            /* SizeOfHeaders, at the same offset in both */
    if (directory_entry >= count || directory_entry >= 16u) {
        return 0;
    }
    rva = rd32(dir + (size_t)directory_entry * 8u);
    if (rva == 0) {
        return 0;
    }
    /* *size is written only for a directory that is there, as a caller that
     * ignores the return value will read it. */
    if (size) {
        *size = rd32(dir + (size_t)directory_entry * 8u + 4u);
    }
    return rva_to_va(b, nt, hdrs, rva, mapped_as_image != 0);
}
