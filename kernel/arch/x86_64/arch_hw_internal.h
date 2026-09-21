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

#include "vibeos/arch_x86_64.h"
#include "vibeos/task_ident.h"
#include "vibeos/fdtable.h"
#include "vibeos/pipe.h"
#include "vibeos/trap.h"
#include "vibeos/boot.h"
#include "vibeos/mm.h"
#include "vibeos/elf.h"
#include "vibeos/services.h"
#include "vibeos/exec_stats.h"
#include "vibeos/account.h"
#include "vibeos/forkguard.h"
#include "vibeos/sched_policy.h"
#include "vibeos/pageinfo.h"
#include "vibeos/rmap.h"
#include "vibeos/reclaim.h"
#include "vibeos/mbz.h"
#include "vibeos/abi.h"
#include "vibeos/abi_linux.h"
#include "vibeos/ceildiv.h"
#include "vibeos/blkdev.h"
#include "vibeos/io_stats.h"
#include "vibeos/blockdev.h"
#include "vibeos/partition.h"
#include "vibeos/parttab.h"
#include "vibeos/ext2.h"
#include "vibeos/iso9660.h"
#include "vibeos/exfat.h"
#include "vibeos/ntfs.h"
#include "vibeos/logsink.h"
#include "vibeos/storage.h"
#include "vibeos/swapmap.h"
#include "vibeos/anon.h"
#include "vibeos/swaparea.h"
#include "vibeos/frame.h"
#include "vibeos/vmspace.h"
#include "vibeos/backing.h"
#include "vibeos/task_stats.h"
#include "vibeos/runq.h"
#include "vibeos/lifetime.h"
#include "vibeos/vfs.h"
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
#define VIBEOS_EINTR  4
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

#define VIBEOS_HW_WBUF VIBEOS_FD_WBUF

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

#define VIBEOS_HW_MAX_TASKS 32  /* kernel + user processes + one idle task per CPU */
#define VIBEOS_HW_MAX_FDS ((int)VIBEOS_FD_SLOTS)
#define VIBEOS_HW_NSIG 65
/* The pending/blocked masks are uint64_t keyed by signal number, so bit 63
 * (signal 63) is the highest that exists - bit 64 does not, and 1ull << 64
 * is undefined. The kernel therefore supports signals 1..63 and refuses 64
 * rather than accept it into a bit that cannot hold it (M-017). The arrays
 * above stay sized NSIG; only signal-number validation uses this. */
#define VIBEOS_HW_SIG_MAX 63u

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
    /* The program break, the mapping cursor and the region list used to live
     * here, and clone copied them along with the rest of this struct - so every
     * thread had its own. They belong to the process and live in
     * hw_procstate_t now. */
    uint64_t user_sp;   /* entry rsp, atop the startup block */
    /* What execve was given. A program that wants to find itself reads
     * /proc/self/exe, and answering from the real path is the difference
     * between a correct answer and a plausible one. */
    char exe_path[64];
} hw_proc_t;

/* What belongs to a process rather than to one of its threads.
 *
 * Referenced, never copied: fork and exec create one, a thread takes a
 * reference, and exit gives it back. See g_procstate in arch_hw.c for the
 * five defects that copying produced. */
typedef struct hw_procstate {
    volatile uint32_t refs;      /* tasks pointing here; 0 means free        */
    volatile uint32_t mm_busy;   /* one address-space mutation at a time:     */
                                 /* brk, and fork's walk of this process      */
    uint64_t brk_cur;            /* current program break                    */
    volatile uint64_t mmap_cur;  /* next anonymous address, claimed by CAS   */
    /* What this process asked for, as opposed to what happens to be mapped.
     * munmap and mprotect consult this; the page tables are the consequence,
     * not the record. See kernel/mm/vma.c. */
    vibeos_vma_list_t vmas;
    uint64_t sig_handler[VIBEOS_HW_NSIG];
    uint64_t sig_restorer[VIBEOS_HW_NSIG];
    uint64_t sig_flags[VIBEOS_HW_NSIG];
    uint64_t sig_mask[VIBEOS_HW_NSIG];
    /* exit_group: claimed by the first caller, whose code the leader reports. */
    volatile uint32_t exit_group_claimed;
    volatile uint32_t exit_group;
    uint64_t exit_group_code;
} hw_procstate_t;

