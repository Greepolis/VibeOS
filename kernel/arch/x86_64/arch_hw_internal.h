#ifndef VIBEOS_ARCH_HW_INTERNAL_H
#define VIBEOS_ARCH_HW_INTERNAL_H

/* The seam between arch_hw.c and the pieces being lifted out of it.
 *
 * arch_hw.c grew to nine thousand lines because everything in it is `static`
 * and shares file-scope globals, so nothing could be moved without first
 * deciding what the rest of the file is allowed to see. This header is that
 * decision, made once and written down: what crosses the line is here, and
 * everything else stays private to whichever file holds it.
 *
 * It is deliberately not a public API. Nothing outside kernel/arch/x86_64/
 * includes it, and a declaration reaching this file should be read as a cost -
 * a thing two files must now agree about - rather than as progress. The measure
 * of a good cut is that this header grows by a little and arch_hw.c shrinks by
 * a lot.
 */

#include <stdint.h>

#include "vibeos/inet.h"
#include "vibeos/fs.h"
#include "vibeos/vma.h"
#include "vibeos/log.h"
#include "vibeos/task.h"
#include "vibeos/mm_model.h"

/* Linux errno values returned to user space (negated). */
#define VIBEOS_ENOSYS 38
#define VIBEOS_EFAULT 14
#define VIBEOS_EINVAL 22
#define VIBEOS_ENOMEM 12
#define VIBEOS_EBADF  9
#define VIBEOS_ENOENT 2
#define VIBEOS_ECHILD 10
#define VIBEOS_EAGAIN 11
#define VIBEOS_ENOTTY 25
#define VIBEOS_EPERM  1
#define VIBEOS_ESRCH  3
#define VIBEOS_EPIPE 32
#define VIBEOS_ERANGE 34
#define VIBEOS_EMFILE 24
#define VIBEOS_E2BIG  7
#define VIBEOS_EMFILE 24
#define VIBEOS_EIO    5
#define VIBEOS_ENOTDIR 20

#define VIBEOS_HW_WBUF 512

typedef struct vibeos_hw_aspace {
    uint64_t *pml4;
} vibeos_hw_aspace_t;

/* The sizes the structs below are built from, and the two types they embed.
 * They arrive here because hw_task_t does - a task holds a saved register
 * frame, an address space and a descriptor table, so moving the task moves
 * them. That is the honest cost of this first cut, and it is worth saying out
 * loud: the seam is wider than the socket code alone needed.
 */
/* The tick rate the network timeout is expressed in, and the timeout itself. */
#define VIBEOS_HW_TIMER_HZ 100u
#define VIBEOS_HW_NET_TIMEOUT_TICKS (VIBEOS_HW_TIMER_HZ * 10u)   /* 10 seconds */

#define VIBEOS_HW_MAX_TASKS 24  /* kernel + user processes + one idle task per CPU */
#define VIBEOS_HW_MAX_FDS 4
#define VIBEOS_HW_NSIG 65

/* Frame pushed by the ISR stubs, in ascending memory order. */
typedef struct vibeos_x86_64_isr_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector;
    uint64_t error_code;
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
} vibeos_x86_64_isr_frame_t;

typedef struct {
    vibeos_hw_aspace_t as;
    uint64_t entry;
    /* Where the interpreter was mapped, or 0 for a program that has none.
     * The interpreter relocates itself from this, so it is not a diagnostic:
     * without it a dynamic program faults on its first relocation. */
    uint64_t interp_base;
    uint64_t brk_cur;   /* current program break            */
    /* What this process asked for, as opposed to what happens to be mapped.
     * munmap and mprotect consult this; the page tables are the consequence,
     * not the record. See kernel/mm/vma.c. */
    vibeos_vma_list_t vmas;
    uint64_t mmap_cur;  /* next free anonymous mmap address  */
    uint64_t user_sp;   /* entry rsp, atop the startup block */
    /* What execve was given. A program that wants to find itself reads
     * /proc/self/exe, and answering from the real path is the difference
     * between a correct answer and a plausible one. */
    char exe_path[64];
} hw_proc_t;

typedef struct {
    volatile int locked;
    uint64_t flags;   /* caller's RFLAGS, restored on release */
} hw_lock_t;

typedef struct {
    int used;
    int writable;
    int dirty;
    /* Index into g_pipes, or -1. A descriptor is a pipe end when this is set;
     * `writable` then says which end. */
    int pipe;
    uint32_t cluster;
    uint32_t size;
    uint32_t pos;
    int net_sock;         /* index into the TCP/IP stack, or -1 for a file */
    uint32_t dir_index;   /* for getdents64 on a directory fd */
    /* Whether this descriptor names a directory. Determined when it is opened
     * rather than guessed later: opendir() opens the path and then fstats the
     * descriptor, and a descriptor that claims to be a regular file is refused
     * with ENOTDIR no matter what stat said about the path a moment earlier. */
    int isdir;
    char name[24];
    uint8_t wbuf[VIBEOS_HW_WBUF];
    uint32_t wlen;
} hw_fd_t;

