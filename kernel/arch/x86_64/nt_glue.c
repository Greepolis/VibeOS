/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The NT personality's hooks into the architecture (docs/abi/windows.md W8
 * step 1).
 *
 * Everything the x86-64 core used to name of Windows directly - the syscall
 * convention, the fault that becomes an exception, the PE image that exec
 * recognises - is here, registered at boot through personality.c. Built only
 * with VIBEOS_PERSONALITY_NT, with pe_load.c and kernel/abi/nt/: a kernel
 * without it has no file that names NT, and check.sh builds one to keep it
 * that way. */

#include "vibeos/abi_nt.h"
#include "vibeos/pe.h"
#include "arch_hw_internal.h"

/* The NT convention (docs/abi/windows.md W1): the number in eax, the first four
 * arguments in r10 (our ntdll's stub moved rcx there, as Windows' does), rdx,
 * r8 and r9, the rest on the stack where the dispatcher reads them. */
static long hw_nt_syscall(vibeos_x86_64_isr_frame_t *frame) {
    uint64_t a[6];

    a[0] = frame->r10;
    a[1] = frame->rdx;
    a[2] = frame->r8;
    a[3] = frame->r9;
    a[4] = 0;
    a[5] = 0;
    return nt_syscall((struct ks_regs *)(void *)frame, (uint64_t)(uint32_t)frame->rax, a);
}

static const hw_personality_t g_hw_nt = {
    0,   /* the ABI, set at init: vibeos_abi_nt() is not a constant expression */
    hw_nt_syscall,
    vibeos_nt_fault,
    vibeos_pe_is_pe,
    hw_pe_map_images,
    hw_pe_start_blocks,
};

static hw_personality_t g_hw_nt_live;

void hw_personality_nt_init(void) {
    vibeos_nt_abi_init();
    g_hw_nt_live = g_hw_nt;
    g_hw_nt_live.abi = vibeos_abi_nt();
    hw_personality_register(&g_hw_nt_live);
}

/* PE images in a running process (docs/abi/windows.md W2): the NT layer's
 * services for sections of images; the loader's work is in pe_load.c. */
int ks_pe_probe(const struct vibeos_file *f, uint64_t *image_size) {
    return hw_pe_probe(f, image_size);
}

int ks_map_pe_image(int slot, const struct vibeos_file *f, uint64_t *base, uint64_t *size) {
    return hw_map_pe_image(slot, f, base, size);
}
