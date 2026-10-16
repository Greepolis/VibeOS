/* PE32+ program-image parser. See include/vibeos/pe.h for what it accepts and
 * why, and for why its answer is an ELF-shaped segment list.
 *
 * Fields are read byte by byte, as in elf.c: the PE header sits wherever
 * e_lfanew says, and nothing here should depend on that offset being aligned.
 * Every sum of values taken from the file is checked before it is used.
 */

#include "vibeos/pe.h"
#include "vibeos/mbz.h"

#define PE_MACHINE_AMD64 0x8664u
#define PE_MAGIC_PE32PLUS 0x20Bu
#define PE_COFF_SIZE 20u
/* The PE32+ optional header up to its data directories. */
#define PE_OPT_FIXED 112u
#define PE_SECTION_SIZE 40u

#define PE_SCN_EXECUTE 0x20000000u
#define PE_SCN_READ 0x40000000u
#define PE_SCN_WRITE 0x80000000u

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const uint8_t *p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

static int add_ok(uint64_t a, uint64_t b, uint64_t *out) {
    if (a > (uint64_t)0xFFFFFFFFFFFFFFFFull - b) {
        return 0;
    }
    *out = a + b;
    return 1;
}

static uint64_t page_down(uint64_t v) { return v & ~(uint64_t)(VIBEOS_ELF_PAGE_SIZE - 1u); }

static int page_up(uint64_t v, uint64_t *out) {
    uint64_t t;
    if (!add_ok(v, VIBEOS_ELF_PAGE_SIZE - 1u, &t)) {
        return 0;
    }
    *out = page_down(t);
    return 1;
}

int vibeos_pe_is_pe(const void *hdr, uint64_t hdr_len) {
    const uint8_t *b = (const uint8_t *)hdr;
    return hdr && hdr_len >= 2u && b[0] == 'M' && b[1] == 'Z';
}

/* Where the PE header starts, after checking the signature is there: 0 if not. */
static uint64_t pe_header_at(const uint8_t *b, uint64_t len) {
    uint64_t at;

    if (len < 0x40u || b[0] != 'M' || b[1] != 'Z') {
        return 0;
    }
    at = rd32(b + 0x3Cu);   /* e_lfanew */
    if (at < 0x40u || at > len || len - at < 4u + PE_COFF_SIZE) {
        return 0;
    }
    if (b[at] != 'P' || b[at + 1u] != 'E' || b[at + 2u] != 0 || b[at + 3u] != 0) {
        return 0;
    }
    return at;
}

uint64_t vibeos_pe_header_extent(const void *hdr, uint64_t hdr_len) {
    const uint8_t *b = (const uint8_t *)hdr;
    uint64_t at, opt, nsec, end;

    if (!hdr) {
        return 0;
    }
    at = pe_header_at(b, hdr_len);
    if (at == 0u) {
        return 0;
    }
    nsec = rd16(b + at + 4u + 2u);
    opt = rd16(b + at + 4u + 16u);
    /* All of this is at most 0xFFFF * 41 past a 32-bit offset: no overflow. */
    end = at + 4u + PE_COFF_SIZE + opt + nsec * PE_SECTION_SIZE;
    return end;
}

