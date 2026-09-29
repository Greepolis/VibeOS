#ifndef VIBEOS_ABI_LINUX_H
#define VIBEOS_ABI_LINUX_H

/* What the Linux ABI needs to name that is not a syscall row.
 *
 * Each implemented number is in the row that also names its handler (the files
 * under kernel/abi/linux, via LINUX_DEFINE_SYSCALLS). Every number, implemented
 * or not, is in the registry below. The two at 1000 and above are VibeOS's own
 * and deliberately outside the Linux number space, so they can never collide
 * with a real syscall implemented later. */

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
