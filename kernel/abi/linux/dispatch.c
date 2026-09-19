/* Linux ABI: the dispatcher - classify the call, then run it.
 *
 * Lifted out of arch_hw.c (C4 stage 3). Nothing here is new: the handlers and the
 * helpers only they use, moved as they were. */

#include "linux_internal.h"

/* Linux ABI entry: nr in rax, args in rdi/rsi/rdx, with the full trapframe
 * available (fork needs it). Reached from both the native `syscall` trampoline
 * and the int 0x80 gate. Each number is offered to the Linux personality
 * (user/compat/linux) so the portable, host-tested translation model sees and
 * accounts for every real syscall. */
long vibeos_x86_64_linux_syscall(vibeos_x86_64_isr_frame_t *frame,
                                 uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3) {
    /* The ABI was bound to this task when it was created; it is not looked up
     * per call. A task with no ABI recorded (there is none on the live path) is
     * treated as Linux, the only one there is. */
    const vibeos_abi_t *abi = (g_current_task >= 0 && g_tasks[g_current_task].abi)
                                  ? g_tasks[g_current_task].abi
                                  : vibeos_abi_linux();
    vibeos_op_id_t id = abi->classify(nr, a1);

    switch (id) {
        case VIBEOS_OP_FORK:
            /* Reached by fork, by vfork and by a clone() whose flags mean fork.
             * ash uses vfork for the short child->exec path. A real vfork
             * shares the parent's address space until exec, but returning a
             * private fork here preserves the observable contract while
             * avoiding a parent that can be modified by a still-running child.
             * The child is constrained by the same exec/exit ABI as vfork's
             * supported use in this personality. */
            return hw_sys_fork(frame);
        case VIBEOS_OP_EXEC:
            return hw_sys_execve(frame, a1, a2, a3);
        case VIBEOS_OP_OPEN:
            return hw_sys_open(a1, a2);
        case VIBEOS_OP_CLOSE:
            return hw_sys_close(a1);
        case VIBEOS_OP_LSEEK:
            return hw_sys_lseek(a1, a2, a3);
        case VIBEOS_OP_GETDENTS:
            return hw_sys_getdents64(a1, a2, a3);
        case VIBEOS_OP_UNLINK:
            return hw_sys_unlink(a1);
        case VIBEOS_OP_MKDIR:
            return hw_sys_mkdir(a1);
        case VIBEOS_OP_WAIT:
            return hw_sys_waitpid(a1, a2, a3);
        case VIBEOS_OP_WRITE:
            return hw_sys_write(a1, a2, a3);
        case VIBEOS_OP_READ:
            return hw_sys_read(a1, a2, a3);
        case VIBEOS_OP_BRK:
            return hw_sys_brk(a1);
        case VIBEOS_OP_MAP:
            /* addr, len, prot in the first three; flags and fd are in r10 and
             * r8, which the trapframe carries. */
            return hw_sys_mmap(a1, a2, a3, frame->r10, frame->r8);
        case VIBEOS_OP_PROTECT:
            return hw_sys_mprotect(a1, a2, a3);
        case VIBEOS_OP_UNMAP:
            return hw_sys_munmap(a1, a2);
        case VIBEOS_OP_GETPID:
            /* The thread group, not the thread. Every thread of a program
             * gets the same answer here, which is the whole point of the
             * distinction: getpid() names the process. */
            return (g_current_task >= 0) ? (long)g_tasks[g_current_task].tgid : 1;
        case VIBEOS_OP_SETPGID:
            return hw_sys_setpgid(a1, a2);
        case VIBEOS_OP_GETPGRP:
            return (g_current_task >= 0 && g_tasks[g_current_task].is_user) ?
                (long)g_tasks[g_current_task].pgid : -VIBEOS_EINVAL;
        case VIBEOS_OP_SETSID:
            return hw_sys_setsid();
        case VIBEOS_OP_GETSID:
            return hw_sys_getsid(a1);

        /* The opening sequence of a real C runtime. */
        case VIBEOS_OP_ARCH_PRCTL:
            return hw_sys_arch_prctl(a1, a2);
        case VIBEOS_OP_IOCTL:
            return hw_sys_ioctl(a1, a2, a3);
        case VIBEOS_OP_WRITEV:
            return hw_sys_writev(a1, a2, a3);
        case VIBEOS_OP_READV:
            return hw_sys_readv(a1, a2, a3);
        case VIBEOS_OP_UNAME:
            return hw_sys_uname(a1);
        case VIBEOS_OP_CLOCK_GETTIME:
            return hw_sys_clock_gettime(a1, a2);
        case VIBEOS_OP_PRLIMIT:
            return hw_sys_prlimit64(a2, a3, frame->r10);
        case VIBEOS_OP_FUTEX:
            return hw_sys_futex(a1, a2, a3);
        case VIBEOS_OP_GETTID:
            /* The thread id proper. Equal to getpid() for a single-threaded
             * program, which is what Linux reports too, and different for
             * every thread of a program that has several. */
            return (g_current_task >= 0) ? (long)g_tasks[g_current_task].pid : 1;
        case VIBEOS_OP_SET_TID_ADDRESS:
            /* Where to write zero and wake when this thread exits. A joiner
             * sleeps on that word, so recording it is half of what makes
             * pthread_join return; the other half is exit doing the writing. */
            if (g_current_task >= 0) {
                g_tasks[g_current_task].clear_child_tid = a1;
                return (long)g_tasks[g_current_task].pid;
            }
            return 1;
        case VIBEOS_OP_SET_ROBUST_LIST:
            /* The list is walked by the kernel when a thread dies holding a
             * robust mutex. No threads, no robust mutexes, nothing to walk. */
            return 0;
        case VIBEOS_OP_RSEQ:
            /* Restartable sequences are an optimisation with a mandatory
             * fallback. Reporting ENOSYS makes the libc take that fallback;
             * claiming success would make it run a fast path this kernel does
             * not implement. */
            return -VIBEOS_ENOSYS;
        case VIBEOS_OP_GETRANDOM:
            /* There is no entropy source here yet. Returning predictable bytes
             * from the syscall a program uses for keys is worse than refusing:
             * ENOSYS is visible, weak randomness is not. */
            return -VIBEOS_ENOSYS;
        case VIBEOS_OP_SIG_ACTION:
            return hw_sys_rt_sigaction(a1, a2, a3);
        case VIBEOS_OP_SIG_PROCMASK:
            return hw_sys_rt_sigprocmask(a1, a2, a3);
        case VIBEOS_OP_YIELD:
            /* Give up the rest of this slice honestly: hlt parks the CPU until
             * the next timer interrupt, which is where the switch happens. */
            __asm__ __volatile__("sti; hlt");
            return 0;
        /* What a program does once it is running. */
        case VIBEOS_OP_FSTAT:
            return hw_sys_fstat(a1, a2);
        case VIBEOS_OP_STAT_AT:
            return hw_sys_newfstatat(a1, a2, a3, frame->r10);
        case VIBEOS_OP_OPEN_AT:
            return hw_sys_openat(a1, a2, a3);
        case VIBEOS_OP_PIPE:
            return hw_sys_pipe2(a1, 0);
        case VIBEOS_OP_PIPE2:
            return hw_sys_pipe2(a1, a2);
        case VIBEOS_OP_DUP2:
            return hw_sys_dup2(a1, a2);
        case VIBEOS_OP_DUP: {
            /* dup() is dup2() onto the lowest free descriptor. */
            hw_task_t *dt;
            int i;
            if (g_current_task < 0) {
                return -VIBEOS_EINVAL;
            }
            dt = &g_tasks[g_current_task];
            for (i = 0; i < VIBEOS_HW_MAX_FDS; i++) {
                if (!dt->fds[i].used) {
                    return hw_sys_dup2(a1, (uint64_t)(3 + i));
                }
            }
            return -VIBEOS_EMFILE;
        }
        case VIBEOS_OP_GETCWD:
            return hw_sys_getcwd(a1, a2);
        case VIBEOS_OP_READLINK_AT:
            return hw_sys_readlinkat(a1, a2, a3, frame->r10);
        case VIBEOS_OP_PRCTL:
            return hw_sys_prctl(a1, a2);
        case VIBEOS_OP_IDENTITY_SET:   /* setuid, setgid */
            return hw_sys_setresid(a1);
        case VIBEOS_OP_TIME:
            return hw_sys_time(a1);
        case VIBEOS_OP_THREAD_CREATE:
            /* Which flag combinations are a thread, and which are refused, is
             * the Linux ABI's decision (abi_linux.c classify); this is only the
             * operation. */
            return hw_sys_clone_thread(frame, a1, a2, a3,
                                       frame->r10, frame->r8);
        case VIBEOS_OP_GETPPID:
            return (g_current_task >= 0) ? (long)g_tasks[g_current_task].ppid : 0;
        case VIBEOS_OP_SIG_RETURN:
            return hw_sys_rt_sigreturn(frame);
        case VIBEOS_OP_KILL:
            return hw_sys_kill(a1, a2);
        case VIBEOS_OP_TKILL:
            /* raise() goes through tkill, not kill: a library raising a signal
             * in itself targets its own thread, and with one thread per
             * process that is the same destination. */
            return hw_sys_tkill(a1, a2);
        case VIBEOS_OP_TGKILL:
            /* tgkill(tgid, tid, sig): the thread named by tid, provided it
             * still belongs to tgid - the check that stops a recycled thread id
             * from reaching a different process. */
            return hw_sys_tgkill(a1, a2, a3);
        case VIBEOS_OP_SENDFILE:
            /* Every caller of sendfile has to cope with it failing, and does:
             * a read-and-write loop is the documented fallback. Refusing is
             * therefore free, while serving it would mean a second copy of the
             * file and console paths purely to move bytes between kernel
             * buffers. */
            return -VIBEOS_ENOSYS;

        case VIBEOS_OP_IDENTITY_GET:   /* getuid, geteuid, getgid, getegid */
            /* Everything runs as the one identity this system has. */
            return 0;
        /* Sockets. The Linux ABI passes the 4th, 5th and 6th arguments in r10,
         * r8 and r9; the trapframe has them, so read them straight from it. */
        case VIBEOS_OP_SOCKET:
            return hw_sys_socket(a1, a2);
        case VIBEOS_OP_CONNECT:
            return hw_sys_connect(a1, a2);
        case VIBEOS_OP_ACCEPT:
            return hw_sys_accept(a1, a2);
        case VIBEOS_OP_SENDTO:
            return hw_sys_sendto(a1, a2, a3, frame->r8);
        case VIBEOS_OP_RECVFROM:
            return hw_sys_recvfrom(a1, a2, a3, frame->r8);
        case VIBEOS_OP_BIND:
            return hw_sys_bind(a1, a2);
        case VIBEOS_OP_LISTEN:
            return hw_sys_listen(a1);
        case VIBEOS_OP_NETCTL:
            return hw_sys_netctl(a1, a2);
        case VIBEOS_OP_PAGEINFO:
            return hw_sys_pageinfo(a1, a2);
        case VIBEOS_OP_EXIT:
            hw_task_exit(a1); /* retires this task and switches away; no return */
            return 0;
        case VIBEOS_OP_EXIT_GROUP:
            hw_task_exit_group(a1); /* the whole process; no return */
            return 0;
        case VIBEOS_OP_NONE:
        default:
            __sync_fetch_and_add(&g_abi_unimplemented, 1u);
            g_abi_last_nr = nr;   /* the witness: which number, not just how many */
            if (nr == VIBEOS_ABI_PROBE_NR) {
                __sync_fetch_and_add(&g_abi_probes, 1u);
                return -VIBEOS_ENOSYS;   /* asked for on purpose; no log line */
            }
            /* One line, one critical section: puts and print_hex each take the console lock on their own. */
            vibeos_x86_64_serial_lock();
            vibeos_x86_64_serial_puts("[HW][SYS] unimplemented Linux syscall nr=0x");
            vibeos_x86_64_serial_print_hex(nr);
            vibeos_x86_64_serial_puts("\n");
            vibeos_x86_64_serial_unlock();
            return -VIBEOS_ENOSYS;
    }
}
