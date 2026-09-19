/* The Linux x86-64 ABI as a translator onto the kernel's vocabulary.
 *
 * The number -> operation map is a constant table, so classifying a call is one
 * array load and there is no state to initialise or to race on. Which checks an
 * operation performs is declared once in include/vibeos/abi.h and is not
 * repeated here: this file says what a number *means*, never what it *checks*.
 */

#include "vibeos/abi.h"
#include "vibeos/abi_linux.h"

#define LINUX_TABLE_SIZE 512u   /* every Linux number implemented is below this */

static const uint8_t g_by_nr[LINUX_TABLE_SIZE] = {
    [LSYS_read] = (uint8_t)VIBEOS_OP_READ,
    [LSYS_write] = (uint8_t)VIBEOS_OP_WRITE,
    [LSYS_open] = (uint8_t)VIBEOS_OP_OPEN,
    [LSYS_close] = (uint8_t)VIBEOS_OP_CLOSE,
    [LSYS_fstat] = (uint8_t)VIBEOS_OP_FSTAT,
    [LSYS_lseek] = (uint8_t)VIBEOS_OP_LSEEK,
    [LSYS_mmap] = (uint8_t)VIBEOS_OP_MAP,
    [LSYS_mprotect] = (uint8_t)VIBEOS_OP_PROTECT,
    [LSYS_munmap] = (uint8_t)VIBEOS_OP_UNMAP,
    [LSYS_brk] = (uint8_t)VIBEOS_OP_BRK,
    [LSYS_rt_sigaction] = (uint8_t)VIBEOS_OP_SIG_ACTION,
    [LSYS_rt_sigprocmask] = (uint8_t)VIBEOS_OP_SIG_PROCMASK,
    [LSYS_rt_sigreturn] = (uint8_t)VIBEOS_OP_SIG_RETURN,
    [LSYS_ioctl] = (uint8_t)VIBEOS_OP_IOCTL,
    [LSYS_readv] = (uint8_t)VIBEOS_OP_READV,
    [LSYS_writev] = (uint8_t)VIBEOS_OP_WRITEV,
    [LSYS_pipe] = (uint8_t)VIBEOS_OP_PIPE,
    [LSYS_sched_yield] = (uint8_t)VIBEOS_OP_YIELD,
    [LSYS_dup] = (uint8_t)VIBEOS_OP_DUP,
    [LSYS_dup2] = (uint8_t)VIBEOS_OP_DUP2,
    [LSYS_getpid] = (uint8_t)VIBEOS_OP_GETPID,
    [LSYS_sendfile] = (uint8_t)VIBEOS_OP_SENDFILE,
    [LSYS_socket] = (uint8_t)VIBEOS_OP_SOCKET,
    [LSYS_connect] = (uint8_t)VIBEOS_OP_CONNECT,
    [LSYS_accept] = (uint8_t)VIBEOS_OP_ACCEPT,
    [LSYS_sendto] = (uint8_t)VIBEOS_OP_SENDTO,
    [LSYS_recvfrom] = (uint8_t)VIBEOS_OP_RECVFROM,
    [LSYS_bind] = (uint8_t)VIBEOS_OP_BIND,
    [LSYS_listen] = (uint8_t)VIBEOS_OP_LISTEN,
    [LSYS_fork] = (uint8_t)VIBEOS_OP_FORK,
    [LSYS_vfork] = (uint8_t)VIBEOS_OP_FORK,
    [LSYS_execve] = (uint8_t)VIBEOS_OP_EXEC,
    [LSYS_exit] = (uint8_t)VIBEOS_OP_EXIT,
    [LSYS_wait4] = (uint8_t)VIBEOS_OP_WAIT,
    [LSYS_kill] = (uint8_t)VIBEOS_OP_KILL,
    [LSYS_uname] = (uint8_t)VIBEOS_OP_UNAME,
    [LSYS_getcwd] = (uint8_t)VIBEOS_OP_GETCWD,
    [LSYS_mkdir] = (uint8_t)VIBEOS_OP_MKDIR,
    [LSYS_unlink] = (uint8_t)VIBEOS_OP_UNLINK,
    [LSYS_getuid] = (uint8_t)VIBEOS_OP_IDENTITY_GET,
    [LSYS_getgid] = (uint8_t)VIBEOS_OP_IDENTITY_GET,
    [LSYS_geteuid] = (uint8_t)VIBEOS_OP_IDENTITY_GET,
    [LSYS_getegid] = (uint8_t)VIBEOS_OP_IDENTITY_GET,
    [LSYS_setuid] = (uint8_t)VIBEOS_OP_IDENTITY_SET,
    [LSYS_setgid] = (uint8_t)VIBEOS_OP_IDENTITY_SET,
    [LSYS_getppid] = (uint8_t)VIBEOS_OP_GETPPID,
    [LSYS_setpgid] = (uint8_t)VIBEOS_OP_SETPGID,
    [LSYS_getpgrp] = (uint8_t)VIBEOS_OP_GETPGRP,
    [LSYS_setsid] = (uint8_t)VIBEOS_OP_SETSID,
    [LSYS_getsid] = (uint8_t)VIBEOS_OP_GETSID,
    [LSYS_prctl] = (uint8_t)VIBEOS_OP_PRCTL,
    [LSYS_arch_prctl] = (uint8_t)VIBEOS_OP_ARCH_PRCTL,
    [LSYS_gettid] = (uint8_t)VIBEOS_OP_GETTID,
    [LSYS_time] = (uint8_t)VIBEOS_OP_TIME,
    [LSYS_futex] = (uint8_t)VIBEOS_OP_FUTEX,
    [LSYS_getdents64] = (uint8_t)VIBEOS_OP_GETDENTS,
    [LSYS_set_tid_address] = (uint8_t)VIBEOS_OP_SET_TID_ADDRESS,
    [LSYS_clock_gettime] = (uint8_t)VIBEOS_OP_CLOCK_GETTIME,
    [LSYS_exit_group] = (uint8_t)VIBEOS_OP_EXIT_GROUP,
    [LSYS_tkill] = (uint8_t)VIBEOS_OP_TKILL,
    [LSYS_tgkill] = (uint8_t)VIBEOS_OP_TGKILL,
    [LSYS_openat] = (uint8_t)VIBEOS_OP_OPEN_AT,
    [LSYS_newfstatat] = (uint8_t)VIBEOS_OP_STAT_AT,
    [LSYS_readlinkat] = (uint8_t)VIBEOS_OP_READLINK_AT,
    [LSYS_pipe2] = (uint8_t)VIBEOS_OP_PIPE2,
    [LSYS_set_robust_list] = (uint8_t)VIBEOS_OP_SET_ROBUST_LIST,
    [LSYS_prlimit64] = (uint8_t)VIBEOS_OP_PRLIMIT,
    [LSYS_getrandom] = (uint8_t)VIBEOS_OP_GETRANDOM,
    [LSYS_rseq] = (uint8_t)VIBEOS_OP_RSEQ,
};

