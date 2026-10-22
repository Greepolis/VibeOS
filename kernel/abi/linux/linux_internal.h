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
#include "vibeos/sockops.h"
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
#include "vibeos/umem.h"   /* the address space and the hidden file every personality shares */

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

/* Sockets for the file calls (net.c, docs/abi/ L5): FIONREAD's count, and
 * readv and writev as one receive or send over the vector. */
long linux_socket_nread(vibeos_file_t *f);
long linux_socket_vec(vibeos_file_t *f, const vibeos_uiov_t *iov, uint32_t n, int write);

/* A signalfd's type (events.c): its records are Linux's layout. */
extern const vibeos_file_ops_t linux_fops_signalfd;

/* epoll (epoll.c): its type, and its entries forgotten with their description -
 * the hook is registered here, and the pool emptied, by the ABI's init. */
extern const vibeos_file_ops_t linux_fops_epoll;
void linux_epoll_init(void);

/* inotify (inotify.c): its type, the bytes a read would return now (FIONREAD),
 * and its tables emptied by the ABI's init. */
extern const vibeos_file_ops_t linux_fops_inotify;
uint32_t linux_inotify_pending(vibeos_file_t *f);
void linux_inotify_limits(uint32_t *queued, uint32_t *instances, uint32_t *watches);
void linux_inotify_init(void);

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
/* A descriptor as Linux reads one: an int, sign-extended back to 64 bits so a
 * negative one is a number no table has (#70). Upper register bits are not
 * part of it - 0x100000003 is descriptor 3 to Linux. */
#define LINUX_FD(v)        ((uint64_t)(int64_t)VIBEOS_ARG_INT(v))

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
/* A descriptor taken back if it still names `f` in tenancy `gen` (M-099). */
long linux_fd_withdraw(uint64_t fd, const vibeos_file_t *f, uint32_t gen);

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
/* A directory renamed from `from` to `to` (absolute): working directories, roots
 * and open descriptions under it follow (names.c). */
void linux_paths_moved(const char *from, const char *to);

/* The release uname reports and /proc/version repeats: one string, so the two
 * cannot disagree. */
#define VIBEOS_LINUX_RELEASE "6.1.0-vibeos"

#include "vibeos/procfs.h"

/* System V IPC (ipc.c, docs/abi/ L7): what message queues, semaphores and
 * shared memory share. An object's number is Linux's, `seq << 15 | index`;
 * LINUX_IPC_SLOTS is how many of each kind this kernel keeps, and the index
 * cycles over at least LINUX_IPC_CYCLE as Linux's does. Everything that reads
 * or changes a table runs under linux_ipc_lock. */
#define LINUX_IPC_SLOTS     32u
#define LINUX_IPC_INDEXES   32768u     /* IPCMNI: an index is below it */
#define LINUX_IPC_CYCLE     64u        /* Linux's ipc_min_cycle */
#define LINUX_IPC_SEQ_SHIFT 15u
#define LINUX_IPC_SEQ_MAX   65535u     /* INT_MAX >> 15: the sequence wraps below it */

typedef struct {
    uint8_t used;
    uint32_t id;
    int32_t key;
    uint32_t uid, gid, cuid, cgid;
    uint32_t mode;       /* the nine permission bits, and the kind's own above them */
    int64_t ctime;
} linux_ipc_obj_t;

typedef struct {
    uint32_t seq;        /* the sequence the next number carries */
    int32_t next_id;     /* msg_next_id and its kin: the number asked for, or -1 */
    uint32_t max;        /* msgmni and its kin, as written; the table is the cap */
    int32_t last_idx;    /* the index handed out last, -1 for none */
} linux_ipc_ns_t;

typedef void (*linux_ipc_put_t)(void *o, const char *s, uint32_t n);

void linux_ipc_lock(void);
void linux_ipc_unlock(void);
int64_t linux_ipc_now(void);
/* A control call's structure, judged and copied: 0, or -1 for EFAULT. */
int linux_ipc_out(uint64_t uptr, const void *src, uint32_t len);
int linux_ipc_in(void *dst, uint64_t uptr, uint32_t len);
/* The slot holding `key` (never IPC_PRIVATE's), or -1. */
int linux_ipc_lookup(const linux_ipc_obj_t *tab, uint32_t n, int32_t key);
/* A new object's slot, its number and its owner filled in; -ENOSPC. */
long linux_ipc_new(linux_ipc_ns_t *ns, linux_ipc_obj_t *tab, uint32_t n, int32_t key, uint64_t flg,
                   const vibeos_cred_t *c);