typedef struct {
    vibeos_x86_64_isr_frame_t ctx;
    hw_proc_t proc;
    uint64_t cr3;
    const char *cr3_set_by;      /* diagnostics only; see HW_TASK_MARK */
    const char *ready_by;
    const char *aspace_killed_by;
    uint32_t alloc_seq;          /* which tenancy of this slot this is */
    uint64_t exit_code;
    /* Non-zero when this task was killed by a signal rather than exiting.
     * wait() encodes the two cases differently, and a parent that cannot tell
     * them apart reads a signal death as an ordinary exit with a large status
     * - which is how a crashed child looks like a successful one. */
    uint32_t exit_signal;
    uint64_t kstack_top;  /* private ring-0 stack: lets a task block in a syscall */
    /* `pid` is the thread id: unique per task, which is what Linux calls a
     * tid. `tgid` is the thread group - the number a program thinks of as its
     * process id, shared by every thread in it. For a single-threaded process
     * the two are equal, which is why everything worked while `pid` was the
     * only one of them.
     *
     * getpid() returns tgid and gettid() returns pid. Getting that backwards
     * is not a cosmetic error: a C library uses the pair to decide whether it
     * is signalling itself or another thread. */
    uint32_t pid;
    uint32_t tgid;
    uint32_t ppid;
    uint32_t pgid;
    uint32_t sid;
    uint32_t service_id;

    /* A thread shares its creator's address space rather than owning one, so
     * exit must not tear that space down while siblings are still running in
     * it. Whether this task is the last of its group is asked of the task
     * table, not tracked in a counter: the table is what the scheduler already
     * believes, and a second count of the same thing is a second thing that
     * can be wrong. */
    uint8_t is_thread;
    /* Whether this task has ever been scheduled. One branch per context
     * switch, and it answered the question that moved the thread
     * investigation furthest: a thread that is created but never runs and a
     * thread that runs and exits immediately look identical from outside. */
    uint8_t ran_once;

    /* CLONE_CHILD_CLEARTID: the address to zero and wake when this thread
     * exits. It is how pthread_join learns the thread is gone - the joiner
     * waits on this word, so a thread that exits without clearing it is a
     * join that never returns. */
    uint64_t clear_child_tid;
    /* Written from interrupt/syscall context (preemption, task exit) and read
     * by the kernel task, so it must not be cached across a wait loop. */
    volatile int state;
    /* Set while some CPU is executing this task, cleared only once its
     * context has been saved. A waker on another core can flip state to
     * READY while the task is still running here; without this flag a
     * third core would pick it up and two CPUs would run one task,
     * sharing its kernel stack. */
    volatile int on_cpu;
    int is_user;
    int is_idle;      /* per-CPU idle task: only run when nothing else is ready */
    int wait_input;   /* blocked in read() on stdin */
    uint8_t signal_stopped; /* stopped by SIGSTOP until SIGCONT */
    /* Set by prctl(PR_SET_NAME); reported back by PR_GET_NAME. */
    char comm[16];
    /* Signals.
     *
     * pending is a bitmask of signals raised but not yet delivered; blocked is
     * the mask the process asked to defer. Delivery happens on the way back to
     * user space, never at the point the signal is raised - raising can happen
     * from an interrupt or from another CPU, and building a signal frame on a
     * stack that is not currently in use would corrupt it.
     *
     * handler[] holds one user address per signal, plus the flags and the
     * restorer trampoline the C library supplied. SIG_DFL and SIG_IGN are
     * stored as they arrive so the default action is a property of the entry
     * rather than of a separate table that could disagree with it. */
    uint64_t sig_pending;
    uint64_t sig_blocked;
    uint64_t sig_handler[VIBEOS_HW_NSIG];
    uint64_t sig_restorer[VIBEOS_HW_NSIG];
    uint64_t sig_flags[VIBEOS_HW_NSIG];
    uint64_t sig_mask[VIBEOS_HW_NSIG];
    /* %fs base for this task, set by arch_prctl(ARCH_SET_FS). Restored on
     * every switch: leaving the previous task's value loaded would let one
     * program read and write another's thread-local state. */
    uint64_t fs_base;
    /* The tick at which this task last became runnable. Half of "how long did
     * it wait"; the other half is recorded when it is picked. Zero means
     * unknown, which the accounting reads as no wait rather than as a wait
     * since boot - the difference between a fresh task and a starved one. */
    uint64_t ready_at;
    uint64_t kstack_base;  /* for reclamation on exit */
    uint32_t kstack_pages;
    hw_fd_t fds[VIBEOS_HW_MAX_FDS];
    /* What descriptors 0, 1 and 2 currently mean. Unused entries mean the
     * console, which is where they point when nothing has redirected them.
     * Kept apart from fds[] because the console is not a table entry and a
     * shell redirects the standard three far more often than anything else. */
    hw_fd_t std_redirect[3];
} hw_task_t;

/* ---- what the lifted files may reach back for ---------------------------- */

extern hw_task_t g_tasks[];
extern vibeos_inet_t g_net;
extern hw_lock_t g_net_lock;
extern int g_net_up;
extern volatile uint64_t g_timer_ticks;

