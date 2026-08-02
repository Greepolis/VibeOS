/* ELF64 program-image parser. See include/vibeos/elf.h for why this works a
 * page at a time rather than a segment at a time.
 *
 * Fields are read byte by byte rather than through a struct cast: a program
 * header can sit at any offset the file chooses, and nothing here should
 * depend on that offset happening to be aligned.
 *
 * Every arithmetic step on a value taken from the file is checked for
 * overflow before it is used. The file is untrusted input.
 */

#include "vibeos/elf.h"

#define PT_LOAD 1u
#define PT_INTERP 3u
#define ET_EXEC 2u
#define ET_DYN 3u
#define ELFCLASS64 2u
#define ELFDATA2LSB 1u
#define EM_X86_64 62u

#define EHDR_SIZE 64u

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

/* a + b, refusing to wrap. */
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

int vibeos_elf_parse(const void *image, uint64_t len,
                     uint64_t min_allowed, uint64_t end_allowed,
                     vibeos_elf_image_t *out) {
    const uint8_t *b = (const uint8_t *)image;
    uint64_t phoff, ph_total;
    uint16_t phnum, phentsize, etype, i;
    uint64_t lo = 0xFFFFFFFFFFFFFFFFull;
    uint64_t hi = 0;

    if (!image || !out || len < EHDR_SIZE) {
        return VIBEOS_ELF_ETRUNCATED;
    }
    for (i = 0; i < (uint16_t)sizeof(*out); i++) {
        ((uint8_t *)(void *)out)[i] = 0;
    }

    if (b[0] != 0x7Fu || b[1] != 'E' || b[2] != 'L' || b[3] != 'F') {
        return VIBEOS_ELF_ENOTELF;
    }
    if (b[4] != ELFCLASS64 || b[5] != ELFDATA2LSB) {
        return VIBEOS_ELF_EMACHINE;
    }
    if (rd16(b + 18) != EM_X86_64) {
        return VIBEOS_ELF_EMACHINE;
    }

    etype = rd16(b + 16);
    if (etype == ET_DYN) {
        /* Position-independent executables need relocation and usually an
         * interpreter. Refuse clearly rather than load something that will
         * fault at a puzzling address. */
        return VIBEOS_ELF_EDYNAMIC;
    }
    if (etype != ET_EXEC) {
        return VIBEOS_ELF_ETYPE;
    }

    phoff = rd64(b + 32);
    phentsize = rd16(b + 54);
    phnum = rd16(b + 56);
    if (phnum == 0u || phentsize < 56u) {
        return VIBEOS_ELF_EMALFORMED;
    }
    if (!add_ok(phoff, (uint64_t)phnum * phentsize, &ph_total) || ph_total > len) {
        return VIBEOS_ELF_ETRUNCATED;
    }

    for (i = 0; i < phnum; i++) {
        const uint8_t *ph = b + phoff + (uint64_t)i * phentsize;
        uint32_t type = rd32(ph + 0);
        uint32_t flags = rd32(ph + 4);
        uint64_t off = rd64(ph + 8);
        uint64_t vaddr = rd64(ph + 16);
        uint64_t filesz = rd64(ph + 32);
        uint64_t memsz = rd64(ph + 40);
        uint64_t file_end, mem_end;

        if (type == PT_INTERP) {
            return VIBEOS_ELF_EDYNAMIC;
        }
        if (type != PT_LOAD || memsz == 0u) {
            continue;
        }
        if (filesz > memsz) {
            return VIBEOS_ELF_EMALFORMED;
        }
        if (!add_ok(off, filesz, &file_end) || file_end > len) {
            return VIBEOS_ELF_ETRUNCATED;
        }
        if (!add_ok(vaddr, memsz, &mem_end)) {
            return VIBEOS_ELF_EMALFORMED;
        }
        if (vaddr < min_allowed || mem_end > end_allowed) {
            return VIBEOS_ELF_ERANGE;
        }
        if (out->count >= VIBEOS_ELF_MAX_SEGMENTS) {
            return VIBEOS_ELF_ETOOMANY;
        }

        out->seg[out->count].vaddr = vaddr;
        out->seg[out->count].file_off = off;
        out->seg[out->count].filesz = filesz;
        out->seg[out->count].memsz = memsz;
        out->seg[out->count].flags = flags & (VIBEOS_ELF_R | VIBEOS_ELF_W | VIBEOS_ELF_X);
        out->count++;

        if (vaddr < lo) {
            lo = vaddr;
        }
        if (mem_end > hi) {
            hi = mem_end;
        }
    }

    if (out->count == 0u) {
        return VIBEOS_ELF_EMALFORMED;
    }

    out->entry = rd64(b + 24);
    if (out->entry < lo || out->entry >= hi) {
        return VIBEOS_ELF_EMALFORMED;
    }
    out->min_vaddr = page_down(lo);
    if (!page_up(hi, &out->end_vaddr)) {
        return VIBEOS_ELF_EMALFORMED;
    }
    out->phnum = phnum;
    out->phentsize = phentsize;

    /* AT_PHDR needs the address the headers ended up at, which only exists if
     * some PT_LOAD segment actually covers them. */
    for (i = 0; i < out->count; i++) {
        const vibeos_elf_segment_t *s = &out->seg[i];
        if (phoff >= s->file_off && ph_total <= s->file_off + s->filesz) {
            out->phdr_vaddr = s->vaddr + (phoff - s->file_off);
            break;
        }
    }
    return VIBEOS_ELF_OK;
}

uint32_t vibeos_elf_page_flags(const vibeos_elf_image_t *img, uint64_t page_va) {
    uint32_t flags = 0;
    uint32_t i;

    if (!img) {
        return 0;
    }
    for (i = 0; i < img->count; i++) {
        const vibeos_elf_segment_t *s = &img->seg[i];
        uint64_t seg_first = page_down(s->vaddr);
        uint64_t seg_last = page_down(s->vaddr + s->memsz - 1u);
        if (page_va >= seg_first && page_va <= seg_last) {
            /* Union, not replacement: a page holding the tail of .text and the
             * head of .data must be both executable and writable. */
            flags |= s->flags;
        }
    }
    return flags;
}

void vibeos_elf_fill_page(const vibeos_elf_image_t *img, const void *image,
                          uint64_t page_va, void *dst) {
    const uint8_t *src = (const uint8_t *)image;
    uint8_t *d = (uint8_t *)dst;
    uint32_t i;
    uint64_t k;

    if (!img || !image || !dst) {
        return;
    }
    for (k = 0; k < VIBEOS_ELF_PAGE_SIZE; k++) {
        d[k] = 0;   /* .bss and the padding either side of a segment */
    }
    for (i = 0; i < img->count; i++) {
        const vibeos_elf_segment_t *s = &img->seg[i];
        uint64_t copy_start = s->vaddr;
        uint64_t copy_end = s->vaddr + s->filesz;   /* memsz beyond this is zero */
        uint64_t page_end = page_va + VIBEOS_ELF_PAGE_SIZE;
        uint64_t from, to;

        from = (copy_start > page_va) ? copy_start : page_va;
        to = (copy_end < page_end) ? copy_end : page_end;
        for (k = from; k < to; k++) {
            d[k - page_va] = src[s->file_off + (k - s->vaddr)];
        }
    }
}
