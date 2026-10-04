#ifndef VIBEOS_ABI_LINUX_H
#define VIBEOS_ABI_LINUX_H

/* What the Linux ABI needs to name that is not a syscall row.
 *
 * Each implemented number is in the row that also names its handler (the files
 * under kernel/abi/linux, via LINUX_DEFINE_SYSCALLS). Every number, implemented
 * or not, is in the registry below. The two at 1000 and above are VibeOS's own
 * and deliberately outside the Linux number space, so they can never collide
 * with a real syscall implemented later. */

/* Linux errno values, returned to user space negated. */
#define VIBEOS_EPERM   1
#define VIBEOS_ENOENT  2
#define VIBEOS_ESRCH   3
#define VIBEOS_EINTR   4
#define VIBEOS_EIO     5
#define VIBEOS_E2BIG   7
#define VIBEOS_EBADF   9
#define VIBEOS_ECHILD 10
#define VIBEOS_EAGAIN 11
#define VIBEOS_ENOMEM 12
#define VIBEOS_EACCES 13
#define VIBEOS_EFAULT 14
#define VIBEOS_EBUSY  16
#define VIBEOS_EEXIST 17
#define VIBEOS_EXDEV  18
#define VIBEOS_ENODEV 19
#define VIBEOS_ENOTDIR 20
#define VIBEOS_EISDIR 21
#define VIBEOS_EINVAL 22
#define VIBEOS_ENFILE 23
#define VIBEOS_EMFILE 24
#define VIBEOS_ENOTTY 25
#define VIBEOS_EFBIG  27
#define VIBEOS_ENOSPC 28
#define VIBEOS_ESPIPE 29
#define VIBEOS_EROFS  30
#define VIBEOS_EMLINK 31
#define VIBEOS_EPIPE  32
#define VIBEOS_ERANGE 34
#define VIBEOS_EDEADLK 35
#define VIBEOS_ENAMETOOLONG 36
#define VIBEOS_ENOLCK 37
#define VIBEOS_ENOSYS 38
#define VIBEOS_ENOTEMPTY 39
#define VIBEOS_ELOOP  40
#define VIBEOS_EOVERFLOW 75
#define VIBEOS_ENOTSOCK 88
#define VIBEOS_EOPNOTSUPP 95
#define VIBEOS_EAFNOSUPPORT 97
#define VIBEOS_ESTALE 116
#define VIBEOS_ETIMEDOUT 110

/* Linux signal numbers. The pending and blocked masks are uint64_t keyed by
 * signal number, so bit 63 is the highest that exists and 64 is refused rather
 * than shifted into a bit that cannot hold it (M-017). */
#define VIBEOS_SIG_MAX 63u
#define VIBEOS_SIGHUP   1u
#define VIBEOS_SIGINT   2u
#define VIBEOS_SIGQUIT  3u
#define VIBEOS_SIGILL   4u
#define VIBEOS_SIGABRT  6u
#define VIBEOS_SIGBUS   7u
#define VIBEOS_SIGFPE   8u
#define VIBEOS_SIGKILL  9u
#define VIBEOS_SIGUSR1 10u
#define VIBEOS_SIGSEGV 11u
#define VIBEOS_SIGUSR2 12u
#define VIBEOS_SIGPIPE 13u
#define VIBEOS_SIGALRM 14u
#define VIBEOS_SIGTERM 15u
#define VIBEOS_SIGCHLD 17u
#define VIBEOS_SIGCONT 18u
#define VIBEOS_SIGSTOP 19u
#define VIBEOS_SIGXCPU 24u
#define VIBEOS_SIGXFSZ 25u
#define VIBEOS_SIGVTALRM 26u
#define VIBEOS_SIGPROF 27u
#define VIBEOS_SIGWINCH 28u

/* Dispositions that are not addresses, and SA_RESTORER: the handler entry
 * carries the address the handler returns to. */
#define SIG_DFL_ADDR 0ull
#define SIG_IGN_ADDR 1ull
#define VIBEOS_SA_RESTORER 0x04000000u
/* The handler asks that a call it interrupted be run again when it returns. */
#define VIBEOS_SA_RESTART 0x10000000u

/* Not an errno, and not Linux's to define: no program ever sees it. (Linux has
 * the same thing inside its kernel, as ERESTARTSYS, 512; the number is borrowed
 * and nothing depends on it being that one - only on it being no errno.)
 *
 * A handler returns it instead of EINTR when the wait a signal cut short is
 * one that may simply be started again: waiting for a child, reading a
 * terminal or a pipe, waiting for a lock. The dispatcher turns it into EINTR
 * and remembers the call; whoever delivers the signal decides whether the
 * program sees that EINTR or the call again (linux_signal_deliver). A wait
 * with a time limit returns plain EINTR: started again it would wait the whole
 * time over. */
#define VIBEOS_RESTART_CALL 512

/* A number no Linux kernel has: the boot asks for it on purpose, so the count of
 * unimplemented calls is seen moving (see [ABI] MUSTBEZERO). */
#define VIBEOS_ABI_PROBE_NR 1999u

/* The registry: every Linux x86-64 syscall and the answer this kernel gives it,
 * one line each in kernel/abi/linux_syscalls.def (docs/abi/, phase A1). A row
 * answers a DONE or PARTIAL number; a number without a row is answered from its
 * line - REFUSED with its errno, DEFERRED and MISSING with ENOSYS - and counted
 * by which of the three it is. */
#include <stdint.h>

typedef enum {
    VIBEOS_SYS_DONE = 0,
    VIBEOS_SYS_PARTIAL,
    VIBEOS_SYS_MISSING,
    VIBEOS_SYS_DEFERRED,
    VIBEOS_SYS_REFUSED
} vibeos_sys_state_t;

typedef struct {
    uint32_t nr;
    const char *name;
    vibeos_sys_state_t state;
    const char *phase;   /* NONE, NATIVE, L1..L9, D or R */
    int err;             /* the errno a row-less number returns; 0 with a row */
    const char *why;     /* the gap, the reason, or "" */
} vibeos_sys_entry_t;

/* The line for a number, or NULL for a number Linux does not have. */
const vibeos_sys_entry_t *vibeos_linux_syscall(uint64_t nr);
uint32_t vibeos_linux_syscall_count(void);
const vibeos_sys_entry_t *vibeos_linux_syscall_at(uint32_t index);

#endif
