/* x86_64 interrupt service routine stubs.
 *
 * Each CPU exception vector (0-31) has a small stub that normalizes the
 * stack into a common layout, then jumps to a shared trampoline which
 * saves the general-purpose registers and calls the C handler
 * (vibeos_x86_64_isr_handler) with a pointer to the saved frame.
 *
 * Stack layout seen by the C handler (low address -> high address):
 *   r15,r14,r13,r12,r11,r10,r9,r8,rbp,rdi,rsi,rdx,rcx,rbx,rax,
 *   vector, error_code, rip, cs, rflags, rsp, ss
 * This matches struct vibeos_x86_64_isr_frame in arch_hw.c.
 *
 * Compiled only into the freestanding kernel image (not host tests).
 */

    .section .text
    .code64

/* Vectors without a CPU-pushed error code: push a dummy 0 for uniformity. */
.macro ISR_NOERR num
    .global vibeos_isr_\num
vibeos_isr_\num:
    pushq $0
    pushq $\num
    jmp vibeos_isr_common
.endm

/* Vectors with a CPU-pushed error code: only push the vector. */
.macro ISR_ERR num
    .global vibeos_isr_\num
vibeos_isr_\num:
    pushq $\num
    jmp vibeos_isr_common
.endm

    ISR_NOERR 0    /* #DE divide error            */
    ISR_NOERR 1    /* #DB debug                   */
    ISR_NOERR 2    /* NMI                         */
    ISR_NOERR 3    /* #BP breakpoint (int3)       */
    ISR_NOERR 4    /* #OF overflow                */
    ISR_NOERR 5    /* #BR bound range             */
    ISR_NOERR 6    /* #UD invalid opcode          */
    ISR_NOERR 7    /* #NM device not available    */
    ISR_ERR   8    /* #DF double fault            */
    ISR_NOERR 9    /* coprocessor segment overrun */
    ISR_ERR   10   /* #TS invalid TSS             */
    ISR_ERR   11   /* #NP segment not present     */
    ISR_ERR   12   /* #SS stack-segment fault     */
    ISR_ERR   13   /* #GP general protection      */
    ISR_ERR   14   /* #PF page fault              */
    ISR_NOERR 15   /* reserved                    */
    ISR_NOERR 16   /* #MF x87 FP                  */
    ISR_ERR   17   /* #AC alignment check         */
    ISR_NOERR 18   /* #MC machine check           */
    ISR_NOERR 19   /* #XM SIMD FP                 */
    ISR_NOERR 20   /* #VE virtualization          */
    ISR_ERR   21   /* #CP control protection      */
    ISR_NOERR 22
    ISR_NOERR 23
    ISR_NOERR 24
    ISR_NOERR 25
    ISR_NOERR 26
    ISR_NOERR 27
    ISR_NOERR 28
    ISR_ERR   29   /* #VC VMM communication       */
    ISR_ERR   30   /* #SX security exception      */
    ISR_NOERR 31   /* reserved                    */

vibeos_isr_common:
    /* Save general-purpose registers. Order matters: the last one pushed
     * (r15) is at the lowest address == first field of the frame struct. */
    push %rax
    push %rbx
    push %rcx
    push %rdx
    push %rsi
    push %rdi
    push %rbp
    push %r8
    push %r9
    push %r10
    push %r11
    push %r12
    push %r13
    push %r14
    push %r15

    mov %rsp, %rdi          /* arg0 = pointer to saved frame */
    mov %rsp, %rax          /* remember unaligned rsp        */
    and $~15, %rsp          /* 16-byte align for SysV call   */
    push %rax
    push %rax               /* keep 16-byte alignment, save old rsp */

    call vibeos_x86_64_isr_handler

    pop %rax
    pop %rax                /* recover saved rsp */
    mov %rax, %rsp

    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %r11
    pop %r10
    pop %r9
    pop %r8
    pop %rbp
    pop %rdi
    pop %rsi
    pop %rdx
    pop %rcx
    pop %rbx
    pop %rax

    add $16, %rsp           /* discard vector + error_code */
    iretq

/* Table of stub entry points, indexed by vector, consumed by arch_hw.c. */
    .section .rodata
    .align 8
    .global vibeos_isr_stub_table
vibeos_isr_stub_table:
    .quad vibeos_isr_0,  vibeos_isr_1,  vibeos_isr_2,  vibeos_isr_3
    .quad vibeos_isr_4,  vibeos_isr_5,  vibeos_isr_6,  vibeos_isr_7
    .quad vibeos_isr_8,  vibeos_isr_9,  vibeos_isr_10, vibeos_isr_11
    .quad vibeos_isr_12, vibeos_isr_13, vibeos_isr_14, vibeos_isr_15
    .quad vibeos_isr_16, vibeos_isr_17, vibeos_isr_18, vibeos_isr_19
    .quad vibeos_isr_20, vibeos_isr_21, vibeos_isr_22, vibeos_isr_23
    .quad vibeos_isr_24, vibeos_isr_25, vibeos_isr_26, vibeos_isr_27
    .quad vibeos_isr_28, vibeos_isr_29, vibeos_isr_30, vibeos_isr_31

/* Mark stack as non-executable to silence the linker warning. */
    .section .note.GNU-stack,"",@progbits
