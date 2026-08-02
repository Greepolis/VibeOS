/* Program entry for VibeOS user programs.
 *
 * This is the same _start a real libc provides, and it exists for the same
 * reason: at entry the stack is not a function's stack. It holds argc, then
 * argv, then the environment, then the auxiliary vector, exactly as the System
 * V ABI lays them out - and rsp is 16-byte aligned rather than in the
 * post-call state a compiled function assumes. A C function cannot be the
 * entry point without either reading those values wrongly or faulting on the
 * first aligned SSE store the optimizer emits.
 *
 * So the transition is done here, in the only place where the raw entry state
 * is visible, and every program below it is an ordinary C function.
 */

    .text
    .globl _start
    .type _start, @function
_start:
    /* Terminate the frame-pointer chain: nothing called us, and a debugger
     * walking past this point would follow garbage. */
    xorl    %ebp, %ebp

    movq    (%rsp), %rdi            /* argc                                  */
    leaq    8(%rsp), %rsi           /* argv                                  */
    /* envp starts one slot past argv's NULL terminator, i.e. at
     * argv + (argc + 1) * 8. */
    leaq    8(%rsi,%rdi,8), %rdx

    /* The ABI wants rsp % 16 == 0 at a call site. It already is, but a stack
     * built by anything else must not be trusted to be, and the cost of being
     * certain is one instruction. */
    andq    $-16, %rsp

    call    vibeos_main

    /* main's return value is the exit status. */
    movq    %rax, %rdi
    movq    $60, %rax               /* SYS_exit */
    syscall

    /* exit does not return. If it somehow does, stop here rather than falling
     * into whatever bytes follow. */
1:  hlt
    jmp     1b
    .size _start, . - _start

    .section .note.GNU-stack,"",@progbits
