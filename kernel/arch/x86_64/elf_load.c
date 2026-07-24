/* Minimal ELF64 parser/loader for VibeOS user programs (image-only).
 *
 * The parser validates the header and walks PT_LOAD segments, handing each to a
 * caller-supplied callback. The callback decides where the memory comes from
 * and how it is mapped, so the same parser serves both a flat identity load and
 * a per-process address space load.
 */

#include <stdint.h>

#include "vibeos/arch_x86_64.h"

typedef struct {
    unsigned char e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} Elf64_Phdr;

#define PT_LOAD 1u
#define ELFCLASS64 2u
#define EM_X86_64 62u

/* Called once per PT_LOAD segment. `flags` carries the ELF PF_* bits
 * (1=X, 2=W, 4=R). Return non-zero to abort the load. */
typedef int (*vibeos_elf_load_cb)(void *ctx, uint64_t vaddr, const unsigned char *data,
                                  uint64_t filesz, uint64_t memsz, uint32_t flags);

int vibeos_x86_64_elf_load(const unsigned char *elf, uint64_t len,
                           void *ctx, vibeos_elf_load_cb cb, uint64_t *out_entry) {
    const Elf64_Ehdr *eh;
    uint16_t i;

    if (!elf || !cb || !out_entry || len < sizeof(Elf64_Ehdr)) {
        return -1;
    }
    eh = (const Elf64_Ehdr *)(const void *)elf;
    if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L' || eh->e_ident[3] != 'F') {
        return -1;
    }
    if (eh->e_ident[4] != ELFCLASS64 || eh->e_machine != EM_X86_64) {
        return -1;
    }
    if (eh->e_phoff == 0 || eh->e_phnum == 0 ||
        eh->e_phoff + (uint64_t)eh->e_phnum * eh->e_phentsize > len) {
        return -1;
    }

    for (i = 0; i < eh->e_phnum; i++) {
        const Elf64_Phdr *ph =
            (const Elf64_Phdr *)(const void *)(elf + eh->e_phoff + (uint64_t)i * eh->e_phentsize);

        if (ph->p_type != PT_LOAD || ph->p_memsz == 0) {
            continue;
        }
        if (ph->p_offset + ph->p_filesz > len || ph->p_filesz > ph->p_memsz) {
            return -1;
        }
        if (cb(ctx, ph->p_vaddr, elf + ph->p_offset, ph->p_filesz, ph->p_memsz, ph->p_flags) != 0) {
            return -1;
        }
    }

    *out_entry = eh->e_entry;
    return 0;
}