/* Which task this core is running, or negative if none.
 *
 * An accessor and not the macro arch_hw.c uses, because that macro reaches
 * through per-CPU state: exporting it would put hw_cpu_t on this header for the
 * sake of one integer. A file that has been lifted out has no business knowing
 * how this kernel finds the current core. */
int hw_current_task(void);

void hw_spin_lock(hw_lock_t *l);
void hw_spin_unlock(hw_lock_t *l);
int hw_user_range_ok(uint64_t base, uint64_t len, int write);
int hw_copy_user_string(uint64_t uptr, char *dst, int max);
int hw_fd_alloc(hw_task_t *t);
hw_fd_t *hw_fd_get(uint64_t fd);

/* ---- the socket syscalls, now in linux_socket.c -------------------------- */

long hw_sys_socket(uint64_t domain, uint64_t type);
long hw_sys_bind(uint64_t fd, uint64_t addr_uptr);
long hw_sys_listen(uint64_t fd);
long hw_sys_connect(uint64_t fd, uint64_t addr_uptr);
long hw_sys_accept(uint64_t fd, uint64_t addr_uptr);
long hw_sys_sendto(uint64_t fd, uint64_t buf, uint64_t len, uint64_t addr_uptr);
long hw_sys_recvfrom(uint64_t fd, uint64_t buf, uint64_t len, uint64_t addr_uptr);
long hw_sys_netctl(uint64_t op, uint64_t arg);
long hw_net_recv(hw_fd_t *f, uint64_t buf, uint64_t len);
long hw_net_send(hw_fd_t *f, uint64_t buf, uint64_t len);

/* Saved on the user stack across a handler. The layout is private to this
 * kernel - only the code that writes it and rt_sigreturn read it - so it holds
 * the whole trapframe rather than a Linux-compatible ucontext, which would
 * matter only to a program that inspects it. */
typedef struct {
    uint64_t magic;
    uint64_t blocked;
    vibeos_x86_64_isr_frame_t frame;
} hw_sigframe_t;

#define HW_SIGFRAME_MAGIC 0x5649424553494721ull   /* "VIBESIG!" */

#define SIG_DFL_ADDR 0ull
#define SIG_IGN_ADDR 1ull

/* The signal numbers are in arch_hw_internal.h: two files name them now. */
#define VIBEOS_SA_RESTORER 0x04000000u

/* Where a task was last handled, for the guard in hw_task_load_cpu_state.
 *
 * Three readings of this code have already been wrong about how an exited task
 * gets scheduled again, so the code stops being the source: each task records
 * the last place its cr3 was written, the last place it was made runnable, and
 * the last place its address space was destroyed. Static strings, one store
 * each - the cost is a pointer write on paths that already do far more, and
 * what it buys is the difference between a theory and a name. */
#define HW_TASK_MARK(idx, field, where) (g_tasks[idx].field = (where))

/* The architecture's names for the portable states, so one transition table
 * governs both and there is no second enum to drift. RESERVED was this file's
 * word for what the plan calls SETUP; the name stays because forty call sites
 * use it and the value is what matters. */
#define HW_TASK_FREE     VIBEOS_TASK_FREE
#define HW_TASK_READY    VIBEOS_TASK_READY
#define HW_TASK_RUNNING  VIBEOS_TASK_RUNNING
#define HW_TASK_ZOMBIE   VIBEOS_TASK_ZOMBIE
#define HW_TASK_BLOCKED  VIBEOS_TASK_BLOCKED
#define HW_TASK_RESERVED VIBEOS_TASK_SETUP

#define VIBEOS_SIGHUP   1u
#define VIBEOS_SIGINT   2u
#define VIBEOS_SIGQUIT  3u
#define VIBEOS_SIGILL   4u
#define VIBEOS_SIGABRT  6u
#define VIBEOS_SIGFPE   8u
#define VIBEOS_SIGKILL  9u
#define VIBEOS_SIGSEGV 11u
#define VIBEOS_SIGPIPE 13u
#define VIBEOS_SIGALRM 14u
#define VIBEOS_SIGTERM 15u
#define VIBEOS_SIGCHLD 17u
#define VIBEOS_SIGCONT 18u
#define VIBEOS_SIGSTOP 19u
#define VIBEOS_SIGWINCH 28u

/* SA_RESTORER: the handler entry carries the address the handler returns to. */

/* ---- signal delivery, now in linux_signal.c ------------------------------ */

int hw_signal_deliver(vibeos_x86_64_isr_frame_t *frame);
long hw_sys_rt_sigreturn(vibeos_x86_64_isr_frame_t *frame);

/* What it reaches back for. hw_task_exit is here because a signal whose default
 * action is death ends the task from inside the delivery path. */
void hw_log(vibeos_log_level_t level, uint32_t code, uint64_t a0, uint64_t a1,
            const char *msg);
void hw_task_exit(uint64_t code);
int hw_task_set_state(int slot, vibeos_task_state_t to, const char *why);
int hw_signal_default_kills(uint32_t sig);

#endif /* VIBEOS_ARCH_HW_INTERNAL_H */
