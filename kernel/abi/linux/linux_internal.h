#ifndef VIBEOS_LINUX_INTERNAL_H
#define VIBEOS_LINUX_INTERNAL_H

/* What the files in kernel/abi/linux share with each other.
 *
 * The Linux syscall handlers used to live in arch_hw.c as file-scope statics, and
 * after they were lifted out (C4) they still reached the architecture through its
 * private header. They do not any more (docs/abi/ phase A2): everything a handler
 * asks of the kernel is in vibeos/ksvc.h, and nothing here names a register, a
 * page-table bit or the task table. That is what lets the same files build into
 * the host test binary against tests/kernel/ksvc_fake.c. */

#include <stdint.h>
#include "vibeos/ksvc.h"
#include "vibeos/linux_exports.h"
#include "vibeos/abi_linux.h"
#include "vibeos/abi_rows.h"
#include "vibeos/procstate.h"
#include "vibeos/fdtable.h"
#include "vibeos/fileops.h"
#include "vibeos/filelock.h"
#include "vibeos/tty.h"
#include "vibeos/linux_layout.h"
#include "vibeos/pipe.h"
#include "vibeos/vma.h"
#include "vibeos/mm_stats.h"
#include "vibeos/swapmap.h"
#include "vibeos/swaparea.h"
#include "vibeos/task_stats.h"
#include "vibeos/lifetime.h"
#include "vibeos/ceildiv.h"
#include "vibeos/frame.h"
#include "vibeos/ptimer.h"
#include "vibeos/account.h"

/* A Linux sigset_t numbers its bits from zero, this kernel by signal; the two
 * meet only through these (see sig.c). */
static inline uint64_t linux_sigset_from_user(uint64_t user_set) { return user_set << 1; }
static inline uint64_t linux_sigset_to_user(uint64_t kernel_set) { return kernel_set >> 1; }

/* Is this stack pointer on the thread's alternate signal stack (signal.c)? */
int linux_on_altstack(const vibeos_task_t *t, uint64_t sp);
/* Whether one more per-signal line goes to the console (signal.c). */
int linux_sig_chatty(void);

/* A timespec as ticks, rounded up; -1 if it is not a time (misc.c). */
int64_t linux_ticks_of(const linux_timespec_t *ts);

/* The clocks (timer.c): what one reads now in ticks, -1 for a clock this
 * kernel does not know; and the CPU time a process or a thread has run. */
int64_t linux_clock_read(uint64_t clk);
uint64_t linux_cpu_of_process(uint32_t tgid);
uint64_t linux_cpu_of_thread(int slot);
uint64_t linux_cpu_slot(int slot);

/* What a description can do now, in Linux's poll bits, for `events` - with the
 * hangup and the error whether asked or not (poll.c, docs/abi/ L4). */
uint32_t linux_revents(vibeos_file_t *f, uint32_t events);

/* A signalfd's type (events.c): its records are Linux's layout. */
extern const vibeos_file_ops_t linux_fops_signalfd;

/* epoll (epoll.c): its type, and its entries forgotten with their description -
 * the hook is registered here, and the pool emptied, by the ABI's init. */
extern const vibeos_file_ops_t linux_fops_epoll;
void linux_epoll_init(void);

/* poll.c's engine, which epoll waits on too: look until something is ready, the
 * time (ticks; < 0 for ever, 0 once) is up or a signal needs acting on; and the
 * mask a call waits under, swapped in and given back as rt_sigsuspend does. */
typedef long (*linux_look_t)(void *ctx);
long linux_wait_ready(linux_look_t look, void *ctx, int64_t ticks, uint64_t *left);
void linux_mask_swap(uint64_t raw);
void linux_mask_back(long r);

/* The pool's entropy as Linux reports it, in bits, for RNDGETENTCNT and
 * /proc/sys/kernel/random/entropy_avail alike (procsrc.c). */
uint32_t linux_entropy_avail(void);

/* Limits (limits.c, L2 step 4): RLIMIT_CPU's two timers for a process that has
 * run `ran` ticks, and whether the caller may have one more task. */
void linux_rlimit_cpu_arm(uint32_t tgid, const vibeos_procstate_t *ps, uint64_t ran);
long linux_nproc_check(void);

/* The process a pidfd names, by pid (sig.c, L2 step 5); EBADF if not one. */
long linux_pidfd_pid(uint64_t fd);

