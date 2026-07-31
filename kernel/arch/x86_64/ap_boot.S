/* Application-processor boot trampoline.
 *
 * A processor woken by INIT-SIPI starts in 16-bit real mode at a page-aligned
 * address below 1 MiB, with none of long mode's state set up. This blob is
 * copied to AP_TRAMPOLINE (0x8000) and walks the core through real mode ->
 * protected mode -> long mode on the kernel's existing page tables, then jumps
 * to a C entry point.
 *
 * Because the code executes at a fixed physical address rather than where it
 * was linked, every intra-blob reference is written as an absolute address
 * computed from TRAMP, and the parameters (CR3, stack, entry) are read from
 * fixed offsets that apic.c fills in before sending the SIPI.
 *
 * Compiled only into the freestanding kernel image.
 */

    .set TRAMP,     0x8000
    .set OFF_CR3,   0xF00
    .set OFF_STACK, 0xF08
    .set OFF_ENTRY, 0xF10
    .set OFF_GDTR,  0xF20

    .section .text
    .global vibeos_ap_trampoline_start
    .global vibeos_ap_trampoline_end

    .code16
vibeos_ap_trampoline_start:
    cli
    cld
    xorw    %ax, %ax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %ss

    lgdtl   (TRAMP + OFF_GDTR)

    movl    %cr0, %eax
    orl     $1, %eax                /* PE: enter protected mode */
    movl    %eax, %cr0
    ljmpl   $0x08, $(TRAMP + (ap_pm32 - vibeos_ap_trampoline_start))

    .code32
ap_pm32:
    movw    $0x10, %ax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %ss
    movw    %ax, %fs
    movw    %ax, %gs

    movl    %cr4, %eax
    orl     $0x20, %eax             /* PAE */
    movl    %eax, %cr4

    movl    (TRAMP + OFF_CR3), %eax /* kernel PML4 */
    movl    %eax, %cr3

    movl    $0xC0000080, %ecx       /* IA32_EFER */
    rdmsr
    orl     $0x100, %eax            /* LME */
    wrmsr

    movl    %cr0, %eax
    orl     $0x80000001, %eax       /* PG | PE: long mode active */
    movl    %eax, %cr0

    ljmpl   $0x18, $(TRAMP + (ap_lm64 - vibeos_ap_trampoline_start))

    .code64
ap_lm64:
    /* A core released from INIT comes up with SSE disabled, while the firmware
     * had already enabled it on the bootstrap processor. An optimizing compiler
     * emits SSE freely, so the first movaps in the C entry point would #UD -
     * before the IDT is loaded, i.e. as a triple fault that takes down the
     * machine. Enable it here, before any C code runs. */
    movq    %cr0, %rax
    andq    $~0x4, %rax             /* EM = 0: no FPU emulation      */
    orq     $0x2, %rax              /* MP = 1                        */
    movq    %rax, %cr0
    movq    %cr4, %rax
    orq     $0x600, %rax            /* OSFXSR | OSXMMEXCPT           */
    movq    %rax, %cr4

    movl    $(TRAMP + OFF_STACK), %eax
    movq    (%rax), %rsp
    /* The System V ABI expects rsp % 16 == 8 on entry to a function, i.e. the
     * state just after a `call` pushed a return address. We jump rather than
     * call, so bias the stack by hand - otherwise every 16-byte-aligned stack
     * slot the callee spills to is misaligned, and an optimizing compiler's
     * SSE spills fault immediately. */
    subq    $8, %rsp
    movl    $(TRAMP + OFF_ENTRY), %eax
    movq    (%rax), %rax
    xorq    %rbp, %rbp
    jmp     *%rax                   /* -> vibeos_x86_64_ap_main, never returns */

vibeos_ap_trampoline_end:

    .section .note.GNU-stack,"",@progbits
