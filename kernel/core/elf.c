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
    /* No options: the historical contract, static ET_EXEC only. */
    return vibeos_elf_parse_ex(image, len, 0, min_allowed, end_allowed, 0, out);
}

int vibeos_elf_parse_ex(const void *image, uint64_t len,
                        uint64_t load_bias,
                        uint64_t min_allowed, uint64_t end_allowed,
                        uint32_t opts, vibeos_elf_image_t *out) {
    /* The buffer *is* the reader. See vibeos_elf_parse_read. */
    return vibeos_elf_parse_read(image, len, load_bias, min_allowed,
                                 end_allowed, opts, 0, 0, out);
}

int vibeos_elf_parse_read(const void *image, uint64_t len,
                          uint64_t load_bias,
                          uint64_t min_allowed, uint64_t end_allowed,
                          uint32_t opts,
                          vibeos_elf_read_fn read, void *read_ctx,
                          vibeos_elf_image_t *out) {
    const uint8_t *b = (const uint8_t *)image;
    uint64_t phoff, ph_total;
    uint16_t phnum, phentsize, etype;
    /* Wider than every bound it is compared against. An index narrower than
     * the count it is tested against cannot terminate the loop if the count
     * ever exceeds the index's range - the comparison is promoted, the
     * increment wraps, and the loop runs forever. */
    uint32_t i;
    uint64_t lo = 0xFFFFFFFFFFFFFFFFull;
    uint64_t hi = 0;

    if (!image || !out || len < EHDR_SIZE) {
        return VIBEOS_ELF_ETRUNCATED;
    }
    for (i = 0; i < (uint32_t)sizeof(*out); i++) {
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
        if (!(opts & VIBEOS_ELF_ALLOW_DYN)) {
            /* A caller that cannot relocate would map the file's zero-based
             * addresses literally and fault at a puzzling address. Refuse
             * clearly instead. */
            return VIBEOS_ELF_EDYNAMIC;
        }
        out->is_dyn = 1u;
    } else if (etype == ET_EXEC) {
        /* Not relocatable: its addresses are absolute, so a bias would move
         * the pages out from under the program's own references. Refusing is
         * the only honest answer to a caller that asked for one anyway. */
        if (load_bias != 0u) {
            return VIBEOS_ELF_EMALFORMED;
        }
    } else {
        return VIBEOS_ELF_ETYPE;
    }
    /* A sub-page bias would leave every segment straddling a page boundary it
     * was not built to straddle, and the caller maps whole pages. */
    if (load_bias != page_down(load_bias)) {
        return VIBEOS_ELF_EMALFORMED;
    }
    out->load_bias = load_bias;

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
            uint64_t k;
            if (!(opts & VIBEOS_ELF_ALLOW_INTERP)) {
                return VIBEOS_ELF_EDYNAMIC;
            }
            /* Two interpreters is not a choice to make on the file's behalf. */
            if (out->has_interp) {
                return VIBEOS_ELF_EMALFORMED;
            }
            if (!add_ok(off, filesz, &file_end) || file_end > len) {
                return VIBEOS_ELF_ETRUNCATED;
            }
            if (filesz == 0u || filesz > VIBEOS_ELF_MAX_INTERP) {
                return VIBEOS_ELF_EMALFORMED;
            }
            /* The only thing here that is not in the headers, and therefore
             * the only reason `image` ever had to be the whole file.
             *
             * With a reader the caller's buffer need only cover the headers;
             * without one it must cover this too, and a range past the end of
             * it is refused rather than read beyond. */
            if (read) {
                if (read(read_ctx, off, (uint32_t)filesz, out->interp) != 0) {
                    return VIBEOS_ELF_EMALFORMED;
                }
            } else {
                if (off + filesz > len) {
                    return VIBEOS_ELF_EMALFORMED;
                }
                for (k = 0; k < filesz; k++) {
                    out->interp[k] = (char)b[off + k];
                }
            }
            /* The path must already be terminated inside its own segment.
             * Terminating it here would turn a malformed file into a request
             * to open some prefix of a path, which is a different file.
             *
             * Checked after the copy rather than before it, because with a
             * reader the bytes are not addressable until they have been
             * fetched - and checking a different copy from the one that is
             * used is how the two come to disagree. */
            if (out->interp[filesz - 1u] != 0) {
                return VIBEOS_ELF_EMALFORMED;
            }
            out->has_interp = 1u;
            continue;
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
        /* From here on `vaddr` is where the segment will really live. Biasing
         * before the range and overflow checks is what makes those checks mean
         * anything for an ET_DYN image. */
        if (!add_ok(vaddr, load_bias, &vaddr)) {
            return VIBEOS_ELF_EMALFORMED;
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

    /* e_entry is file-relative for ET_DYN just as segment addresses are, so it
     * is biased before it is checked against the biased segment span. */
    if (!add_ok(rd64(b + 24), load_bias, &out->entry)) {
        return VIBEOS_ELF_EMALFORMED;
    }
    if (out->entry < lo || out->entry >= hi) {
        return VIBEOS_ELF_EMALFORMED;
    }
    out->min_vaddr = page_down(lo);
    if (!page_up(hi, &out->end_vaddr)) {
        return VIBEOS_ELF_EMALFORMED;
    }
    out->image_span = out->end_vaddr - out->min_vaddr;
    out->phnum = phnum;
    out->phentsize = phentsize;

    /* AT_PHDR needs the address the headers ended up at, which only exists if
     * some PT_LOAD segment actually covers them. The segment's vaddr already
     * carries the bias, so what comes out of this is where the headers really
     * are, not where the file said they would be. */
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

int vibeos_elf_page_file_offset(const vibeos_elf_image_t *img, uint64_t page_va,
                                uint64_t *out_off) {
    uint32_t i;
    uint32_t covering = 0;
    uint64_t off = 0;

    if (!img || !out_off) {
        return 0;
    }
    for (i = 0; i < img->count; i++) {
        const vibeos_elf_segment_t *s = &img->seg[i];
        uint64_t seg_first = page_down(s->vaddr);
        uint64_t seg_last = page_down(s->vaddr + s->memsz - 1u);

        if (page_va < seg_first || page_va > seg_last) {
            continue;
        }
        /* A second segment touching this page means the page belongs to both,
         * and no single file offset describes it. */
        covering++;
        if (covering > 1u) {
            return 0;
        }
        /* The page must lie entirely inside the part of the segment the file
         * actually supplies, and start where the file does: a segment whose
         * vaddr is not page-aligned begins part-way into its first page, so
         * that page carries bytes from before the segment too. */
        if ((s->vaddr & (VIBEOS_ELF_PAGE_SIZE - 1u)) != 0u) {
            return 0;
        }
        if (page_va + VIBEOS_ELF_PAGE_SIZE > s->vaddr + s->filesz) {
            return 0;   /* runs into the .bss tail, or past the file */
        }
        off = s->file_off + (page_va - s->vaddr);
        /* And the file offset has to be page-aligned, or the cache - which is
         * keyed on page-aligned offsets - cannot hand back this page. */
        if ((off & (VIBEOS_ELF_PAGE_SIZE - 1u)) != 0u) {
            return 0;
        }
    }
    if (covering != 1u) {
        return 0;
    }
    *out_off = off;
    return 1;
}

void vibeos_elf_fill_page_via(const vibeos_elf_image_t *img,
                              vibeos_elf_read_fn read, void *ctx,
                              uint64_t page_va, void *dst) {
    uint8_t *d = (uint8_t *)dst;
    uint32_t i;
    uint64_t k;

    if (!img || !read || !dst) {
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
        if (from >= to) {
            continue;   /* this segment does not reach this page */
        }
        /* One read for the whole overlap. Within a segment the file layout is
         * contiguous, so the bytes this page needs from it are one range - and
         * issuing one read per byte would turn a page fill into four thousand
         * cache lookups. */
        if (read(ctx, s->file_off + (from - s->vaddr),
                 (uint32_t)(to - from), d + (from - page_va)) != 0) {
            /* Leave the rest zeroed rather than half-written. A partially
             * filled page of program text is indistinguishable from a correct
             * one until it is executed. */
            for (k = from; k < to; k++) {
                d[k - page_va] = 0;
            }
        }
    }
}

/* The pointer form, now a reader over memory.
 *
 * Kept because the host tests and the bootloader have the whole image to hand
 * and there is no reason to make them invent a callback. It is the same code
 * path, which is the point: two fills that could drift apart is how the copied
 * pages and the mapped pages come to disagree about a program. */
typedef struct elf_mem_reader {
    const uint8_t *base;
    uint64_t len;
} elf_mem_reader_t;

static int elf_read_memory(void *ctx, uint64_t off, uint32_t len, void *buf) {
    const elf_mem_reader_t *m = (const elf_mem_reader_t *)ctx;
    uint8_t *d = (uint8_t *)buf;
    uint32_t i;

    if (!m || off + (uint64_t)len > m->len) {
        return -1;
    }
    for (i = 0; i < len; i++) {
        d[i] = m->base[off + i];
    }
    return 0;
}

void vibeos_elf_fill_page(const vibeos_elf_image_t *img, const void *image,
                          uint64_t page_va, void *dst) {
    elf_mem_reader_t m;

    if (!img || !image || !dst) {
        return;
    }
    m.base = (const uint8_t *)image;
    /* The pointer form has no length, so the bound is the largest offset any
     * segment names. A caller that passes a shorter buffer than its own image
     * header describes is already wrong in a way this cannot detect. */
    m.len = 0xFFFFFFFFFFFFFFFFull;
    vibeos_elf_fill_page_via(img, elf_read_memory, &m, page_va, dst);
}

/* ---- initial process stack ------------------------------------------------ */

static uint64_t str_len(const char *s) {
    uint64_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

static uint64_t count_vec(const char *const *v) {
    uint64_t n = 0;
    if (!v) {
        return 0;
    }
    while (v[n]) {
        n++;
    }
    return n;
}

uint64_t vibeos_elf_build_stack(void *buf, uint64_t buf_len, uint64_t stack_top,
                                const vibeos_elf_stack_desc_t *desc) {
    uint8_t *base = (uint8_t *)buf;
    uint64_t argc, envc, i;
    uint64_t strings_bytes = 0;
    uint64_t sp;                 /* virtual address as we build downwards */
    uint64_t str_cursor;
    uint64_t *slot;
    uint64_t need;
    uint64_t aux_pairs;

    if (!buf || !desc || buf_len == 0u || (stack_top & 0xFu) != 0u) {
        return 0;
    }
    argc = count_vec(desc->argv);
    envc = count_vec(desc->envp);

    for (i = 0; i < argc; i++) {
        strings_bytes += str_len(desc->argv[i]) + 1u;
    }
    for (i = 0; i < envc; i++) {
        strings_bytes += str_len(desc->envp[i]) + 1u;
    }
    if (desc->random16) {
        strings_bytes += 16u;
    }

    /* PAGESZ, ENTRY, UID, EUID, GID, EGID, NULL, plus PHDR/PHENT/PHNUM,
     * RANDOM and BASE when they apply. */
    aux_pairs = 7u + (desc->phdr_vaddr ? 3u : 0u) + (desc->random16 ? 1u : 0u)
              + (desc->interp_base ? 1u : 0u);

    need = strings_bytes + 16u                     /* strings + alignment slack */
         + 8u                                      /* argc                      */
         + (argc + 1u) * 8u                        /* argv + NULL               */
         + (envc + 1u) * 8u                        /* envp + NULL               */
         + aux_pairs * 16u;                        /* auxv pairs                */
    if (need + 16u > buf_len) {
        return 0;
    }

    /* Strings sit at the very top; the pointer arrays below refer back to them. */
    str_cursor = stack_top - strings_bytes;
    str_cursor &= ~0xFull;
    sp = str_cursor;

    {
        uint64_t at = str_cursor;
        for (i = 0; i < argc; i++) {
            uint64_t n = str_len(desc->argv[i]) + 1u;
            uint64_t k;
            for (k = 0; k < n; k++) {
                base[(at + k) - (stack_top - buf_len)] = (uint8_t)desc->argv[i][k];
            }
            at += n;
        }
        for (i = 0; i < envc; i++) {
            uint64_t n = str_len(desc->envp[i]) + 1u;
            uint64_t k;
            for (k = 0; k < n; k++) {
                base[(at + k) - (stack_top - buf_len)] = (uint8_t)desc->envp[i][k];
            }
            at += n;
        }
        if (desc->random16) {
            for (i = 0; i < 16u; i++) {
                base[(at + i) - (stack_top - buf_len)] = desc->random16[i];
            }
        }
    }

    /* Now the vectors, growing down from just below the strings. The whole
     * block has to leave the stack pointer 16-byte aligned at entry, which the
     * ABI requires and a real _start relies on. */
    sp -= aux_pairs * 16u;
    sp -= (envc + 1u) * 8u;
    sp -= (argc + 1u) * 8u;
    sp -= 8u;                     /* argc itself */
    sp &= ~0xFull;

    slot = (uint64_t *)(void *)(base + (sp - (stack_top - buf_len)));
    *slot++ = argc;

    {
        uint64_t at = str_cursor;
        for (i = 0; i < argc; i++) {
            *slot++ = at;
            at += str_len(desc->argv[i]) + 1u;
        }
        *slot++ = 0;
        for (i = 0; i < envc; i++) {
            *slot++ = at;
            at += str_len(desc->envp[i]) + 1u;
        }
        *slot++ = 0;

        if (desc->phdr_vaddr) {
            *slot++ = VIBEOS_AT_PHDR;   *slot++ = desc->phdr_vaddr;
            *slot++ = VIBEOS_AT_PHENT;  *slot++ = desc->phentsize;
            *slot++ = VIBEOS_AT_PHNUM;  *slot++ = desc->phnum;
        }
        if (desc->interp_base) {
            /* Only when an interpreter was actually loaded. A static program
             * that saw AT_BASE would believe it. */
            *slot++ = VIBEOS_AT_BASE; *slot++ = desc->interp_base;
        }
        *slot++ = VIBEOS_AT_PAGESZ; *slot++ = VIBEOS_ELF_PAGE_SIZE;
        *slot++ = VIBEOS_AT_ENTRY;  *slot++ = desc->entry;
        *slot++ = VIBEOS_AT_UID;    *slot++ = 0;
        *slot++ = VIBEOS_AT_EUID;   *slot++ = 0;
        *slot++ = VIBEOS_AT_GID;    *slot++ = 0;
        *slot++ = VIBEOS_AT_EGID;   *slot++ = 0;
        if (desc->random16) {
            *slot++ = VIBEOS_AT_RANDOM; *slot++ = at;
        }
        *slot++ = VIBEOS_AT_NULL;   *slot++ = 0;
    }
    return sp;
}
