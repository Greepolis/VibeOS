/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The personalities other than Linux, as the architecture reaches them
 * (docs/abi/windows.md W8 step 1).
 *
 * Linux is the machine's own: a task without a personality of its own is a
 * Linux task, its syscalls are read with Linux's registers, its faults become
 * signals, and an ELF is its image. Any other personality - NT today -
 * registers here at boot from glue of its own, and the core reaches it only
 * through this table: by the ABI bound to a task, or by an image it
 * recognises. So a kernel built without a personality is the same kernel with
 * a shorter list, and that is what lets the build without NT be checked
 * rather than promised.
 *
 * The list is written once, during early init on the boot core, before any
 * task exists; afterwards it is only read, so it takes no lock. */

#include "arch_hw_internal.h"

#define HW_PERSONALITIES_MAX 4u

static const hw_personality_t *g_personalities[HW_PERSONALITIES_MAX];
static uint32_t g_personality_count;

void hw_personality_register(const hw_personality_t *p) {
    uint32_t i;

    if (!p || !p->abi || !p->syscall || !p->fault || !p->is_image || !p->map_images ||
        !p->start_blocks) {
        hw_panic("personality registered without its hooks");
    }
    for (i = 0; i < g_personality_count; i++) {
        if (g_personalities[i] == p || g_personalities[i]->abi == p->abi) {
            hw_panic("personality registered twice");
        }
    }
    if (g_personality_count >= HW_PERSONALITIES_MAX) {
        hw_panic("too many personalities");
    }
    g_personalities[g_personality_count++] = p;
}

const hw_personality_t *hw_personality_of_abi(const vibeos_abi_t *abi) {
    uint32_t i;

    for (i = 0; abi && i < g_personality_count; i++) {
        if (g_personalities[i]->abi == abi) {
            return g_personalities[i];
        }
    }
    return 0;
}

const hw_personality_t *hw_personality_of_image(const unsigned char *hdr, uint64_t len) {
    uint32_t i;

    for (i = 0; i < g_personality_count; i++) {
        if (g_personalities[i]->is_image(hdr, len)) {
            return g_personalities[i];
        }
    }
    return 0;
}

uint32_t hw_personality_count(void) {
    return g_personality_count;
}
