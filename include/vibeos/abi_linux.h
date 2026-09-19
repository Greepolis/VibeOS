#ifndef VIBEOS_ABI_LINUX_H
#define VIBEOS_ABI_LINUX_H

/* Linux x86-64 syscall numbers this kernel knows about. Taken from
 * arch/x86/entry/syscalls/syscall_64.tbl, not from memory. The two numbers at
 * 1000 and above are VibeOS's own and deliberately outside the Linux number
 * space, so they can never collide with a real syscall implemented later.
 *
 * Which of the kernel's calls each number means lives in kernel/abi/abi_linux.c;
 * this header is only the numbers. */
#define LSYS_read   0
#define LSYS_write  1
#define LSYS_brk    12
#define LSYS_mmap   9
#define LSYS_getpid 39
#define LSYS_exit   60
#define LSYS_exit_group 231
#define LSYS_fork   57
#define LSYS_vfork  58
#define LSYS_wait4  61
#define LSYS_execve 59
#define LSYS_open   2
#define LSYS_close  3
#define LSYS_lseek  8
#define LSYS_getdents64 217
#define LSYS_unlink 87
#define LSYS_mkdir  83
#define LSYS_socket   41
#define LSYS_connect  42
#define LSYS_accept   43
#define LSYS_sendto   44
#define LSYS_recvfrom 45
#define LSYS_bind     49
#define LSYS_listen   50
#define LSYS_netctl   1000
#define LSYS_pageinfo 1001
#define LSYS_mprotect       10
#define LSYS_munmap         11
#define LSYS_rt_sigaction   13
#define LSYS_rt_sigprocmask 14
#define LSYS_ioctl          16
#define LSYS_readv          19
#define LSYS_writev         20
#define LSYS_sched_yield    24
#define LSYS_uname          63
#define LSYS_getuid        102
#define LSYS_getgid        104
#define LSYS_geteuid       107
#define LSYS_getegid       108
#define LSYS_arch_prctl    158
#define LSYS_gettid        186
#define LSYS_futex         202
#define LSYS_set_tid_address 218
#define LSYS_clock_gettime 228
#define LSYS_set_robust_list 273
#define LSYS_prlimit64     302
#define LSYS_getrandom     318
#define LSYS_rseq          334
#define LSYS_fstat           5
#define LSYS_sendfile       40
#define LSYS_getcwd         79
#define LSYS_setuid        105
#define LSYS_setgid        106
#define LSYS_prctl         157
#define LSYS_openat        257
#define LSYS_newfstatat    262
#define LSYS_readlinkat    267
#define LSYS_kill           62
#define LSYS_tgkill        234
#define LSYS_tkill         200
#define LSYS_dup            32
#define LSYS_dup2           33
#define LSYS_pipe           22
#define LSYS_pipe2         293
#define LSYS_time          201
#define LSYS_clone          56
#define LSYS_getppid       110
#define LSYS_setpgid       109
#define LSYS_getpgrp       111
#define LSYS_setsid        112
#define LSYS_getsid        124
#define LSYS_rt_sigreturn   15

/* clone() flags that decide whether it is a fork or a thread. */
#define CLONE_VM     0x00000100u
#define CLONE_FS     0x00000200u
#define CLONE_FILES  0x00000400u
#define CLONE_SIGHAND 0x00000800u
#define CLONE_THREAD 0x00010000u
#define CLONE_SYSVSEM 0x00040000u
#define CLONE_SETTLS 0x00080000u
#define CLONE_PARENT_SETTID  0x00100000u
#define CLONE_CHILD_CLEARTID 0x00200000u
#define CLONE_CHILD_SETTID   0x01000000u

/* A number no Linux kernel has: the boot asks for it on purpose, so the count of
 * unimplemented calls is seen moving (see [ABI] MUSTBEZERO). */
#define VIBEOS_ABI_PROBE_NR 1999u

#endif