static int pe_parse_inner(const uint8_t *b, uint64_t hdr_len, uint64_t file_len,
                          uint64_t place, uint64_t min_allowed, uint64_t end_allowed,
                          vibeos_pe_image_t *out) {
    uint64_t at, coff, opt, sect, extent, end, image_end;
    uint32_t opt_size, section_align, i;
    uint16_t nsec;
    vibeos_elf_image_t *L = &out->layout;
    uint64_t last_end = 0;

    at = pe_header_at(b, hdr_len);
    if (at == 0u) {
        return (hdr_len >= 2u && b[0] == 'M' && b[1] == 'Z' && hdr_len < 0x40u)
                   ? VIBEOS_PE_ETRUNCATED : VIBEOS_PE_ENOTPE;
    }
    extent = vibeos_pe_header_extent(b, hdr_len);
    if (extent > hdr_len || extent > file_len) {
        return VIBEOS_PE_ETRUNCATED;
    }
    coff = at + 4u;
    if (rd16(b + coff) != PE_MACHINE_AMD64) {
        return VIBEOS_PE_EMACHINE;
    }
    nsec = rd16(b + coff + 2u);
    opt_size = rd16(b + coff + 16u);
    out->characteristics = rd16(b + coff + 18u);
    opt = coff + PE_COFF_SIZE;
    if (opt_size < 2u || rd16(b + opt) != PE_MAGIC_PE32PLUS) {
        return VIBEOS_PE_EMACHINE;
    }
    if (opt_size < PE_OPT_FIXED) {
        return VIBEOS_PE_EMALFORMED;
    }
    if (nsec > VIBEOS_PE_MAX_SECTIONS) {
        return VIBEOS_PE_ETOOMANY;
    }
    out->sections = nsec;
    out->image_base = rd64(b + opt + 24u);
    section_align = rd32(b + opt + 32u);
    out->image_size = rd32(b + opt + 56u);
    out->headers_size = rd32(b + opt + 60u);
    out->subsystem = rd16(b + opt + 68u);
    out->dll_characteristics = rd16(b + opt + 70u);
    out->stack_reserve = rd64(b + opt + 72u);
    out->stack_commit = rd64(b + opt + 80u);
    out->dirs = rd32(b + opt + 108u);
    if (out->dirs > VIBEOS_PE_MAX_DIRS ||
        PE_OPT_FIXED + (uint64_t)out->dirs * 8u > opt_size) {
        return VIBEOS_PE_EMALFORMED;
    }
    for (i = 0; i < out->dirs; i++) {
        out->dir_rva[i] = rd32(b + opt + PE_OPT_FIXED + i * 8u);
        out->dir_size[i] = rd32(b + opt + PE_OPT_FIXED + i * 8u + 4u);
    }
    if (section_align < VIBEOS_ELF_PAGE_SIZE ||
        (section_align & (VIBEOS_ELF_PAGE_SIZE - 1u)) != 0u) {
        return VIBEOS_PE_EALIGN;
    }
    /* The image itself: page-aligned where it starts, inside the window, its
     * headers inside it and inside the file. */
    if ((out->image_base & (VIBEOS_ELF_PAGE_SIZE - 1u)) != 0u ||
        out->image_size == 0u || out->headers_size == 0u ||
        out->headers_size > out->image_size || out->headers_size > file_len ||
        out->headers_size < extent) {
        return VIBEOS_PE_EMALFORMED;
    }
    if (!add_ok(out->image_base, out->image_size, &end) || !page_up(end, &image_end)) {
        return VIBEOS_PE_EMALFORMED;
    }
    /* Somewhere else, if the caller says where: the same span, moved. Only an
     * image that can be moved - its relocations kept, and some to apply - and
     * only to a page boundary, which is all a PE relocation can express. */
    out->preferred_base = out->image_base;
    if (place != 0u && place != out->image_base) {
        /* What decides is the flag, as on Windows: an image whose relocations
         * were not stripped and that has none to apply - a DLL with no
         * absolute address in it - moves with nothing to fix. */
        if ((place & (VIBEOS_ELF_PAGE_SIZE - 1u)) != 0u ||
            (out->characteristics & VIBEOS_PE_FILE_RELOCS_STRIPPED)) {
            return VIBEOS_PE_ERANGE;
        }
        out->image_base = place;
        if (!add_ok(place, out->image_size, &end) || !page_up(end, &image_end)) {
            return VIBEOS_PE_EMALFORMED;
        }
    }
    out->delta = (int64_t)(out->image_base - out->preferred_base);
    if (out->image_base < min_allowed || image_end > end_allowed) {
        return VIBEOS_PE_ERANGE;
    }
    {
        uint32_t entry_rva = rd32(b + opt + 16u);
        if (entry_rva >= out->image_size) {
            return VIBEOS_PE_EMALFORMED;
        }
        out->entry = entry_rva ? out->image_base + entry_rva : 0u;
    }

    /* The headers are mapped too, read-only: our own ntdll reads the export
     * and import directories through them, as every Windows loader does. */
    L->count = 0;
    L->seg[0].vaddr = out->image_base;
    L->seg[0].file_off = 0;
    L->seg[0].filesz = out->headers_size;
    L->seg[0].memsz = out->headers_size;
    L->seg[0].flags = VIBEOS_ELF_R;
    L->count = 1;
    last_end = out->headers_size;

    sect = opt + opt_size;
    for (i = 0; i < nsec; i++) {
        const uint8_t *s = b + sect + (uint64_t)i * PE_SECTION_SIZE;
        uint64_t vsize = rd32(s + 8u);
        uint64_t va = rd32(s + 12u);
        uint64_t raw = rd32(s + 16u);
        uint64_t raw_at = rd32(s + 20u);
        uint32_t ch = rd32(s + 36u);
        uint64_t filesz, memsz, sec_end, raw_end;
        vibeos_elf_segment_t *g;

        /* VirtualSize 0 means the raw size, as a linker that leaves it out
         * means; raw data past VirtualSize is padding, never mapped. */
        memsz = vsize ? vsize : raw;
        filesz = raw < memsz ? raw : memsz;
        if (memsz == 0u) {
            continue;   /* a section that occupies nothing */
        }
        if ((va & (section_align - 1u)) != 0u || va < last_end) {
            /* Out of order, overlapping the headers or the section before, or
             * not at its alignment: no linker writes that. */
            return VIBEOS_PE_EMALFORMED;
        }
        if (!add_ok(va, memsz, &sec_end) || sec_end > out->image_size) {
            return VIBEOS_PE_EMALFORMED;
        }
        if (filesz != 0u &&
            (!add_ok(raw_at, filesz, &raw_end) || raw_end > file_len)) {
            return VIBEOS_PE_ETRUNCATED;
        }
        g = &L->seg[L->count++];
        g->vaddr = out->image_base + va;
        g->file_off = filesz ? raw_at : 0u;
        g->filesz = filesz;
        g->memsz = memsz;
        g->flags = 0;
        if (ch & PE_SCN_READ) {
            g->flags |= VIBEOS_ELF_R;
        }
        if (ch & PE_SCN_WRITE) {
            g->flags |= VIBEOS_ELF_W;
        }
        if (ch & PE_SCN_EXECUTE) {
            g->flags |= VIBEOS_ELF_X;
        }
        if (g->flags == 0u) {
            /* Windows maps every section of an image readable; a page with no
             * permission at all would be a hole the mapper skips. */
            g->flags = VIBEOS_ELF_R;
        }
        last_end = sec_end;
    }
    L->entry = out->entry;
    L->min_vaddr = out->image_base;
    L->end_vaddr = image_end;
    L->image_span = image_end - out->image_base;
    L->load_bias = 0;
    L->phdr_vaddr = 0;
    L->phnum = 0;
    L->phentsize = 0;
    L->is_dyn = 0;
    L->has_interp = 0;
    L->interp[0] = 0;
    return VIBEOS_PE_OK;
}