typedef struct {
    volatile int locked;
    uint64_t flags;   /* caller's RFLAGS, restored on release */
    /* Who holds it, for when nobody lets go.
     *
     * A spin loop that never gives up turns a deadlock into silence, and
     * silence is the most expensive failure this project has: the machine
     * stops, the log stops, and the evidence is a wedge report naming whatever
     * static function happened to precede the address. Two of those cost days.
     *
     * These are written after the lock is taken and cleared before it is
     * released, so the bound below can say which lock, whose it is, and what
     * they were doing - which turns a wedge into a panic with a name. */
    volatile int owner_cpu;
    const char *volatile owner_fn;
} hw_lock_t;

/* A descriptor is `vibeos_fd_t` (include/vibeos/fdtable.h); the old name stays so
 * the ~60 places that say hw_fd_t are not rewritten for a rename. */
typedef vibeos_fd_t hw_fd_t;

typedef struct {
    vibeos_x86_64_isr_frame_t ctx;
    hw_proc_t proc;
    hw_procstate_t *ps;   /* shared by every thread of this process; 0 = none */
    uint64_t cr3;
    const char *cr3_set_by;      /* diagnostics only; see HW_TASK_MARK */
    const char *ready_by;
    const char *aspace_killed_by;
    uint32_t alloc_seq;          /* which tenancy of this slot this is */
    uint64_t kstack_top;  /* private ring-0 stack: lets a task block in a syscall */
    /* Who this task is - ids, parentage, how it ended, its name, its pending signals:
     * the portable half (include/vibeos/task_ident.h), cleared in one place by
     * vibeos_task_identity_reset. What is left in this structure is what a context
     * switch, an address space and a descriptor table need. */
    vibeos_task_t id;

    /* Whether this task has ever been scheduled. One branch per context
     * switch, and it answered the question that moved the thread
     * investigation furthest: a thread that is created but never runs and a
     * thread that runs and exits immediately look identical from outside. */
    uint8_t ran_once;

    /* The syscall ABI this task speaks, bound when the slot is allocated and
     * never looked up per call (C4). Set in hw_task_alloc, which every way of
     * making a task passes through, so a fork or a thread inherits nothing by
     * accident and gets exactly what a new process gets. */
    const struct vibeos_abi *abi;
    /* Written from interrupt/syscall context (preemption, task exit) and read
     * by the kernel task, so it must not be cached across a wait loop. */
    volatile int state;
    /* Set while some CPU is executing this task, cleared only once its
     * context has been saved. A waker on another core can flip state to
     * READY while the task is still running here; without this flag a
     * third core would pick it up and two CPUs would run one task,
     * sharing its kernel stack. */
    volatile int on_cpu;
    /* The handlers, flags, restorers and per-signal masks are the process's and
     * live in hw_procstate_t. Pending and blocked stay here because they are the
     * thread's, which is the Linux model. */
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
    /* Open files: the table (fd 3 up) and what 0, 1 and 2 are redirected to.
     * vibeos_fdtable_t (include/vibeos/fdtable.h) owns the layout and the rules;
     * the entries are plain data, so fork copies the table by value. */
    vibeos_fdtable_t files;
    /* The x87/SSE register file, saved and restored across a context switch.
     *
     * Until this existed the kernel set CR4.OSFXSR, compiled thousands of XMM
     * instructions of its own, and contained no fxsave anywhere - so a task's
     * vector registers survived a syscall or an interrupt only by luck. clang
     * stores 16-byte stack buffers with movdqa, which is why a corrupted user
     * buffer in this system lost exactly sixteen bytes, one register wide.
     *
     * 512 bytes and 16-byte aligned because fxsave requires both; fxsave faults
     * on a misaligned destination rather than fixing it up. Per task, never
     * shared: a single global area would restore the previous task's registers
     * into this one, which is worse than not saving at all and quieter. */
    unsigned char fpu[512] __attribute__((aligned(16)));
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
/* The one place a user pointer is judged. Every syscall's pointer arguments, and
 * the kernel's own reads of user memory, go through it (defined in
 * kernel/abi/linux/dispatch.c) - so "who validates user memory" has one answer
 * and check-chokepoints.py can count it. */
int linux_user_ok(uint64_t base, uint64_t len, int write);

/* Give a child its parent's open files (fork and clone); defined in kernel/abi/linux/fs.c. */
void hw_fds_inherit(hw_task_t *child, const hw_task_t *parent);
int hw_copy_user_string(uint64_t uptr, char *dst, int max);
int hw_fd_alloc(hw_task_t *t);
hw_fd_t *hw_fd_get(uint64_t fd);

/* ---- the socket syscalls, now in linux_socket.c -------------------------- */

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

/* What it reaches back for. hw_task_exit is here because a signal whose default
 * action is death ends the task from inside the delivery path. */
void hw_log(vibeos_log_level_t level, uint32_t code, uint64_t a0, uint64_t a1,
            const char *msg);
void hw_task_exit(uint64_t code);
/* Copy `len` bytes where one side is user memory. 0, or -1 if the user side
 * faulted - which a sibling thread's munmap can cause at any moment after the
 * range was validated (H-003, H-010). */
int vibeos_uaccess_copy(void *dst, const void *src, uint64_t len);

/* ---- the seam with io_bringup.c ------------------------------------------
 *
 * Eight names it needs from arch_hw.c, and seven it provides. Kept together
 * and named as a seam so that a future cut can see what this one cost: every
 * line below is a static that had to stop being one, which is the real price
 * of splitting a file where everything could reach everything.
 */
extern vibeos_fsmount_t g_rootfs;
/* Moved out of arch_hw.c with the bitmap it sizes: a constant that describes a
 * shared object belongs beside the declaration of that object, or the two
 * files disagree about how big it is and only the linker notices. */
#define VIBEOS_HW_SWAP_SLOTS 8192u
extern uint8_t g_swap_bitmap[(VIBEOS_HW_SWAP_SLOTS + 7u) / 8u];
void *hw_alloc_page(void);
void hw_free_page_why(void *p, const char *why);
void *hw_frame_identity_map(uint64_t phys);

void hw_swap_bringup(void);
void hw_write_proof(void);
void hw_logsink_bringup(void);
void hw_fsimages_bringup(void);
void hw_scratch_bringup(void);
void hw_volumes_bringup(void);
void hw_mount_report(void);
int hw_task_set_state(int slot, vibeos_task_state_t to, const char *why);
int hw_signal_default_kills(uint32_t sig);

/* ---- shared with the Linux ABI layer (kernel/abi/linux) ---------------------
 *
 * Things arch_hw.c owns that the syscall handlers need. They used to be file-scope
 * statics in one 12,000-line file; lifting the handlers out made each of these a
 * named dependency, which is the honest size of the seam. */

/* VIBEOS_HW_TIMER_HZ is in arch_hw_internal.h. */



/* SYSRET requires user data (SS) to precede user code (CS) in the GDT, so the
 * user segments are ordered data-then-code: index 3 = data, index 4 = code. */
#define VIBEOS_HW_USER_DATA_SEL 0x1Bu   /* GDT index 3, RPL 3 */
#define VIBEOS_HW_USER_CODE_SEL 0x23u   /* GDT index 4, RPL 3 */
struct tss64 {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));
/* One of these per core, reached through GS.base. The first two fields are read
 * and written by the `syscall` trampoline in isr.S at fixed offsets 0 and 8 -
 * do not reorder them.
 *
 * GS.base is programmed once per CPU and never swapped: user code cannot change
 * it (CR4.FSGSBASE stays clear and ring-3 programs never load %gs), so the
 * kernel entry paths can rely on it without a swapgs dance. */