/* A reason in Linux's words: the siginfo_t a handler or sigtimedwait sees. */
void linux_siginfo_from(linux_siginfo_t *o, uint32_t sig, const vibeos_siginfo_t *in);

/* Rows are declared with the macros in vibeos/abi_rows.h, which every
 * personality shares; LINUX_DEFINE_SYSCALLS is the Linux spelling. */
#define LINUX_DEFINE_SYSCALLS(topic, LIST) VIBEOS_DEFINE_SYSCALLS(linux, topic, LIST)

#define VIBEOS_ARG_INT(v)  ((int)(uint32_t)(v))

/* How many descriptors a process may have: RLIMIT_NOFILE, as prlimit reports it. */
#define LINUX_MAX_FDS ((int)VIBEOS_FD_MAX)


/* The exec staging cache (common.c): which image the staging buffer holds. */
extern char g_exec_cached[128];
extern long g_exec_cached_len;
extern uint32_t g_exec_cached_id;
void linux_exec_cache_drop(void);

/* Descriptors (fs.c), shared with the socket calls: a reference to what fd names
 * (the caller puts it), a description installed at the lowest free number at or
 * above min (the reference is taken over, or released on failure), and a close. */
vibeos_file_t *linux_file_get(uint64_t fd);
long linux_fd_install(vibeos_file_t *f, uint32_t fdflags, uint32_t min);
long linux_fd_close(uint64_t fd);

/* A path argument walked (fs.c, docs/abi/ A4 and L1): relative to the directory
 * `dirfd` names, or the working directory for LINUX_AT_FDCWD, never above the root,
 * symbolic links followed as `flags` (VIBEOS_PATH_*) says. 0, or a negated errno. */
long linux_walk_at(uint64_t dirfd, uint64_t upath, uint32_t flags, vibeos_path_t *w);

/* The file a descriptor names, walked from the path its description remembers
 * (AT_FDCWD: the working directory), and a path that may be empty under
 * AT_EMPTY_PATH - then `dirfd` is the file. */
long linux_walk_fd(uint64_t fd, vibeos_path_t *w);
long linux_walk_at_empty(uint64_t dirfd, uint64_t upath, uint64_t atflags, uint32_t flags,
                         vibeos_path_t *w);

/* What fs.c and names.c share (docs/abi/ L1 step 5): what stat reports for a
 * name or a descriptor and the device it is on, open by another name, the
 * umask, and rmdir for unlinkat's AT_REMOVEDIR. */
long linux_stat_get(uint64_t dirfd, uint64_t path_uptr, uint64_t atflags,
                    vibeos_file_stat_t *st, uint64_t *dev);
uint64_t linux_dev_of(const vibeos_fsmount_t *mnt);
long linux_sys_openat(uint64_t dirfd, uint64_t path_uptr, uint64_t flags, uint64_t mode);
uint32_t linux_umask(void);

/* Who is asking (docs/abi/ L2 step 1): a copy of the caller's credentials, and
 * the questions the file handlers put to them. All answer 0 for the superuser
 * without looking anything up.
 *
 *   linux_may          may the caller do `want` (VIBEOS_MAY_*) to this file?
 *   linux_may_add      may it make a new name where `w` says one would go?
 *   linux_may_remove   may it take away the name `w` found?  (the directory's
 *                      permission, and the sticky bit's rule in a shared one)
 *   linux_may_own      is it the file's owner, or the superuser?  (-EPERM)
 *   linux_own_new      give a file the caller just made its owner and group
 */
void linux_cred(vibeos_cred_t *out);
long linux_may(const vibeos_fs_node_t *node, uint32_t want);
long linux_may_add(const vibeos_path_t *w);
long linux_may_remove(const vibeos_path_t *w);
long linux_may_own(const vibeos_fs_node_t *node);
void linux_own_new(const vibeos_path_t *w);
long linux_rmdir_at(uint64_t dirfd, uint64_t path_uptr);

/* The release uname reports and /proc/version repeats: one string, so the two
 * cannot disagree. */
#define VIBEOS_LINUX_RELEASE "6.1.0-vibeos"

#endif
/* A directory renamed from `from` to `to` (absolute): working directories, roots
 * and open descriptions under it follow (names.c). */
void linux_paths_moved(const char *from, const char *to);
