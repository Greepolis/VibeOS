/* Minimal ELF64 loader for VibeOS user programs (image-only).
 *
 * Loads a statically-linked, position-dependent ELF64 from memory: it copies
 * each PT_LOAD segment to its p_vaddr (which must lie in the kernel's
 * identity-mapped, user-accessible low region) and zero-fills the BSS tail.
 * Returns the entry point so the caller can jump to it in ring 3.
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
#define VIBEOS_ELF_VADDR_LIMIT 0x100000000ull /* identity map covers first 4 GiB */

int vibeos_x86_64_load_elf(const unsigned char *elf, uint64_t len, uint64_t *out_entry) {
    const Elf64_Ehdr *eh;
    uint16_t i;

    if (!elf || !out_entry || len < sizeof(Elf64_Ehdr)) {
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
        uint8_t *dst;
        uint64_t j;

        if (ph->p_type != PT_LOAD || ph->p_memsz == 0) {
            continue;
        }
        /* Bounds: file range in the blob, and target range in the identity map. */
        if (ph->p_offset + ph->p_filesz > len || ph->p_filesz > ph->p_memsz) {
            return -1;
        }
        if (ph->p_vaddr + ph->p_memsz > VIBEOS_ELF_VADDR_LIMIT) {
            return -1;
        }
        dst = (uint8_t *)(uintptr_t)ph->p_vaddr;
        for (j = 0; j < ph->p_filesz; j++) {
            dst[j] = elf[ph->p_offset + j];
        }
        for (; j < ph->p_memsz; j++) {
            dst[j] = 0; /* .bss */
        }
    }

    *out_entry = eh->e_entry;
    return 0;
}