typedef struct hw_cpu {
    uint64_t syscall_kstack_top;  /* offset 0  - isr.S loads rsp from here */
    uint64_t user_saved_rsp;      /* offset 8  - isr.S stashes the user rsp */
    struct hw_cpu *self;          /* offset 16 - so C can find its own block */
    uint32_t lapic_id;
    uint32_t index;
    int current_task;             /* index into g_tasks, -1 before bring-up */
    int idle_task;                /* this core's idle task, -1 on the BSP */
    volatile int online;
    /* Ticks left in the current task's slice. See hw_schedule. */
    uint32_t slice_left;
    /* A kernel stack whose task has exited but whose core has not yet left it.
     *
     * The stack a task exits on is the one it is standing on. It cannot be
     * freed by the exiting task - there is nothing to run on afterwards - and
     * it must not be freed by anybody else either, which is the defect this
     * exists to close: the reaper on another core used to free it the instant
     * it saw the zombie, while the dying core was still executing the handful
     * of instructions between publishing the zombie and switching away. Those
     * instructions push. So the frame was written after it had been freed and
     * poisoned, and later handed out again - to a page table, in the run this
     * was caught on, which is how a core ends up executing 0xdead0000dead0000.
     *
     * The stack is parked here instead, and freed by this core after it is
     * demonstrably running on a different one. */
    uint64_t dead_kstack_base;
    uint32_t dead_kstack_pages;
    /* How many times this core has loaded CR3.
     *
     * With neither PCID nor global pages - and this kernel enables neither -
     * writing CR3 flushes this core's entire TLB. So a core whose count has
     * moved since some moment cannot still hold any translation from before it,
     * which is the whole quiescence argument the unmap quarantine rests on.
     * Both of those facts are load-bearing; see hw_tlb_quarantine_put. */
    volatile uint64_t cr3_generation;
    struct tss64 tss;
} hw_cpu_t;
/* The scheduler state below is shared by every core; `g_current_task` is not.
 * Making it a macro over the per-CPU block keeps every existing use site
 * (syscalls, exit, fork) correct on SMP without threading a CPU argument
 * through the whole syscall layer. */