/* The slot an object's number names, or by its index (the *_STAT commands); -EINVAL. */
int linux_ipc_slot(const linux_ipc_obj_t *tab, uint32_t n, uint64_t id);
int linux_ipc_index(const linux_ipc_obj_t *tab, uint32_t n, uint64_t index);
/* May the caller have the access `flg`'s permission bits ask for: -EACCES. */
long linux_ipc_may(const linux_ipc_obj_t *o, uint32_t flg, const vibeos_cred_t *c);
/* Is the caller its owner, its creator or the superuser: -EPERM. */
long linux_ipc_owner(const linux_ipc_obj_t *o, const vibeos_cred_t *c);
void linux_ipc_perm_out(const linux_ipc_obj_t *o, linux_ipc64_perm_t *p);
void linux_ipc_set(linux_ipc_obj_t *o, const linux_ipc64_perm_t *p);
uint32_t linux_ipc_live(const linux_ipc_obj_t *tab, uint32_t n);
long linux_ipc_max_index(const linux_ipc_obj_t *tab, uint32_t n);
void linux_ipc_proc_msg(linux_ipc_put_t put, void *o);
/* /proc's listings and the numbers in them (ipc.c, sem.c, shm.c). */
void linux_ipc_putn(linux_ipc_put_t put, void *o, int64_t v, uint32_t width, uint32_t base);
void linux_ipc_puts(linux_ipc_put_t put, void *o, const char *s);
void linux_ipc_proc_sem(linux_ipc_put_t put, void *o);
void linux_ipc_proc_semlimits(linux_ipc_put_t put, void *o);
void linux_ipc_proc_shm(linux_ipc_put_t put, void *o);
const vibeos_procfs_extra_t *linux_ipc_proc_files(uint32_t *count);
void linux_ipc_reset(void);
void linux_ipc_sem_reset(void);
void linux_ipc_shm_reset(void);

/* What this kernel keeps of each kind. A message queue's text is stored in
 * full, so msgmnb and msgmax are its sizes, and Linux's defaults; a set's
 * semaphores likewise. SEMMSL is 250, Linux's old default and what every
 * program that sizes a set by it was written against, and SEMOPM 500 fits a
 * page of sembufs. */
#define LINUX_MSGMAX 8192u
#define LINUX_MSGMNB 16384u
#define LINUX_SEMMSL 250u
#define LINUX_SEMMNS (LINUX_SEMMSL * LINUX_IPC_SLOTS)
#define LINUX_SEMOPM 500u
#define LINUX_SEMVMX 32767
#define LINUX_SEMAEM 32767
#define LINUX_SHM_ALIGN 4096u   /* SHMLBA on x86-64: a page */

linux_ipc_ns_t *linux_ipc_sem_ns(void);
linux_ipc_ns_t *linux_ipc_shm_ns(void);
void linux_ipc_shmmax_text(linux_ipc_put_t put, void *o);
int linux_ipc_shmmax_write(const char *buf, uint32_t len);
void linux_ipc_shmall_text(linux_ipc_put_t put, void *o);
int linux_ipc_shmall_write(const char *buf, uint32_t len);

/* shm's mappings (mm.c): an attachment mapped shared from the segment's file,
 * its regions tagged with the segment's number plus one; the attachment that
 * starts at `addr` unmapped, answering with its tag; and how many attachments
 * of a tag every process has between them. */
long linux_shm_map(vibeos_file_t *f, uint64_t addr, uint64_t len, int ro, int exec, int remap,
                   uint32_t tag);
long linux_shm_unmap(uint64_t addr);
uint32_t linux_shm_attached(uint32_t tag);

/* A process's state, held for reading after the scheduler's lock is let go
 * (procsrc.c): 0 if the slot has none or it is being let go. Under the
 * scheduler's lock; given back with ks_procstate_put. */
vibeos_procstate_t *linux_ps_hold(int slot);

/* A file on /tmp that is gone with its last reference, under a name that
 * starts with `stem` (fs.c): what memfd_create and shmget make. */
int linux_ipc_parse(const char *buf, uint32_t len, int64_t *out);
void linux_mq_reset(void);
/* POSIX message queues (mqueue.c): a queue's description, and the tunables
 * under /proc/sys/fs/mqueue. */
extern const vibeos_file_ops_t linux_fops_mqueue;
void linux_mq_on_close(const vibeos_file_t *f);
#define LINUX_MQ_TUNABLE_DECL(name) \
    void linux_mq_##name##_text(linux_ipc_put_t put, void *o); \
    int linux_mq_##name##_write(const char *buf, uint32_t len);
LINUX_MQ_TUNABLE_DECL(msg_max)
LINUX_MQ_TUNABLE_DECL(msgsize_max)
LINUX_MQ_TUNABLE_DECL(msg_default)
LINUX_MQ_TUNABLE_DECL(msgsize_default)
LINUX_MQ_TUNABLE_DECL(queues_max)

/* splice (splice.c): which flags exist, and readv or writev for vmsplice (fs.c). */
#define LINUX_SPLICE_F_KNOWN (LINUX_SPLICE_F_MOVE | LINUX_SPLICE_F_NONBLOCK | LINUX_SPLICE_F_MORE | \
                              LINUX_SPLICE_F_GIFT)
long linux_vec_io(uint64_t fd, uint64_t iov_uptr, uint64_t iovcnt, int write);

#endif
