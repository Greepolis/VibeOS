/* Copying to and from user memory, surviving a fault (H-003, H-010).
 *
 * A user range the kernel has validated can stop being mapped before it is
 * copied: a sibling thread munmaps it on another core, or - for a syscall that
 * blocks - at any time while the caller sleeps. Validation cannot close that
 * window; only the copy can survive it.
 *
 *     int vibeos_uaccess_copy(void *dst, const void *src, uint64_t len);
 *
 * returns 0, or -1 if the copy faulted. The only instruction that touches the
 * user side is the `rep movsb` between vibeos_uaccess_copy_begin and
 * vibeos_uaccess_copy_end. When the trap handler takes a ring-0 page fault that
 * the copy-on-write path did not resolve, at a rip in that range, on a user
 * address, it resumes at vibeos_uaccess_copy_fixup instead of panicking. The
 * stack is exactly as the fault left it - inside this leaf function, before any
 * push - so the fixup's `ret` returns to the caller.
 *
 * Assembly, and not a label taken with `&&label` from C: that was the first
 * attempt, and under this freestanding link it produced a section-relative
 * address, so the handler resumed at 0xaa020 and took #UD. These symbols are
 * ordinary relocations, the same kind every function pointer in the kernel is. */

    .text

    .globl vibeos_uaccess_copy
    .type vibeos_uaccess_copy, @function
vibeos_uaccess_copy:
    movq %rdx, %rcx
    .globl vibeos_uaccess_copy_begin
vibeos_uaccess_copy_begin:
    rep movsb
    .globl vibeos_uaccess_copy_end
vibeos_uaccess_copy_end:
    xorl %eax, %eax
    ret

    .globl vibeos_uaccess_copy_fixup
    .type vibeos_uaccess_copy_fixup, @function
vibeos_uaccess_copy_fixup:
    movl $-1, %eax
    ret

/* int vibeos_uaccess_cmpxchg32(uint32_t *uaddr, uint32_t *expected, uint32_t desired);
 *
 * One atomic compare-exchange on a user word (docs/abi/ L6: futex's WAKE_OP
 * and the robust list at a thread's exit both change a word other threads
 * change with their own atomics, so a read and a later write would lose
 * theirs). 0 with *expected holding the word as it was - equal to what was
 * passed when the exchange happened - or -1 if the word was not there. The
 * same recovery point as the copy: the one instruction that touches the user
 * side lies between its own begin and end, and nothing is pushed. */
    .globl vibeos_uaccess_cmpxchg32
    .type vibeos_uaccess_cmpxchg32, @function
vibeos_uaccess_cmpxchg32:
    movl (%rsi), %eax
    .globl vibeos_uaccess_atomic_begin
vibeos_uaccess_atomic_begin:
    lock cmpxchgl %edx, (%rdi)
    .globl vibeos_uaccess_atomic_end
vibeos_uaccess_atomic_end:
    movl %eax, (%rsi)
    xorl %eax, %eax
    ret

    .section .note.GNU-stack,"",@progbits
