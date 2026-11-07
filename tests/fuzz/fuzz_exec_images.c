/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Coverage-guided fuzzing for the production ELF64 and PE32+ image parsers.
 * Each testcase is independent; successful parses also exercise page layout
 * and image-byte extraction, not just header rejection. */

#include <stddef.h>
#include <stdint.h>

#include "vibeos/elf.h"
#include "vibeos/pe.h"

static uint8_t g_page[VIBEOS_ELF_PAGE_SIZE];

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    vibeos_elf_image_t elf;
    vibeos_pe_image_t pe;
    int rc;

    if (!data || size == 0u || size > (1u << 20)) {
        return 0;
    }

    rc = vibeos_elf_parse_ex(data, size, 0u, 0x10000u,
                             0x0000800000000000ull,
                             VIBEOS_ELF_ALLOW_DYN | VIBEOS_ELF_ALLOW_INTERP,
                             &elf);
    if (rc == VIBEOS_ELF_OK) {
        uint32_t i;
        for (i = 0; i < elf.count; i++) {
            uint64_t first = elf.seg[i].vaddr & ~(uint64_t)(VIBEOS_ELF_PAGE_SIZE - 1u);
            vibeos_elf_fill_page(&elf, data, first, g_page);
            if (elf.seg[i].memsz > VIBEOS_ELF_PAGE_SIZE) {
                uint64_t last = (elf.seg[i].vaddr + elf.seg[i].memsz - 1u) &
                                ~(uint64_t)(VIBEOS_ELF_PAGE_SIZE - 1u);
                vibeos_elf_fill_page(&elf, data, last, g_page);
            }
        }
    }

    rc = vibeos_pe_parse(data, size, size, 0x10000u,
                         0x0000800000000000ull, &pe);
    if (rc == VIBEOS_PE_OK) {
        uint32_t i;
        uint8_t extracted[256];
        for (i = 0; i < pe.layout.count; i++) {
            uint64_t first = pe.layout.seg[i].vaddr &
                             ~(uint64_t)(VIBEOS_ELF_PAGE_SIZE - 1u);
            vibeos_elf_fill_page(&pe.layout, data, first, g_page);
            if (pe.layout.seg[i].memsz > VIBEOS_ELF_PAGE_SIZE) {
                uint64_t last = (pe.layout.seg[i].vaddr +
                                 pe.layout.seg[i].memsz - 1u) &
                                ~(uint64_t)(VIBEOS_ELF_PAGE_SIZE - 1u);
                vibeos_elf_fill_page(&pe.layout, data, last, g_page);
            }
        }
        vibeos_pe_read_image_mem(&pe, data, size, pe.image_base,
                                 extracted, sizeof(extracted));
    }
    return 0;
}