_Static_assert((uint32_t)VIBEOS_OP_COUNT < 256u, "an id must fit the table's byte");

static vibeos_op_id_t linux_classify(uint64_t nr, uint64_t a1) {
    /* VibeOS's own calls, outside the Linux number space. */
    if (nr == LSYS_netctl) {
        return VIBEOS_OP_NETCTL;
    }
    if (nr == LSYS_pageinfo) {
        return VIBEOS_OP_PAGEINFO;
    }
    if (nr >= LINUX_TABLE_SIZE) {
        return VIBEOS_OP_NONE;
    }
    if (nr == LSYS_clone) {
        /* A C library does not call fork(); it calls clone() with the flags that
         * happen to mean fork. Sharing the address space *and* being a thread is
         * a thread; a private address space is a process. The other two
         * combinations are refused rather than approximated - a thread with a
         * private address space, or a vfork-like sharing without being a thread,
         * would be something that only looks like what it claims to be. */
        if ((a1 & CLONE_THREAD) != 0u) {
            return ((a1 & CLONE_VM) != 0u) ? VIBEOS_OP_THREAD_CREATE : VIBEOS_OP_NONE;
        }
        return ((a1 & CLONE_VM) != 0u) ? VIBEOS_OP_NONE : VIBEOS_OP_FORK;
    }
    return (vibeos_op_id_t)g_by_nr[nr];
}

/* Const, so it needs no lock: nothing writes it after link time. (The linker
 * files a const struct that holds a function pointer under initialised data,
 * and check-subsystem.py, which reads that, wants the discipline stated.) */
static const vibeos_abi_t g_linux = { "linux-x86_64", linux_classify };

const vibeos_abi_t *vibeos_abi_linux(void) {
    return &g_linux;
}