static int pe_parse_placed(const void *hdr, uint64_t hdr_len, uint64_t file_len,
                           uint64_t place, uint64_t min_allowed, uint64_t end_allowed,
                           vibeos_pe_image_t *out) {
    uint32_t i;
    int rc;

    if (!hdr || !out) {
        return VIBEOS_PE_ENOTPE;
    }
    for (i = 0; i < (uint32_t)sizeof(*out); i++) {
        ((uint8_t *)(void *)out)[i] = 0;
    }
    rc = pe_parse_inner((const uint8_t *)hdr, hdr_len, file_len, place, min_allowed,
                        end_allowed, out);
    /* A file no linker produced, as against the wrong kind of file: the boot's
     * media holds none, so the boot asserts the count is zero (vibeos/mbz.h). */
    if (rc == VIBEOS_PE_EMALFORMED || rc == VIBEOS_PE_ETOOMANY) {
        vibeos_mbz_hit(VIBEOS_MBZ_PE_MALFORMED, (uint64_t)rc);
    }
    if (rc != VIBEOS_PE_OK) {
        out->layout.count = 0;
    }
    return rc;
}

int vibeos_pe_parse(const void *hdr, uint64_t hdr_len, uint64_t file_len,
                    uint64_t min_allowed, uint64_t end_allowed,
                    vibeos_pe_image_t *out) {
    return pe_parse_placed(hdr, hdr_len, file_len, 0u, min_allowed, end_allowed, out);
}

int vibeos_pe_parse_at(const void *hdr, uint64_t hdr_len, uint64_t file_len,
                       uint64_t base, uint64_t min_allowed, uint64_t end_allowed,
                       vibeos_pe_image_t *out) {
    if (base == 0u) {
        return VIBEOS_PE_ERANGE;   /* nowhere is not a place */
    }
    return pe_parse_placed(hdr, hdr_len, file_len, base, min_allowed, end_allowed, out);
}

/* ---- base relocation ------------------------------------------------------------
 *
 * The directory is a run of blocks, one per page that has anything to fix: the
 * page's RVA, the block's size in bytes (header included), then 16-bit
 * entries - a type in the top four bits, the offset in the page below. A PE32+
 * image needs two types: ABSOLUTE, padding that keeps a block a multiple of
 * four bytes, and DIR64, a 64-bit address to move by delta. Anything else is a
 * 32-bit image's, or no linker's. */

#define PE_REL_ABSOLUTE 0u
#define PE_REL_DIR64 10u

