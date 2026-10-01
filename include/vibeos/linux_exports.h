#ifndef VIBEOS_LINUX_EXPORTS_H
#define VIBEOS_LINUX_EXPORTS_H

/* What the Linux ABI layer provides to the kernel that runs it.
 *
 * The other direction from vibeos/ksvc.h, and much shorter: the entry for a
 * syscall, signal delivery on the way back to user space, and the few things
 * exit and fork need from the layer that owns descriptors and futexes. Included
 * by the architecture (arch_hw_internal.h) and by the layer itself, so the two
 * cannot disagree about a signature. */

#include <stdint.h>
#include "vibeos/procstate.h"

struct ks_regs;

/* Register every syscall table; panics on a table the registry contradicts. */
void vibeos_linux_abi_init(void);

/* One syscall: its number and six arguments in the order the ABI passes them
 * (rdi, rsi, rdx, r10, r8, r9), and the trap frame for the calls that need it. */
long linux_syscall(struct ks_regs *frame, uint64_t nr, const uint64_t a[6]);

/* Called on the way back to user space. Non-zero if the frame was rewritten to
 * enter a handler; does not return if the signal kills. */
int linux_signal_deliver(struct ks_regs *frame);

/* The one place a user pointer is judged (dispatch.c). */
int linux_user_ok(uint64_t base, uint64_t len, int write);

/* Descriptors (fs.c): a fork's or exec's copy of a table (0 or -ENOMEM), and a
 * thread done with one - which closes it if that thread was the last, and returns
 * 1 if so. */
int linux_fds_copy(vibeos_procstate_t *dst, vibeos_procstate_t *src);
int linux_files_leave(vibeos_procstate_t *ps);
/* A process has ended: the record locks it held end with it (L1 step 6). */
void linux_locks_exit(uint32_t tgid);

/* Futexes (futex.c): exit wakes whoever joins the thread. One waiter per task at
 * most, so the table is sized by the task table, which the architecture asserts. */
#define LINUX_FUTEX_WAITERS 32u
long linux_futex_wake(const vibeos_procstate_t *ps, uint64_t addr, uint32_t count);

/* Counters the boot's [ABI] MUSTBEZERO line reports. */
extern volatile uint64_t g_abi_unimplemented;
extern volatile uint64_t g_abi_refused;
extern volatile uint64_t g_abi_deferred;
extern volatile uint64_t g_abi_deferred_nr;
extern volatile uint64_t g_abi_probes;
extern volatile uint64_t g_abi_last_nr;

#endif