#define g_current_task (hw_this_cpu()->current_task)
/* Both defined further down, and both needed above their definitions: this is
 * one 5000-line file, and a helper used before it is declared compiles as an
 * implicit declaration and then fails confusingly at the definition. */
/* Reasons a range is refused, so a caller can say which one it hit. */
#define HW_RANGE_OK          0u
#define HW_RANGE_NO_TASK     1u   /* no current task, or not a user one */
#define HW_RANGE_WRAP        2u
#define HW_RANGE_LEVEL0      3u   /* PML4 entry absent or not user */
#define HW_RANGE_LEAF        6u   /* the 4 KiB entry itself */
#define HW_RANGE_READONLY    7u
/* The last few copy-on-write faults, kept so a corrupted user buffer can be
 * reported together with the faults on its own page. Power of two: the slot is
 * a masked atomic increment, so several cores record without a lock. */
#define HW_COW_RING 16u
typedef struct {
    uint64_t va, rip, err, pid, handled, cpu;
} hw_cow_rec_t;
#define PTE_PRESENT 0x001ull
#define PTE_WRITE   0x002ull
#define PTE_USER    0x004ull            /* ring-3 accessible */
/* Bits 9 through 11 are ignored by the hardware and belong to the OS. This one
 * marks a page that is shared after fork and must be duplicated before it is
 * written. Without it a read-only page is indistinguishable from a page the
 * program was never allowed to write, and a genuine protection fault would be
 * silently turned into a successful write. */
#define PTE_NX      (1ull << 63)       /* no-execute; needs EFER.NXE (hw_enable_syscall) */
#define PTE_COW     0x200ull
/* User virtual layout: PML4 slot 1 (512 GiB). VibeOS programs are linked at
 * VIBEOS_HW_USER_BASE (user/prog/user.ld); their stack sits above the image. */
#define VIBEOS_HW_USER_BASE 0x8000000000ull
/* Programs no longer start on a bare stack: hw_proc_create builds the System V
 * startup block (argc, argv, envp, auxv) in the topmost stack page and reports
 * the stack pointer to enter on, which is 16-byte aligned as the ABI requires.
 * `_start` is written in assembly (user/prog/crt0.S) precisely so that state is
 * consumed correctly instead of being reinterpreted as a function frame. */
#define VIBEOS_HW_USER_STACK_PAGES 4u
#define hw_aspace_destroy(as) hw_aspace_destroy_why((as), __func__)
/* Thread-local storage base. A C runtime reaches its own thread state through
 * %fs on x86-64 - errno, the stack guard, locale - so this MSR is per task,
 * not per CPU, and has to be reloaded on every context switch. */
