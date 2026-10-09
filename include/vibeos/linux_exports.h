#ifndef VIBEOS_LINUX_EXPORTS_H
#define VIBEOS_LINUX_EXPORTS_H

/* What the Linux ABI layer provides to the kernel that runs it.
 *
 * The other direction from vibeos/ksvc.h, and much shorter: the entry for a
 * syscall, signal delivery on the way back to user space, and the few things
 * exit and fork need from the layer that owns descriptors and futexes. Included
 * by the architecture (arch_hw_internal.h) and by the layer itself, so the two
 * cannot disagree about a signature. */

#include "vibeos/siginfo.h"
#include <stdint.h>
#include "vibeos/procstate.h"
#include "vibeos/procfs.h"

struct ks_regs;

/* Register every syscall table; panics on a table the registry contradicts. */
void vibeos_linux_abi_init(void);

/* One syscall: its number and six arguments in the order the ABI passes them
 * (rdi, rsi, rdx, r10, r8, r9), and the trap frame for the calls that need it. */
long linux_syscall(struct ks_regs *frame, uint64_t nr, const uint64_t a[6]);

/* Called on the way back to user space. Non-zero if the frame was rewritten to
 * enter a handler; does not return if the signal kills. */
int linux_signal_deliver(struct ks_regs *frame);

/* A CPU exception in ring 3 (docs/abi/ L2): 1 if the program has a handler for
 * `sig` and the frame now enters it; 0 and the caller kills the task. */
int linux_signal_fault(struct ks_regs *frame, uint32_t sig, const vibeos_siginfo_t *why);

/* The one place a user pointer is judged (dispatch.c). */
int linux_user_ok(uint64_t base, uint64_t len, int write);

/* Descriptors (fs.c): a fork's or exec's copy of a table (0 or -ENOMEM), and a
 * thread done with one - which closes it if that thread was the last, and returns
 * 1 if so. */
int linux_fds_copy(vibeos_procstate_t *dst, vibeos_procstate_t *src);
/* A new process state's limits and personality, as Linux starts a process
 * nothing else set them for (limits.c, L2 step 4): the architecture's
 * hw_procstate_new calls it, fork and exec copy instead. */
void linux_procstate_defaults(vibeos_procstate_t *ps);
int linux_files_leave(vibeos_procstate_t *ps);
/* A process has ended: the record locks it held end with it (L1 step 6). */
void linux_locks_exit(uint32_t tgid);

/* What /proc says about processes (procsrc.c, L2 step 6): the mount's owner
 * fills the machine's facts and asks this for the rest. */
void linux_procfs_bind(vibeos_procfs_t *pf);

/* Futexes (futex.c): exit wakes whoever joins the thread. A task waits on one
 * word, or on up to 128 at once with futex_waitv (docs/abi/ L6), so the table
 * is at least the task table - which the architecture asserts - and room for
 * a few vectors besides. Not Linux's FUTEX_WAITERS, which is a bit in a lock
 * word; the two names were one until L6 needed both. */
#define LINUX_FUTEX_TABLE 512u
long linux_futex_wake(const vibeos_procstate_t *ps, uint64_t addr, uint32_t count);
/* A thread is ending: every robust lock it held is marked OWNER_DIED and one
 * waiter on each woken (set_robust_list). Called by exit while the thread's
 * address space is still the one loaded, before the join word is cleared. */
void linux_futex_exit_robust(int slot);

/* Counters the boot's [ABI] MUSTBEZERO line reports. */
extern volatile uint64_t g_abi_unimplemented;
extern volatile uint64_t g_abi_refused;
extern volatile uint64_t g_abi_deferred;
extern volatile uint64_t g_abi_deferred_nr;
extern volatile uint64_t g_abi_probes;
extern volatile uint64_t g_abi_last_nr;

#endif