int vibeos_pe_check_relocs(const vibeos_pe_image_t *pe, const uint8_t *relocs,
                           uint32_t len) {
    uint32_t at = 0;

    if (!pe || (!relocs && len != 0u)) {
        return VIBEOS_PE_EMALFORMED;
    }
    while (at < len) {
        uint32_t page, size, i;

        if (len - at < 8u) {
            return VIBEOS_PE_EMALFORMED;
        }
        page = rd32(relocs + at);
        size = rd32(relocs + at + 4u);
        if (size < 8u || size > len - at || (size & 1u) != 0u ||
            (page & (VIBEOS_ELF_PAGE_SIZE - 1u)) != 0u || page >= pe->image_size) {
            return VIBEOS_PE_EMALFORMED;
        }
        for (i = 8u; i < size; i += 2u) {
            uint16_t e = rd16(relocs + at + i);
            uint32_t type = (uint32_t)e >> 12, off = (uint32_t)e & 0xFFFu;

            if (type == PE_REL_ABSOLUTE) {
                continue;
            }
            /* The eight bytes it moves must be the image's. */
            if (type != PE_REL_DIR64 || (uint64_t)page + off + 8u > pe->image_size) {
                return VIBEOS_PE_EMALFORMED;
            }
        }
        at += size;
    }
    return VIBEOS_PE_OK;
}

/* `n` bytes of the image as the file supplies them at `va`: zero where no
 * segment's file part reaches, as the page fill makes them. */
void vibeos_pe_read_image(const vibeos_pe_image_t *pe, vibeos_elf_read_fn read, void *ctx,
                          uint64_t va, uint8_t *out, uint32_t n) {
    uint32_t k, i;

    for (k = 0; k < n; k++) {
        out[k] = 0;
    }
    for (i = 0; i < pe->layout.count; i++) {
        const vibeos_elf_segment_t *s = &pe->layout.seg[i];
        uint64_t from = va > s->vaddr ? va : s->vaddr;
        uint64_t to = va + n < s->vaddr + s->filesz ? va + n : s->vaddr + s->filesz;

        if (from < to) {
            (void)read(ctx, s->file_off + (from - s->vaddr), (uint32_t)(to - from),
                       out + (from - va));
        }
    }
}

typedef struct {
    const uint8_t *b;
    uint64_t len;
} pe_mem_t;

static int pe_read_mem(void *ctx, uint64_t off, uint32_t len, void *buf) {
    const pe_mem_t *m = (const pe_mem_t *)ctx;
    uint32_t i;

    if (off + (uint64_t)len > m->len) {
        return -1;
    }
    for (i = 0; i < len; i++) {
        ((uint8_t *)buf)[i] = m->b[off + i];
    }
    return 0;
}

void vibeos_pe_read_image_mem(const vibeos_pe_image_t *pe, const void *file, uint64_t len,
                              uint64_t va, uint8_t *out, uint32_t n) {
    pe_mem_t m;

    m.b = (const uint8_t *)file;
    m.len = len;
    vibeos_pe_read_image(pe, pe_read_mem, &m, va, out, n);
}

/* One block's entries against one page: each DIR64 value whose eight bytes
 * overlap [page_va, page_va + 4096), moved by delta, and only the overlapping
 * bytes written. */
static void pe_relocate_block(const vibeos_pe_image_t *pe, const uint8_t *blk, uint32_t size,
                              vibeos_elf_read_fn read, void *ctx, uint64_t page_va,
                              uint8_t *page) {
    uint64_t block_va = pe->image_base + rd32(blk);
    uint32_t i;

    for (i = 8u; i < size; i += 2u) {
        uint16_t e = rd16(blk + i);
        uint64_t at = block_va + ((uint32_t)e & 0xFFFu);
        uint8_t v[8];
        uint64_t value;
        uint32_t k;

        if (((uint32_t)e >> 12) != PE_REL_DIR64 || at + 8u <= page_va ||
            at >= page_va + VIBEOS_ELF_PAGE_SIZE) {
            continue;
        }
        vibeos_pe_read_image(pe, read, ctx, at, v, 8u);
        value = rd64(v) + (uint64_t)pe->delta;
        for (k = 0; k < 8u; k++) {
            uint64_t b = at + k;
            if (b >= page_va && b < page_va + VIBEOS_ELF_PAGE_SIZE) {
                page[b - page_va] = (uint8_t)(value >> (8u * k));
            }
        }
    }
}

void vibeos_pe_relocate_page(const vibeos_pe_image_t *pe, const uint8_t *relocs,
                             uint32_t len, vibeos_elf_read_fn read, void *ctx,
                             uint64_t page_va, uint8_t *page) {
    uint32_t at = 0;

    if (!pe || pe->delta == 0 || !relocs || !read || !page) {
        return;
    }
    while (at + 8u <= len) {
        uint32_t size = rd32(relocs + at + 4u);
        uint64_t block_va = pe->image_base + rd32(relocs + at);

        if (size < 8u || size > len - at) {
            return;   /* checked already; never trusted further than that */
        }
        /* This page's block, and the one before it for an entry that starts
         * on the previous page and ends on this one. */
        if (block_va == page_va || block_va + VIBEOS_ELF_PAGE_SIZE == page_va) {
            pe_relocate_block(pe, relocs + at, size, read, ctx, page_va, page);
        }
        at += size;
    }
}