#define MSR_FS_BASE 0xC0000100u
/* User address-space layout (all inside the process's own PML4 slot):
 *   [USER_BASE ..]            program image (linked address)
 *   [.. USER_STACK_TOP]       stack (grows down)
 *   [USER_HEAP_BASE ..]       brk heap (grows up)
 *   [USER_MMAP_BASE ..]       anonymous mmap arena (grows up)
 */
#define VIBEOS_HW_USER_HEAP_BASE (VIBEOS_HW_USER_BASE + 0x00800000ull) /* +8 MiB  */
#define VIBEOS_HW_USER_MMAP_BASE (VIBEOS_HW_USER_BASE + 0x04000000ull) /* +64 MiB */
/* VIBEOS_HW_WBUF is in arch_hw_internal.h. */

/* Pipes.
 *
 * A pipe is a ring buffer with two ends, and what makes it a pipe rather than
 * a buffer is what happens at the edges: a reader with nothing to read waits
 * for a writer, a writer with no room waits for a reader, and a reader whose
 * writers have all closed gets end of file rather than waiting forever. Those
 * three rules are the entire difference between `ls | wc -l` printing a number
 * and hanging.
 *
 * The ends are counted, not flagged, because a descriptor can be duplicated
 * and inherited: `ls | wc` gives the write end to a child, and the parent must
 * close its own copy or the reader never sees end of file. That is the classic
 * way a shell pipeline hangs, and it is a refcount bug, not a pipe bug. */
/* Pipes are include/vibeos/pipe.h; the architecture supplies its lock. */
/* futex(): one thread per process here, so there is never another thread to
 * wake or to wait for. WAKE woke nobody, which is 0. WAIT would deadlock, and
 * EAGAIN is what Linux returns when the value already moved - an outcome every
 * caller is written to handle. Real futexes belong with real threads, not
 * before them. */
/* futex: the primitive every thread library builds its waiting on.
 *
 * The contract is deliberately odd and the oddity is the point. WAIT says
 * "sleep, but only if this word still holds the value I last saw"; the check
 * and the sleep happen together, under a lock, so a wake that arrives between
 * a thread reading the word and deciding to sleep cannot be lost. Without
 * that, a mutex hands out a lock to a thread that will never be told, which is
 * a hang and not a slowdown.
 *
 * Uncontended locks never come here at all - a library takes those with an
 * atomic instruction - so this is the path for contention and for joins.
 *
 * Waiters are matched on the address alone. Every thread that can share a
 * futex shares an address space, so the same virtual address is the same word;
 * two processes waiting on the same address in their own spaces would be
 * confused with each other, and that is a real limitation, written down rather
 * than papered over. Shared futexes across processes are not implemented.
 */
#define VIBEOS_HW_MAX_FUTEX_WAITERS VIBEOS_HW_MAX_TASKS
typedef struct {
    /* `used` and `addr` are not the same question, and conflating them was a
     * bug worth keeping the distinction for. A woken waiter still owns its
     * slot until it returns - it is reading `woken` out of it - so the waker
     * clears `addr`, which stops further wakes from matching, and leaves
     * `used` alone. Freeing on the waker's side let a new waiter take the slot
     * while the old one was still in it: the old one then cleared the new
     * one's registration on its way out, and that thread slept forever with
     * every wake passing it by. */
    uint8_t used;
    uint64_t addr;     /* 0 once woken: no further wake should match */
    /* Whose address. A futex word is named by a user virtual address, and a
     * virtual address means nothing without its process: every Linux program
     * here links at 0x400000, and a forked child has its parent's layout
     * exactly. The table was keyed by address alone, so a wake in one process
     * ended a wait in another - THREADS_C5_FUTEX_XPROC, red first. There are no
     * shared mappings in this kernel, so the process is the whole key. */
    const hw_procstate_t *ps;
    int task;
    /* Which tenancy of `task` enqueued. The slot index alone is an ABA: a
     * blocked waiter can be reaped and its slot handed to a new task, and a wake
     * matching by address would then set the wrong tenant READY - once, a task
     * that had already exited, scheduled onto a kernel stack being freed under
     * it (ready_by=futex_wake, the four-worker crash). A wake requires this to
     * still equal g_tasks[task].alloc_seq, the same tenancy check H-007 uses. */
    uint32_t seq;
    volatile int woken;
} hw_futex_waiter_t;


hw_cpu_t *hw_this_cpu(void);
uint32_t vibeos_x86_64_cpu_id(void);
extern hw_lock_t g_sched_lock;
void hw_spin_lock_preemptible(hw_lock_t *lock);
void hw_spin_unlock_preemptible(hw_lock_t *lock);
void hw_spin_lock_named(hw_lock_t *lock, const char *fn);
extern uint64_t g_ring3_write_nul;
extern hw_cow_rec_t g_cow_ring[HW_COW_RING];
extern volatile uint64_t g_abi_unimplemented;
extern volatile uint64_t g_abi_probes;
extern volatile uint64_t g_abi_last_nr;
void hw_sched_point(const char *where);
void hw_panic(const char *why);
extern uint8_t *g_exec_elf;
extern uint32_t g_exec_elf_cap;
int hw_page_put(uint64_t phys);
uint64_t hw_read_cr3(void);
void hw_write_cr3(uint64_t pml4_phys);
void hw_tlbq_drain(void);
void *hw_alloc_user_page(void);
long hw_read_file_cached(const char *path, void *buf, uint32_t cap,
                                uint32_t *out_id);
vibeos_vmspace_t hw_vm(const vibeos_hw_aspace_t *as);
int hw_map_page(vibeos_hw_aspace_t *as, uint64_t va, uint64_t pa,
                       uint64_t leaf_flags);
int hw_aspace_create(vibeos_hw_aspace_t *as);
void hw_aspace_destroy_why(vibeos_hw_aspace_t *as, const char *why);
void hw_wrmsr(uint32_t msr, uint64_t value);
int hw_exec_refuse(vibeos_exec_fail_t why, const char *path,
                          const char *detail);
int hw_proc_create(hw_proc_t *p, hw_procstate_t *ps,
                          const unsigned char *elf, uint64_t len,
                          uint64_t staged,
                          const char *const *argv, const char *const *envp,
                          const char *path, uint32_t file_id);
int hw_map_user_pages(vibeos_hw_aspace_t *as, uint64_t va, uint64_t pages);
uint64_t hw_proc_cr3(const hw_proc_t *p);
void hw_pipe_init(void);
uint64_t hw_alloc_kstack(uint64_t *out_base, uint32_t *out_pages);
hw_procstate_t *hw_procstate_new(void);
void hw_procstate_put(hw_procstate_t *ps);
extern uint32_t g_next_pid;
extern uint32_t g_console_foreground_pgid;
int hw_task_alloc_for_user(const char *what);
void hw_task_release(int i);
void hw_keyboard_wake(void);
void vibeos_linux_abi_init(void);      /* kernel/abi/linux/dispatch.c: register the syscall tables */
int hw_console_getc(void);          /* next console byte, or -1 if none yet */
void hw_console_echo(char c);        /* echo one byte to the screen */
void hw_fpu_init_area(unsigned char *area);
int hw_aspace_shared_by_other(const uint64_t *pml4, int except);
int hw_user_range_why(uint64_t va, uint64_t len, int need_write,
                             uint32_t *why);
void hw_pipe_release(hw_fd_t *f);
uint64_t *hw_pte_lookup(vibeos_hw_aspace_t *as, uint64_t va);
void hw_invlpg(uint64_t va);
int hw_aspace_copy_user(vibeos_hw_aspace_t *dst, vibeos_hw_aspace_t *src);
int hw_signal_interrupts(int task);
int hw_signal_raise(int task_index, uint32_t sig);
void hw_task_exit_group(uint64_t code);
int hw_task_by_pid(uint32_t pid);
int hw_task_by_tid(uint32_t tid);
int hw_user_addr_ok(uint64_t va);
extern hw_futex_waiter_t g_futex_waiters[VIBEOS_HW_MAX_FUTEX_WAITERS];
extern hw_lock_t g_futex_lock;
long hw_futex_wake(const hw_procstate_t *ps, uint64_t addr,
                          uint32_t count);
long vibeos_x86_64_linux_syscall(vibeos_x86_64_isr_frame_t *frame,
                                 uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3);


#endif
