/* Linux ABI: creating, running, waiting for and ending tasks.
 *
 * What a C runtime asks for before it runs the program, and what it does to
 * create and reap tasks. These are not conveniences. A static libc executes a
 * fixed opening sequence and dies if any of it fails: it installs thread-local
 * storage, registers a thread id, asks whether its output is a terminal, and only
 * then reaches main. Each handler either does the real thing or returns the error
 * Linux returns, because a syscall that reports a success it did not perform makes
 * the program fail later, somewhere unrelated, with nothing pointing back here.
 *
 * Lifted out of arch_hw.c (C4 stage 3), moved as it was. */

#include "linux_internal.h"

static hw_lock_t g_exec_lock;

#define HW_RANGE_LEVEL1      4u

#define HW_RANGE_LEVEL2      5u

/* The codes as words, because a number in a log has to be looked up and a
 * reason that has to be looked up is one people stop reading. Which level of
 * the walk failed is the whole diagnostic here: an absent PML4 entry and a leaf
 * that is present but not user-accessible are completely different bugs. */
static const char *hw_range_why_name(uint32_t why) {
    switch (why) {
        case HW_RANGE_OK:       return "ok";
        case HW_RANGE_NO_TASK:  return "no_current_user_task";
        case HW_RANGE_WRAP:     return "address_wrapped";
        case HW_RANGE_LEVEL0:   return "pml4_absent_or_not_user";
        case HW_RANGE_LEVEL1:   return "pdpt_absent_or_not_user";
        case HW_RANGE_LEVEL2:   return "pd_absent_or_not_user";
        case HW_RANGE_LEAF:     return "leaf_absent_or_not_user";
        case HW_RANGE_READONLY: return "leaf_not_writable";
        default:                return "?";
    }
}

static int hw_exec_cache_hit(const char *path) {
    uint32_t i;
    if (g_exec_cached_len <= 0) {
        return 0;
    }
    for (i = 0; i < sizeof(g_exec_cached); i++) {
        if (g_exec_cached[i] != path[i]) {
            return 0;
        }
        if (path[i] == 0) {
            return 1;
        }
    }
    return 0;
}

/* prctl operations. PR_SET_NAME is the one a real program actually uses. */
#define PR_SET_NAME 15

#define PR_GET_NAME 16

/* arch_prctl subfunctions. */
#define ARCH_SET_GS 0x1001

#define ARCH_SET_FS 0x1002

#define ARCH_GET_FS 0x1003

#define ARCH_GET_GS 0x1004

/* futex operations we can answer honestly. */
#define FUTEX_WAIT 0

#define FUTEX_WAKE 1

#define FUTEX_CMD_MASK 0x7F

/* fork(): duplicate the calling task, address space and all. The child resumes
 * at the same instruction with a 0 return value. */
static long hw_sys_fork(const vibeos_x86_64_isr_frame_t *frame) {
    hw_task_t *parent;
    hw_task_t *child;
    int idx;
    uint32_t my_tenancy;

    if (g_current_task < 0 || !g_tasks[g_current_task].id.is_user) {
        return -VIBEOS_EINVAL;
    }

    /* The guard and the allocation, one critical section. EAGAIN rather than
     * ENOMEM: the refusal is temporary by nature, since a child exiting undoes
     * it, and that is what a C library turns into "resource temporarily
     * unavailable" and what a shell retries on. */
    idx = hw_task_alloc_for_user("fork");
    if (idx < 0) {
        /* DEBUG, not WARN. hw_task_alloc_for_user already prints a line
         * naming which rule refused, which is strictly more than this said -
         * and a warning that a service provokes on purpose is a warning people
         * learn to scroll past. It stays in the ring for a failure that turns
         * out to be about one of these. */
        hw_log(VIBEOS_LOG_DEBUG, 7u, 0, 0,
               "fork refused: guard or no free task slot");
        return -VIBEOS_EAGAIN;
    }
    parent = &g_tasks[g_current_task];
    child = &g_tasks[idx];
    my_tenancy = child->alloc_seq;

    /* From here to the vma clone below reads the parent's address space - the
     * page tables and the region list - which a sibling thread's brk (and,
     * once converted, mmap/munmap/mprotect) mutates. Held so fork sees one
     * consistent state, not a walk racing a half-finished mapping. */
    hw_mm_lock(parent->ps);
    if (hw_aspace_create(&child->proc.as) != 0 ||
        hw_aspace_copy_user(&child->proc.as, &parent->proc.as) != 0) {
        /* Give the tables back before the slot is published as reusable.
         * Publishing first leaves an address space nothing will ever free -
         * the next tenant overwrites the pointer - and it leaves a FREE slot
         * naming live tables, which is a question the sharing test skips. */
        hw_mm_unlock(parent->ps);
        hw_aspace_destroy(&child->proc.as);
        (void)hw_task_set_state((int)(child - g_tasks), HW_TASK_FREE, __func__);
        return -VIBEOS_ENOMEM;
    }
    child->kstack_top = hw_alloc_kstack(&child->kstack_base, &child->kstack_pages);
    if (child->kstack_top == 0) {
        hw_mm_unlock(parent->ps);
        hw_aspace_destroy(&child->proc.as);
        (void)hw_task_set_state((int)(child - g_tasks), HW_TASK_FREE, __func__);
        return -VIBEOS_ENOMEM;
    }
    /* The child's regions are its own from the first instruction. Copying the
     * proc struct copied the list *head*, which would have left two processes
     * sharing one chain of descriptors - and the first munmap in either would
     * have unlinked descriptors out from under the other. */
    child->ps = hw_procstate_new();
    if (!child->ps || vibeos_vma_clone(&child->ps->vmas, &parent->ps->vmas) != 0) {
        hw_mm_unlock(parent->ps);
        hw_procstate_put(child->ps);
        child->ps = 0;
        hw_aspace_destroy(&child->proc.as);
        (void)hw_task_set_state((int)(child - g_tasks), HW_TASK_FREE, __func__);
        return -VIBEOS_ENOMEM;
    }
    /* The parent's tables and list have both been read; release before the
     * child-only setup below. */
    hw_mm_unlock(parent->ps);
    vibeos_task_stats()->forks++;
    child->proc.entry = parent->proc.entry;
    child->ps->brk_cur = parent->ps->brk_cur;
    __atomic_store_n(&child->ps->mmap_cur,
                     __atomic_load_n(&parent->ps->mmap_cur, __ATOMIC_ACQUIRE),
                     __ATOMIC_RELEASE);
    child->cr3 = hw_proc_cr3(&child->proc);
    child->cr3_set_by = "fork";
    child->ctx = *frame;   /* resume exactly where the parent is */
    /* Including the vector registers. "Exactly where the parent is" was only
     * ever true of the integer ones, and a child that resumes mid-expression
     * with somebody else's xmm is the same defect as not saving them at all. */
    {
        uint32_t fi;
        for (fi = 0; fi < 512u; fi++) {
            child->fpu[fi] = parent->fpu[fi];
        }
    }
    child->ctx.rax = 0;    /* ... but fork() returns 0 in the child */
    child->id.pid = (uint32_t)__sync_fetch_and_add(&g_next_pid, 1u);
    hw_log(VIBEOS_LOG_DEBUG, 44u, (uint64_t)parent->id.pid, (uint64_t)child->id.pid,
           "fork produced a child (a0 = parent, a1 = child)");
    /* fork makes a process, so the child heads its own thread group. Its
     * parent is the *group*, not the thread that happened to call fork:
     * wait() is a process relationship. */
    child->id.tgid = child->id.pid;
    child->id.ppid = parent->id.tgid;
    child->id.pgid = parent->id.pgid;
    child->id.sid = parent->id.sid;
    child->id.signal_stopped = 0;
    child->id.is_thread = 0;
    child->id.clear_child_tid = 0;
    child->fs_base = parent->fs_base;   /* the copied image expects its TLS */
    {
        uint32_t sg;
        /* Open descriptors are inherited. This is not a refinement: a shell
         * builds a pipeline by creating the pipe, forking, and having the
         * child move an inherited end onto its standard output. Without
         * inheritance the child has no such descriptor, the redirection fails,
         * and its output goes to the console while the reader waits forever.
         *
         * Task slots are recycled, so the child's table is whatever the
         * previous occupant left; it must be overwritten, not added to. */
        hw_fds_inherit(child, parent);

        child->id.exit_signal = 0;
        child->id.sig_pending = 0;   /* pending signals are not inherited */
        child->id.sig_blocked = parent->id.sig_blocked;
        for (sg = 0; sg < VIBEOS_HW_NSIG; sg++) {
            child->ps->sig_handler[sg] = parent->ps->sig_handler[sg];
            child->ps->sig_restorer[sg] = parent->ps->sig_restorer[sg];
            child->ps->sig_flags[sg] = parent->ps->sig_flags[sg];
            child->ps->sig_mask[sg] = parent->ps->sig_mask[sg];
        }
    }
    child->id.is_user = 1;
    child->id.exit_code = 0;
    /* The slot must still be the one this fork was given.
     *
     * Everything above writes into g_tasks[idx] without the scheduler lock,
     * on the strength of hw_task_alloc having marked it RESERVED. If that ever
     * fails to hold, two owners fill one slot and the loser's half-written
     * task is what gets scheduled - which is exactly the shape of the wedge
     * this is hunting. Saying so out loud beats inferring it from wreckage. */
    if (child->alloc_seq != my_tenancy || child->state != HW_TASK_RESERVED) {
        /* One line, one critical section: puts and print_hex each take the console lock on their own. */
        vibeos_x86_64_serial_lock();
        vibeos_x86_64_serial_puts("[SCHED] fork lost its slot: idx=0x");
        vibeos_x86_64_serial_print_hex((uint64_t)idx);
        vibeos_x86_64_serial_puts(" mine=0x");
        vibeos_x86_64_serial_print_hex((uint64_t)my_tenancy);
        vibeos_x86_64_serial_puts(" now=0x");
        vibeos_x86_64_serial_print_hex((uint64_t)child->alloc_seq);
        vibeos_x86_64_serial_puts(" state=0x");
        vibeos_x86_64_serial_print_hex((uint64_t)child->state);
        vibeos_x86_64_serial_puts("\n");
        vibeos_x86_64_serial_unlock();
        hw_panic("two owners filled one task slot");
    }
    (void)hw_task_set_state((int)(child - g_tasks), HW_TASK_READY, __func__);
    child->ready_by = "fork";
    return (long)child->id.pid;
}

/* clone() with CLONE_VM|CLONE_THREAD: another thread in this process.
 *
 * The difference from fork is what is *not* copied. The address space is
 * shared rather than duplicated, which means the new task carries the same
 * page tables and the same cr3 - not a copy of them - and exit must therefore
 * not tear that space down while siblings are still running in it.
 *
 * What the new thread does have of its own: a kernel stack, so it can block in
 * a syscall independently; a user stack, which the caller supplies because a C
 * library allocates it; and a TLS base, because thread-local storage is the
 * one thing threads must not share.
 *
 * It resumes at the same instruction the caller returns to, with rax zero -
 * the same trick fork uses - so the C library's clone wrapper sees a return of
 * 0 in the child and the tid in the parent, and branches on that.
 */
static long hw_sys_clone_thread(const vibeos_x86_64_isr_frame_t *frame,
                                uint64_t flags, uint64_t child_stack,
                                uint64_t ptid, uint64_t ctid, uint64_t tls) {
    hw_task_t *parent;
    hw_task_t *child;
    int idx;
    uint32_t my_tenancy;

    if (g_current_task < 0 || !g_tasks[g_current_task].id.is_user) {
        return -VIBEOS_EINVAL;
    }
    /* A thread with no stack of its own would run on its creator's, which is
     * not a degraded thread but two threads writing to one stack. */
    if (child_stack == 0u || !linux_user_ok(child_stack - 8u, 8u, 1)) {
        hw_log(VIBEOS_LOG_WARN, 6u, child_stack, flags,
               "clone refused: unusable thread stack");
        return -VIBEOS_EINVAL;
    }

    /* The same door fork uses.
     *
     * This path had no guard at all, so a thread bomb - pthread_create in a
     * loop, which is all it takes - filled the task table including the
     * reserve that was supposed to keep the machine administrable. The guard
     * was written for fork and clone was left open beside it, which is why
     * there is now one entry point rather than a rule to remember. */
    idx = hw_task_alloc_for_user("clone");
    if (idx < 0) {
        hw_log(VIBEOS_LOG_WARN, 7u, flags, 0,
               "clone refused: no free task slot");
        return -VIBEOS_ENOMEM;
    }
    parent = &g_tasks[g_current_task];
    child = &g_tasks[idx];
    my_tenancy = child->alloc_seq;

    child->kstack_top = hw_alloc_kstack(&child->kstack_base, &child->kstack_pages);
    if (child->kstack_top == 0) {
        (void)hw_task_set_state((int)(child - g_tasks), HW_TASK_FREE, __func__);
        return -VIBEOS_ENOMEM;
    }

    /* The same address space, by sharing the description rather than copying
     * the tables. hw_aspace_create is deliberately not called: two sets of
     * page tables would be two processes wearing one name. */
    vibeos_task_stats()->threads++;
    child->proc = parent->proc;
    /* And the same process: a reference, not a copy. See g_procstate. */
    child->ps = parent->ps;
    (void)__atomic_add_fetch(&child->ps->refs, 1u, __ATOMIC_ACQ_REL);
    child->cr3 = parent->cr3;
    child->cr3_set_by = "clone_thread";

    child->ctx = *frame;
    child->ctx.rax = 0;            /* the child's return from clone() */
    child->ctx.rsp = child_stack;  /* on the stack the library gave it */
    child->ctx.rbp = 0;            /* no caller frame: this is a stack top */
    /* A clean FP environment, not the creator's. A new thread begins at a
     * function entry, so there is no partly-built vector value to inherit -
     * and inheriting one would mean two threads sharing a register file they
     * each believe is theirs. */
    hw_fpu_init_area(child->fpu);

    child->id.pid = (uint32_t)__sync_fetch_and_add(&g_next_pid, 1u);
    child->id.tgid = parent->id.tgid;    /* same process */
    child->id.ppid = parent->id.ppid;    /* threads share their creator's parent */
    child->id.pgid = parent->id.pgid;
    child->id.sid = parent->id.sid;
    child->id.signal_stopped = 0;
    child->id.is_thread = 1;
    child->id.is_user = 1;
    child->id.exit_code = 0;
    child->id.exit_signal = 0;

    /* Thread-local storage. Without this every thread reads the creator's
     * errno and its own stack guard, which is the kind of sharing that looks
     * like memory corruption from user space. */
    child->fs_base = (flags & CLONE_SETTLS) ? tls : parent->fs_base;

    child->id.clear_child_tid = (flags & CLONE_CHILD_CLEARTID) ? ctid : 0;

    /* Descriptors are copied, not shared. Linux shares them under CLONE_FILES
     * and a C library asks for that; here each thread gets its own table with
     * the same entries, so opening a file in one thread is invisible to the
     * others. Pipe ownership stays balanced because the copy takes a
     * reference and exit releases it. Recorded as a difference rather than
     * hidden: it is wrong for a program that passes descriptors between its
     * own threads. */
    {
        hw_fds_inherit(child, parent);
    }

    /* Signal dispositions are the process's. That sentence used to sit above
     * a loop that copied them, so a handler installed in one thread was never
     * seen by another - verified by THREADS_C5_SIGACTION, which died by
     * SIGUSR1. They are shared through child->ps now. The mask and the pending
     * set stay per thread, which is the Linux model. */
    child->id.sig_pending = 0;
    child->id.sig_blocked = parent->id.sig_blocked;

    /* Written through the fault-safe copy: a sibling thread can munmap the page
     * between the range check and the store, faulting in ring 0 (H-021). A
     * failed write is dropped - the thread is created either way, as it is on
     * Linux when these optional stores fault. */
    if ((flags & CLONE_PARENT_SETTID) && ptid != 0u &&
        linux_user_ok(ptid, 4u, 1)) {
        uint32_t v = child->id.pid;
        (void)vibeos_uaccess_copy((void *)(uintptr_t)ptid, &v, sizeof(v));
    }
    if ((flags & CLONE_CHILD_SETTID) && ctid != 0u &&
        linux_user_ok(ctid, 4u, 1)) {
        uint32_t v = child->id.pid;
        (void)vibeos_uaccess_copy((void *)(uintptr_t)ctid, &v, sizeof(v));
    }

    if (child->alloc_seq != my_tenancy || child->state != HW_TASK_RESERVED) {
        hw_panic("two owners filled one task slot");
    }
    (void)hw_task_set_state((int)(child - g_tasks), HW_TASK_READY, __func__);
    child->ready_by = "clone_thread";
    hw_log(VIBEOS_LOG_DEBUG, 8u, (uint64_t)child->id.pid, child->fs_base,
           "thread created (a1 = its TLS base)");
    return (long)child->id.pid;
}

/* waitpid(): reap a finished child. Blocks the caller (state BLOCKED, so the
 * scheduler stops running it) until a child exit wakes it, instead of spinning.
 * The check-and-block is done under cli so a child exit cannot slip in between
 * (lost wakeup); `sti; hlt` then parks the task with interrupts enabled. */
static long hw_sys_waitpid(uint64_t want_pid, uint64_t status_ptr,
                           uint64_t options) {
    uint32_t mypid;

    if (g_current_task < 0 || !g_tasks[g_current_task].id.is_user) {
        return -VIBEOS_EINVAL;
    }
    mypid = g_tasks[g_current_task].id.tgid;

    /* The options used to be dropped by the dispatcher before they got here,
     * so WNOHANG blocked. A shell reaping background jobs stalls on the first
     * one still running, and every bounded poll in a test is not bounded at all.
     *
     * WNOHANG is honoured. WUNTRACED, WCONTINUED and Linux's __WALL, __WCLONE
     * and __WNOTHREAD are accepted and have no effect: this kernel reports
     * neither stopped nor continued children, and it has no thread-group wait
     * distinctions to make. Refusing them would be more precise and would
     * break BusyBox's shell, which passes WUNTRACED for job control. Any other
     * bit is refused, as Linux refuses it. */
    if (options & ~(uint64_t)(0x00000001u | 0x00000002u | 0x00000008u |
                              0x20000000u | 0x40000000u | 0x80000000u)) {
        return -VIBEOS_EINVAL;
    }

    for (;;) {
        int i;
        int have_children = 0;

        __asm__ __volatile__("cli");
        hw_spin_lock_named(&g_sched_lock, __func__);
        for (i = 0; i < VIBEOS_HW_MAX_TASKS; i++) {
            hw_task_t *t = &g_tasks[i];
            if (t->id.ppid != mypid || t->state == HW_TASK_FREE) {
                continue;
            }
            if (want_pid != (uint64_t)-1 && t->id.tgid != (uint32_t)want_pid) {
                continue;
            }
            if (t->state == HW_TASK_ZOMBIE) {
                uint32_t child_pid = t->id.tgid;
                uint64_t code = t->id.exit_code;
                uint32_t exit_signal = t->id.exit_signal;
                /* One encoding, defined once. A wait status is not an exit
                 * code, and an init that read only the code byte reported a
                 * segfault as a clean stop - the crashing service came back
                 * STOPPED. Producer and consumer now share the function. */
                int status = vibeos_wait_status_make((uint32_t)code, exit_signal);
                /* Publishing the slot as FREE is the last thing done to it,
                 * and everything still needed from it is taken first.
                 *
                 * It used to set FREE, drop the lock, and only then call
                 * hw_free_kstack(t) - which both frees pages and writes to the
                 * task. In between, another core allocating a task slot sees
                 * this one free and starts a fork into it, and the two owners
                 * interleave: the reaper then frees the kernel stack the fork
                 * has just allocated, those pages go back on the freelist, and
                 * the allocator hands them out again as page tables. That is
                 * how a live process ends up with a PML4 whose entry zero is a
                 * freelist pointer instead of the kernel - and a core loading
                 * that CR3 stops mid-instruction with no way to report why.
                 *
                 * The freeing happens outside the lock, from locals, because
                 * hw_free_page takes the memory lock and nesting the two would
                 * be a new ordering rule to get wrong. */
                /* Nothing to take: the stack was parked on the core the
                 * task exited on, and that core frees it once it is running on
                 * a different one.
                 *
                 * This used to read kstack_base and kstack_pages here and free
                 * them below, and it was wrong in a way three careful readings
                 * of the exit path missed. Publishing the slot last was not
                 * enough, because the danger is not the slot - it is that the
                 * dying core is still standing on that stack when the reaper
                 * runs. The window is a handful of instructions and it was hit
                 * about one boot in six. */
                uint64_t kbase = 0;
                uint32_t kpages = 0;
                (void)vibeos_teardown_step((uint32_t)(t - g_tasks),
                                           VIBEOS_TEARDOWN_HARVESTED);
                vibeos_task_stats()->reaped++;
                (void)vibeos_teardown_step((uint32_t)(t - g_tasks),
                                           VIBEOS_TEARDOWN_PUBLISHED);
                (void)hw_task_set_state((int)(t - g_tasks), HW_TASK_FREE, __func__); /* reaped; nothing may touch t now */
                hw_spin_unlock(&g_sched_lock);
                (void)kbase; (void)kpages;
                __asm__ __volatile__("sti");
                if (status_ptr != 0 && linux_user_ok(status_ptr, 4, 1)) {
                    /* The wait status word: a normal exit puts the code in the
                     * high byte and leaves the low seven bits clear; a signal
                     * death puts the signal number in those low bits. That is
                     * what WIFEXITED and WIFSIGNALED read.
                     *
                     * Written through the fault-safe copy: the reap has already
                     * published the slot FREE and released the lock, so a
                     * sibling can munmap this page before the store (H-022). The
                     * child stays consumed if it faults - the pid is returned
                     * regardless, as Linux does after EFAULT here. */
                    int st = status;
                    (void)vibeos_uaccess_copy((void *)(uintptr_t)status_ptr,
                                              &st, sizeof(st));
                }
                return (long)child_pid;
            }
            have_children = 1;
        }
        if (!have_children) {
            hw_spin_unlock(&g_sched_lock);
            __asm__ __volatile__("sti");
            return -VIBEOS_ECHILD;
        }
        if (options & 0x00000001u) {   /* WNOHANG: children, none changed */
            hw_spin_unlock(&g_sched_lock);
            __asm__ __volatile__("sti");
            return 0;
        }
        /* Block until a child exit sets us READY again (see hw_task_exit) -
         * or until a signal needs acting on. Blocked first, then asked, still
         * under the lock, for the same reason as the futex and console waits. */
        (void)hw_task_set_state(g_current_task, HW_TASK_BLOCKED, __func__);
        if (hw_signal_interrupts(g_current_task)) {
            (void)hw_task_set_state(g_current_task, HW_TASK_READY, __func__);
            HW_TASK_MARK(g_current_task, ready_by, "waitpid_interrupted");
            hw_spin_unlock(&g_sched_lock);
            __asm__ __volatile__("sti");
            return -VIBEOS_EINTR;
        }
        hw_spin_unlock(&g_sched_lock);
        hw_sched_point("block");
        __asm__ __volatile__("sti; hlt" ::: "memory");
    }
}

/* execve(): replace the current process image with an ELF read from the
 * filesystem. On success the trapframe is rewritten to the new program's entry
 * and CR3 switched, so the syscall return path resumes into the new image. The
 * old address space is not reclaimed yet (no PMM free), which is a known leak. */
/* Copy an argument vector out of user memory.
 *
 * It has to be copied, not pointed at: the strings live in the address space
 * that execve is about to destroy. Both the vector and every string it names
 * are attacker-controlled, so each pointer is validated before it is followed
 * and the whole thing is bounded - a program that asks for more arguments than
 * fit gets E2BIG rather than a kernel that walks off the end of an array. */
#define VIBEOS_HW_MAX_ARGV 16

#define VIBEOS_HW_ARG_BYTES 512

typedef struct {
    const char *slot[VIBEOS_HW_MAX_ARGV + 1];
    char store[VIBEOS_HW_ARG_BYTES];
} hw_argv_t;

/* Why the last argv copy failed, for the exec refusal to quote.
 *
 * "bad-args" alone says a vector could not be read and not which of the range
 * check's several reasons applied - and that check has a why for exactly this
 * situation, because "not accessible" without a reason is a dead end. The
 * distinction that matters here: a page that is not mapped and a page that is
 * mapped but refused are different bugs, and the argv vector lives in a page
 * the child has just written through a copy-on-write fault. */
/* Set on failure and never cleared, which is deliberate and was learned the
 * short way.
 *
 * The first version cleared it on entry. execve calls this twice - argv then
 * envp - so when argv failed and envp then succeeded, envp's entry wiped the
 * reason argv had just recorded and the refusal printed `at=argv:-`. The
 * diagnostic erased its own evidence, and it took two boots out of twenty-four
 * to find out, because the failure it explains is intermittent.
 *
 * Only read immediately after a call returned negative, so the last failure to
 * set it is the one being reported. */
static const char *g_argv_fail_why = "-";

/* The address the range check refused, beside the reason. Set wherever the
 * reason is set, because a reason without the address it applies to cannot
 * tell a garbage pointer from a page that is merely not resident. */
static uint64_t g_argv_fail_addr;

/* How many of the first 64 words of that page read as poison. */
static uint32_t g_argv_poison_words;

static long hw_copy_user_argv(uint64_t uvec, hw_argv_t *out) {
    uint32_t count = 0;
    uint32_t used = 0;

    out->slot[0] = 0;
    if (uvec == 0u) {
        return 0;
    }
    for (;;) {
        uint64_t ptr;
        int len;

        if (count == VIBEOS_HW_MAX_ARGV) {
            g_argv_fail_why = "too_many_entries";
            return -VIBEOS_E2BIG;
        }
        {
            uint32_t why = HW_RANGE_OK;
            if (!hw_user_range_why(uvec + (uint64_t)count * 8u, 8, 0, &why)) {
                g_argv_fail_why = hw_range_why_name(why);
                g_argv_fail_addr = uvec + (uint64_t)count * 8u;
                if (uvec == VIBEOS_FRAME_POISON) {
                    g_argv_fail_why = "use_after_free:argv_vector_is_poison";
                }
                return -VIBEOS_EFAULT;
            }
        }
        /* Fault-safe: a sibling execing or unmapping can pull this page out
         * between the check above and the read. */
        if (vibeos_uaccess_copy(&ptr, (const void *)(uintptr_t)
                (uvec + (uint64_t)count * 8u), 8u) != 0) {
            g_argv_fail_why = "fault_reading_argv_vector";
            g_argv_fail_addr = uvec + (uint64_t)count * 8u;
            return -VIBEOS_EFAULT;
        }
        if (ptr == 0u) {
            break;
        }
        if (used >= VIBEOS_HW_ARG_BYTES) {
            g_argv_fail_why = "arg_bytes_exhausted";
            return -VIBEOS_E2BIG;
        }
        if (hw_copy_user_string(ptr, &out->store[used],
                                (int)(VIBEOS_HW_ARG_BYTES - used)) != 0) {
            uint32_t why = HW_RANGE_OK;
            (void)hw_user_range_why(ptr, 1, 0, &why);
            g_argv_fail_why = hw_range_why_name(why);
            g_argv_fail_addr = ptr;
            /* A pointer that *is* the free-page poison is not a wild pointer.
             * It means the memory holding this argv vector was released while
             * something still referred to it, and the range check is the only
             * reason the machine did not follow it. Named here so the next
             * person does not have to recognise 0xdead0000dead0000 by eye -
             * which is exactly what it took to find this one. */
            if (ptr == VIBEOS_FRAME_POISON) {
                /* How much of the page is poison, which separates two very
                 * different faults that produce the same word.
                 *
                 * If the whole page reads as poison, this is a freshly
                 * allocated frame whose copy-on-write copy never happened -
                 * the page is pristine, nothing wrote to it after it was
                 * freed, and no free-side detector would ever fire.
                 *
                 * If only some words are poison, something wrote the pattern
                 * into a page that is still live, which is the opposite
                 * problem and is the one the free-side detectors are for.
                 *
                 * Counted over the array itself rather than the whole page:
                 * this runs inside a refusal on a path a program can reach, so
                 * it must not become a loop over four thousand words. */
                uint32_t poisoned = 0;
                uint32_t probe;

                for (probe = 0; probe < 64u; probe++) {
                    uint64_t word;
                    if (vibeos_uaccess_copy(&word, (const void *)(uintptr_t)
                            ((uvec & ~0xFFFull) + (uint64_t)probe * 8u), 8u) == 0 &&
                        word == VIBEOS_FRAME_POISON) {
                        poisoned++;
                    }
                }
                g_argv_poison_words = poisoned;
                g_argv_fail_why = (poisoned >= 64u)
                                ? "use_after_free:whole_page_is_poison"
                                : "use_after_free:argv_is_poison";
            }
            return -VIBEOS_EFAULT;
        }
        out->slot[count] = &out->store[used];
        for (len = 0; out->store[used + (uint32_t)len]; len++) {
            /* measure */
        }
        used += (uint32_t)len + 1u;
        count++;
    }
    out->slot[count] = 0;
    return (long)count;
}

static long hw_sys_execve(vibeos_x86_64_isr_frame_t *frame, uint64_t path_uptr,
                          uint64_t argv_uptr, uint64_t envp_uptr) {
    char path[128];
    hw_proc_t np;
    hw_procstate_t *nps, *ops;
    hw_task_t *t;
    long n;
    uint32_t k;
    static hw_argv_t g_exec_argv;   /* under g_exec_lock, like the image buffer */
    static hw_argv_t g_exec_envp;
    const char *fallback_argv[2];
    const char *const *argv;

    if (g_current_task < 0 || !g_tasks[g_current_task].id.is_user) {
        return -VIBEOS_EINVAL;
    }
    /* Named, not just returned.
     *
     * These copies from user memory were the only exec failures that said
     * nothing at all - no line, no tally - so a child that died on one of them
     * left an exit code of 127 and a log with no reason in it anywhere. That
     * is what happened to svc-stress on two boots out of six, with every
     * memory counter reporting clean, and working out that 127 even meant
     * "execve failed" took reading init's source.
     *
     * The reason lands in the same tally as every other refusal, and the boot
     * gate asserts the *set* of refusals seen is only "not-found" - so one of
     * these turns a boot red instead of leaving a service quietly missing. */
    if (hw_copy_user_string(path_uptr, path, sizeof(path)) != 0) {
        return hw_exec_refuse(VIBEOS_EXEC_BAD_ARGS, "-", "path");
    }
    fallback_argv[0] = path;
    fallback_argv[1] = 0;
    /* g_exec_elf is a single shared staging buffer, so the read and the load out
     * of it have to be one critical section: two cores exec'ing at once would
     * otherwise each load the other's image. */
    hw_spin_lock_preemptible(&g_exec_lock);
    {
        long na = hw_copy_user_argv(argv_uptr, &g_exec_argv);
        long ne = hw_copy_user_argv(envp_uptr, &g_exec_envp);
        if (na < 0 || ne < 0) {
            /* Which vector, because argv and envp fail for different reasons:
             * a program with too many arguments and a program handed a bad
             * environment pointer are not the same bug. */
            {
                /* One string, so the line stays one fact: which vector and
                 * why. Two fields printed separately from two cores come back
                 * interleaved and read as a contradiction. */
                char detail[96];
                const char *which = (na < 0) ? "argv:" : "envp:";
                const char *why = g_argv_fail_why;
                uint32_t w = 0, k;
                for (k = 0; which[k] && w < sizeof(detail) - 1u; k++) {
                    detail[w++] = which[k];
                }
                for (k = 0; why && why[k] && w < sizeof(detail) - 1u; k++) {
                    detail[w++] = why[k];
                }
                /* Who was asking, and whether they had an address space.
                 *
                 * "pml4_absent_or_not_user" says a range check found no user
                 * page tables; it does not say whether the caller was a kernel
                 * task that never had any, or a user task whose tables went
                 * missing under it. Those are completely different defects and
                 * the reason alone cannot tell them apart, which is why this
                 * refusal has been open all session on a stable signature.
                 *
                 * Appended to the same string rather than printed separately:
                 * two fields from two cores come back interleaved and read as
                 * a contradiction. */
                {
                    static const char hexd[] = "0123456789abcdef";
                    int cur = g_current_task;
                    uint64_t cr3 = (cur >= 0) ? g_tasks[cur].cr3 : 0ull;
                    int is_user = (cur >= 0) ? g_tasks[cur].id.is_user : -1;
                    int is_thread = (cur >= 0) ? g_tasks[cur].id.is_thread : -1;
                    const char *tag = " task=";
                    int j;

                    for (k = 0; tag[k] && w < sizeof(detail) - 1u; k++) {
                        detail[w++] = tag[k];
                    }
                    if (cur < 0 && w < sizeof(detail) - 1u) {
                        detail[w++] = '-';
                    } else {
                        for (j = 1; j >= 0; j--) {
                            if (w < sizeof(detail) - 1u) {
                                detail[w++] =
                                    hexd[((uint32_t)cur >> (j * 4)) & 0xFu];
                            }
                        }
                    }
                    tag = (is_user > 0) ? " user" : (is_user == 0 ? " kernel"
                                                                  : " none");
                    for (k = 0; tag[k] && w < sizeof(detail) - 1u; k++) {
                        detail[w++] = tag[k];
                    }
                    if (is_thread > 0) {
                        tag = " thread";
                        for (k = 0; tag[k] && w < sizeof(detail) - 1u; k++) {
                            detail[w++] = tag[k];
                        }
                    }
                    /* And the address that was refused. A top-level entry
                     * that is absent means the whole 512 GiB region holding
                     * that address is unmapped, which is what a garbage
                     * pointer looks like and is not what a paged-out stack
                     * looks like - so the address separates the two. */
                    tag = " at=";
                    for (k = 0; tag[k] && w < sizeof(detail) - 1u; k++) {
                        detail[w++] = tag[k];
                    }
                    for (j = 15; j >= 0; j--) {
                        if (w < sizeof(detail) - 1u) {
                            detail[w++] =
                                hexd[(g_argv_fail_addr >> (j * 4)) & 0xFu];
                        }
                    }
                    tag = " poisonw=";
                    for (k = 0; tag[k] && w < sizeof(detail) - 1u; k++) {
                        detail[w++] = tag[k];
                    }
                    for (j = 1; j >= 0; j--) {
                        if (w < sizeof(detail) - 1u) {
                            detail[w++] =
                                hexd[(g_argv_poison_words >> (j * 4)) & 0xFu];
                        }
                    }
                    tag = " cr3=";
                    for (k = 0; tag[k] && w < sizeof(detail) - 1u; k++) {
                        detail[w++] = tag[k];
                    }
                    for (j = 15; j >= 0; j--) {
                        if (w < sizeof(detail) - 1u) {
                            detail[w++] = hexd[(cr3 >> (j * 4)) & 0xFu];
                        }
                    }
                }
                detail[w] = 0;
                (void)hw_exec_refuse(VIBEOS_EXEC_BAD_ARGS, path, detail);
            }
            hw_spin_unlock_preemptible(&g_exec_lock);
            return (na < 0) ? na : ne;
        }
        /* A caller that passes no argv still gets an argv[0]: the path it was
         * started from, which is what a program prints as its own name - and
         * what BusyBox uses to decide which applet it is. */
        argv = (na > 0) ? g_exec_argv.slot : (const char *const *)fallback_argv;
    }
    if (hw_exec_cache_hit(path)) {
        n = g_exec_cached_len;   /* already staged, byte for byte */
    } else {
        hw_exec_cache_drop();
        n = hw_read_file_cached(path, g_exec_elf, g_exec_elf_cap,
                                &g_exec_cached_id);
        if (n > 0) {
            uint32_t i;
            for (i = 0; i < sizeof(g_exec_cached) - 1u && path[i]; i++) {
                g_exec_cached[i] = path[i];
            }
            g_exec_cached[i] = 0;
            g_exec_cached_len = n;
            /* Clear whatever the previous program left beyond this one.
             *
             * The staging buffer is shared by every exec, so a read that
             * stops short leaves the tail of the last image in place - and
             * the result is not obviously broken, it is a plausible ELF made
             * of two programs. That parses far enough to fail somewhere
             * confusing. Zeroing costs one pass over memory on a path that
             * already read the file, and turns a subtle corruption into a
             * clean parse failure. */
            {
                uint32_t z;
                for (z = (uint32_t)n; z < g_exec_elf_cap; z++) {
                    g_exec_elf[z] = 0;
                }
            }
        }
    }
    if (n <= 0) {
        /* Which of the two it was matters: a file that cannot be read and a
         * file that reads but does not parse are different bugs, and the shell
         * prints the same "cannot exec" for both. */
        /* DEBUG, not ERROR. A shell resolving a bare command name tries it as
         * a path first, and a C runtime asks for /proc/self/exe, which this
         * filesystem does not have. Both miss on every healthy boot, and an
         * error that fires every time teaches you to ignore errors. It is
         * still recorded, so it is in the ring dump if a failure turns out to
         * be about one of these. */
        hw_log(VIBEOS_LOG_DEBUG, 3u, (uint64_t)n, 0,
               "execve could not read the program image");
        /* Counted as well as printed. This one had a message of its own, which
         * is exactly how it stayed outside the tally: a sentence in the log is
         * not a reason anything can assert on. It is also the *expected*
         * refusal on a healthy boot - a shell resolving a bare command name
         * tries it as a path first, and a C runtime asks for /proc/self/exe -
         * so the gate's expected set is not empty, and a set that must contain
         * this and nothing else is a much stronger assertion than a count. */
        (void)hw_exec_refuse(VIBEOS_EXEC_NOT_FOUND, path, "read_file");
        hw_spin_unlock_preemptible(&g_exec_lock);
        return -VIBEOS_ENOENT;
    }
    /* The new image gets a new process. The program break, the mapping cursor,
     * the regions and the dispositions all describe the old image, and a
     * sibling thread still running that image keeps the old process. Built
     * before the old one is let go, which is why the pool has room for an exec
     * per core. */
    if (g_tasks[g_current_task].ps == 0) {
        hw_spin_unlock_preemptible(&g_exec_lock);
        return -VIBEOS_EINVAL;
    }
    nps = hw_procstate_new();
    if (!nps) {
        hw_spin_unlock_preemptible(&g_exec_lock);
        return -VIBEOS_ENOMEM;
    }
    if (hw_proc_create(&np, nps, g_exec_elf, (uint64_t)n,
                       (uint64_t)g_exec_elf_cap, argv,
                       g_exec_envp.slot[0] ? g_exec_envp.slot : 0, path,
                       g_exec_cached_id) != 0) {
        hw_procstate_put(nps);
        hw_log(VIBEOS_LOG_ERROR, 4u, (uint64_t)n, 0,
               "execve rejected the program image");
        vibeos_x86_64_serial_lock();
        vibeos_x86_64_serial_puts("[EXEC] image rejected: ");
        vibeos_x86_64_serial_puts(path);
        vibeos_x86_64_serial_puts(" bytes=0x");
        vibeos_x86_64_serial_print_hex((uint64_t)n);
        vibeos_x86_64_serial_puts("\n");
        vibeos_x86_64_serial_unlock();
        hw_spin_unlock_preemptible(&g_exec_lock);
        return -VIBEOS_ENOMEM;
    }
    hw_spin_unlock_preemptible(&g_exec_lock);

    t = &g_tasks[g_current_task];

    /* An exec ends every other thread of the process, and the thread that
     * called it becomes the process. H-006, verified: none of that happened.
     * The siblings kept running the old image, and a thread that was not the
     * leader kept its own id and is_thread = 1 - so the program it loaded ran
     * as a thread, and a thread's exit is released at once instead of left as
     * a zombie. Its exit code could never reach the parent.
     *
     * Here and not earlier: only now has the new image loaded. An exec that
     * fails must leave the process with every thread it had.
     *
     * Siblings are found by thread-group id, not by process reference. A leader
     * that already exited - pthread_exit from main - gave its reference back
     * on the way out, so it no longer points at this process; its tgid is all
     * that still says whose it was. */
    {
        int me = (int)(t - g_tasks);
        uint32_t tgid = t->id.tgid;
        int i, any;

        /* The leader must die quietly. hw_task_exit reads is_thread under
         * g_sched_lock to choose between "released" and "zombie, wake the
         * parent", so setting it under the same lock is decided before that
         * choice or not at all. A leader that is already a zombie is handled
         * in the wait below. */
        hw_spin_lock_named(&g_sched_lock, __func__);
        for (i = 0; i < VIBEOS_HW_MAX_TASKS; i++) {
            if (i != me && g_tasks[i].id.is_user && g_tasks[i].id.tgid == tgid &&
                g_tasks[i].id.pid == tgid &&
                g_tasks[i].state != HW_TASK_FREE &&
                g_tasks[i].state != HW_TASK_ZOMBIE) {
                g_tasks[i].id.is_thread = 1;
            }
        }

        /* Still under g_sched_lock: slots found by tgid can be reused the moment
         * the lock is dropped (H-007). This used to drop it first, under a note
         * that hw_signal_raise reaches hw_task_set_state - which takes no lock.
         * And not through exit_group's flag: nothing that dies here should
         * report a group exit code - they are all threads now, and threads
         * report nothing. */
        for (i = 0; i < VIBEOS_HW_MAX_TASKS; i++) {
            if (i != me && g_tasks[i].id.is_user && g_tasks[i].id.tgid == tgid &&
                g_tasks[i].state != HW_TASK_FREE &&
                g_tasks[i].state != HW_TASK_RESERVED &&
                g_tasks[i].state != HW_TASK_ZOMBIE) {
                (void)hw_signal_raise(i, VIBEOS_SIGKILL);
            }
        }
        hw_spin_unlock(&g_sched_lock);

        /* Wait until they are gone, sleeping. Unbounded on purpose: a sibling in
         * a syscall finishes it and dies on the way out, and one blocked in a
         * wait is interrupted since waits notice signals. A bound would only
         * turn a slow exec into a half-done one; if this ever hangs, the wait
         * that ignored a signal is the defect. */
        for (;;) {
            any = 0;
            hw_spin_lock_named(&g_sched_lock, __func__);
            for (i = 0; i < VIBEOS_HW_MAX_TASKS; i++) {
                if (i == me || !g_tasks[i].id.is_user || g_tasks[i].id.tgid != tgid ||
                    g_tasks[i].state == HW_TASK_FREE) {
                    continue;
                }
                if (g_tasks[i].state == HW_TASK_ZOMBIE && g_tasks[i].id.pid == tgid) {
                    /* A leader that exited before this exec. Its id is the one
                     * this task is about to take, so the slot cannot stay: a
                     * parent reaping it afterwards would see the process end
                     * while the new image runs. Released under the lock waitpid
                     * reaps under, with the same final step the thread branch of
                     * hw_task_exit uses. */
                    hw_task_release(i);
                    continue;
                }
                any = 1;
            }
            hw_spin_unlock(&g_sched_lock);
            if (!any) {
                break;
            }
            if (hw_signal_interrupts(me)) {
                /* Something must be acted on here first - a SIGKILL from a
                 * sibling's exit_group, say. The new image is not committed:
                 * give back what was built for it and let the signal be
                 * delivered on the way out. */
                hw_aspace_destroy_why(&np.as, "exec_interrupted");
                hw_procstate_put(nps);
                return -VIBEOS_EINTR;
            }
            hw_sched_point("block");
            __asm__ __volatile__("sti; hlt" ::: "memory");
        }

        /* Alone now, and the old leader's slot is gone, so no two tasks ever
         * hold pid == tgid at once. ppid is already the leader's: threads
         * inherit their creator's parent. */
        if (t->id.pid != tgid) {
            t->id.pid = tgid;
            HW_TASK_MARK(me, ready_by, "exec_took_leader_id");
        }
        t->id.is_thread = 0;
    }
    /* Stored with a leading slash even when the caller used a relative path.
     * /proc/self/exe is defined to be absolute, and a C runtime does not merely
     * prefer that: glibc asserts on it during startup and aborts the process,
     * which is how the relative form was found. */
    {
        uint32_t w = 0;
        if (path[0] != '/') {
            np.exe_path[w++] = '/';
        }
        for (k = 0; w < (uint32_t)sizeof(np.exe_path) - 1u && path[k]; k++) {
            np.exe_path[w++] = path[k];
        }
        np.exe_path[w] = 0;
    }
    {
        vibeos_hw_aspace_t old_as = t->proc.as; /* reclaim after switching CR3 */

        /* Caught signals revert to their default in the new image, because
         * the handler addresses point into the one that is going away. Ignored
         * stays ignored and the per-signal masks carry over, which is what
         * execve is defined to do.
         *
         * Derived here, from the outgoing process, and not after the commit:
         * the reference to it is given back below, and once the last one is
         * gone its slot can be handed to an exec or a fork on another core -
         * so reading it afterwards could copy a stranger's dispositions. */
        ops = t->ps;
        {
            uint32_t sg;
            for (sg = 0; sg < VIBEOS_HW_NSIG; sg++) {
                nps->sig_handler[sg] = (ops->sig_handler[sg] == SIG_IGN_ADDR)
                                       ? SIG_IGN_ADDR : SIG_DFL_ADDR;
                nps->sig_restorer[sg] = 0;
                nps->sig_flags[sg] = 0;
                nps->sig_mask[sg] = ops->sig_mask[sg];
            }
        }

        vibeos_task_stats()->execs++;
        int shared;

        /* nps already carries the regions the loader built for the new image,
         * and the outgoing regions stay with the outgoing process. Clearing the
         * list here - which the first version did - throws away the description
         * of the program that is about to run, and mprotect then refuses the
         * RELRO the C library performs on its own image during startup. */
        t->proc = np;
        t->ps = nps;
        /* The address a thread asked to have zeroed on exit belongs to the image
         * that asked. Left set, the new image's exit would write zero into
         * whatever now lives at that address - found while writing H-006, and
         * true of every exec, not only a threaded one. A C library sets it
         * again at startup if it wants it. */
        t->id.clear_child_tid = 0;
        t->cr3 = hw_proc_cr3(&t->proc);
        t->cr3_set_by = "execve";
        hw_write_cr3(t->cr3);

        /* Only if nobody else is running in there.
         *
         * This used to free the old tables unconditionally, and a threaded
         * process that execs shares them with its siblings: their PML4 went
         * back to the allocator, was handed to some other allocation, and was
         * written over while they were still executing. The symptom is a
         * ring-3 instruction fetch that faults with cr2 equal to rip on a page
         * the process was already running from, and a walk that stops at level
         * one with a PML4 entry holding a kernel pointer instead of a table.
         *
         * hw_task_exit has asked this question since the wedge it was written
         * for; exec never did, which is the whole defect. Not destroying is
         * safe: the last sibling to exit takes the same path and frees them
         * then. */
        hw_spin_lock_named(&g_sched_lock, __func__);
        shared = hw_aspace_shared_by_other(old_as.pml4, (int)(t - g_tasks));
        hw_spin_unlock(&g_sched_lock);

        if (shared) {
            HW_TASK_MARK((int)(t - g_tasks), aspace_killed_by,
                         "execve_kept_shared");
        } else {
            hw_aspace_destroy(&old_as);         /* old CR3 no longer active */
        }
        /* The outgoing regions go with the last reference to the outgoing
         * process: now, for a single-threaded exec, and when the last sibling
         * still running the old image exits, for a threaded one. That is the
         * same moment the address space above is freed or kept, because the
         * siblings hold both. */
        hw_procstate_put(ops);
    }

    for (k = 0; k < (uint32_t)sizeof(*frame); k++) {
        ((uint8_t *)(void *)frame)[k] = 0;
    }
    /* The old image is gone and its thread-local storage with it. Clear the
     * base here and in the register, because exec returns straight to user
     * space without passing through the scheduler's restore. */
    t->fs_base = 0;
    hw_wrmsr(MSR_FS_BASE, 0);
    /* The dispositions were derived into the new process before the commit,
     * while the outgoing one still existed. Pending signals do not survive an
     * exec: they were raised against the old image. */
    t->id.sig_pending = 0;

    frame->rip = np.entry;
    frame->cs = VIBEOS_HW_USER_CODE_SEL;
    frame->rflags = 0x202;
    frame->rsp = np.user_sp;
    frame->ss = VIBEOS_HW_USER_DATA_SEL;
    /* One line per exec: what was loaded, how big it was, and where it
      * starts. Enough to tell a failed load from a failed program without
      * being enough to drown the log. */
    /* One line, one critical section. puts and print_hex each take the
     * lock on their own, so an unbracketed multi-part message is several
     * critical sections and another core lands in the middle of it. The
     * boot gate's log-integrity check found this by seeing a hex field
     * cut off right after its "0x". */
    hw_log(VIBEOS_LOG_DEBUG, 41u, (uint64_t)n, np.entry,
           "execve loaded a program (a0 = bytes, a1 = entry)");
    vibeos_x86_64_serial_lock();
    vibeos_x86_64_serial_puts("[EXEC] ");
    vibeos_x86_64_serial_puts(path);
    vibeos_x86_64_serial_puts(" bytes=0x");
    vibeos_x86_64_serial_print_hex((uint64_t)n);
    vibeos_x86_64_serial_puts(" entry=0x");
    vibeos_x86_64_serial_print_hex(np.entry);
    vibeos_x86_64_serial_puts("\n");
    vibeos_x86_64_serial_unlock();
    return 0; /* frame replaced; syscall return enters the new image */
}

/* prctl(): the process name is the operation programs actually use, and it is
 * stored rather than acknowledged - it costs sixteen bytes and makes the
 * scheduler's log say which program a pid is. */
static long hw_sys_prctl(uint64_t op, uint64_t arg) {
    hw_task_t *t;

    if (g_current_task < 0) {
        return -VIBEOS_EINVAL;
    }
    t = &g_tasks[g_current_task];
    if (op == PR_SET_NAME) {
        char kname[16];
        /* Copy in fault-safe, then terminate: a sibling munmap between the
         * check and the read would fault in ring 0 (uaccess follow-up). */
        if (vibeos_uaccess_copy(kname, (const void *)(uintptr_t)arg, 16) != 0) {
            return -VIBEOS_EFAULT;
        }
        vibeos_task_set_comm(&t->id, kname, sizeof(kname));
        return 0;
    }
    if (op == PR_GET_NAME) {
        if (vibeos_uaccess_copy((void *)(uintptr_t)arg, t->id.comm, 16) != 0) {
            return -VIBEOS_EFAULT;
        }
        return 0;
    }
    return -VIBEOS_EINVAL;
}

static long hw_sys_setpgid(uint64_t requested_pid, uint64_t requested_pgid) {
    uint32_t pid;
    int target;
    int leader_slot;
    int leader;
    long r;
    if (g_current_task < 0 || !g_tasks[g_current_task].id.is_user) {
        return -VIBEOS_EINVAL;
    }
    pid = requested_pid == 0 ? g_tasks[g_current_task].id.tgid : (uint32_t)requested_pid;
    /* Both arms cast to int explicitly. `pid` is uint32_t, so the conditional
     * otherwise takes the unsigned type and converts back on assignment - the
     * guard below still works, but the reader has to prove that, and the
     * compiler warns rather than take it on faith. */
    leader = requested_pgid == 0 ? (int)pid : (int)requested_pgid;
    /* Two lookups and a write to one of the slots found: under g_sched_lock,
     * or the write can land on a task that took the slot in between (H-007). */
    hw_spin_lock_named(&g_sched_lock, __func__);
    target = hw_task_by_pid(pid);
    if (target < 0) {
        r = -VIBEOS_ESRCH;
    } else if (g_tasks[target].id.sid != g_tasks[g_current_task].id.sid) {
        r = -VIBEOS_EPERM;
    } else if (leader <= 0 || (leader_slot = hw_task_by_pid((uint32_t)leader)) < 0) {
        r = -VIBEOS_ESRCH;
    } else if (g_tasks[leader_slot].id.sid != g_tasks[target].id.sid) {
        r = -VIBEOS_EPERM;
    } else {
        g_tasks[target].id.pgid = (uint32_t)leader;
        r = 0;
    }
    hw_spin_unlock(&g_sched_lock);
    return r;
}

static long hw_sys_setsid(void) {
    int i;
    hw_task_t *current;
    if (g_current_task < 0 || !g_tasks[g_current_task].id.is_user) {
        return -VIBEOS_EINVAL;
    }
    current = &g_tasks[g_current_task];
    /* The check and the whole transition under g_sched_lock, so the "not a
     * group leader" test cannot race the assignment that follows it and no
     * sibling - setpgid, getsid, kill's group resolution, all of which read
     * these fields under the lock - observes pgid/sid mid-change (H-027, the
     * same reasoning as H-007). */
    hw_spin_lock_named(&g_sched_lock, __func__);
    if (current->id.pgid == current->id.tgid) {
        hw_spin_unlock(&g_sched_lock);
        return -VIBEOS_EPERM;
    }
    current->id.sid = current->id.tgid;
    current->id.pgid = current->id.tgid;
    for (i = 0; i < VIBEOS_HW_MAX_TASKS; i++) {
        if (i != g_current_task && g_tasks[i].id.is_user &&
            g_tasks[i].state != HW_TASK_FREE &&
            g_tasks[i].state != HW_TASK_RESERVED &&
            g_tasks[i].id.tgid == current->id.tgid) {
            g_tasks[i].id.sid = current->id.sid;
            g_tasks[i].id.pgid = current->id.pgid;
        }
    }
    hw_spin_unlock(&g_sched_lock);
    return (long)current->id.tgid;
}

static long hw_sys_getsid(uint64_t requested_pid) {
    int target;
    uint32_t pid;
    long r;
    if (g_current_task < 0 || !g_tasks[g_current_task].id.is_user) {
        return -VIBEOS_EINVAL;
    }
    pid = requested_pid == 0 ? g_tasks[g_current_task].id.tgid : (uint32_t)requested_pid;
    /* The sid is read from the slot the lookup found, so the two are one
     * critical section: otherwise the answer can be another process's (H-007). */
    hw_spin_lock_named(&g_sched_lock, __func__);
    target = hw_task_by_pid(pid);
    r = target < 0 ? -VIBEOS_ESRCH : (long)g_tasks[target].id.sid;
    hw_spin_unlock(&g_sched_lock);
    return r;
}

/* setuid()/setgid(): there is one identity and it is root. Becoming it again
 * succeeds; becoming anyone else is refused rather than pretended. */
static long hw_sys_setresid(uint64_t id) {
    return (id == 0u) ? 0 : -VIBEOS_EPERM;
}

/* arch_prctl(): the one everything else depends on.
 *
 * On x86-64 a C runtime addresses its thread state through %fs - errno, the
 * stack-protector cookie, the locale pointer. The base of that segment lives
 * in a model-specific register, so setting it is privileged and a program
 * cannot do it itself. Until this works, a libc faults on its first line. */
static long hw_sys_arch_prctl(uint64_t code, uint64_t addr) {
    hw_task_t *t;

    if (g_current_task < 0 || !g_tasks[g_current_task].id.is_user) {
        return -VIBEOS_EINVAL;
    }
    t = &g_tasks[g_current_task];

    switch (code) {
        case ARCH_SET_FS:
            /* A non-canonical address in this MSR faults on the wrmsr itself,
             * in ring 0 - user space must not be able to reach that. So the
             * base is checked before the write.
             *
             * It has to accept both user windows, and getting that wrong is
             * not a subtle failure: a static musl binary keeps its thread
             * pointer in its own .bss, down in the low window, and reacts to a
             * refusal by executing hlt on purpose. The kernel then reports a
             * general-protection fault in ring 3 with nothing to say that a
             * bounds check three functions away was the cause. */
            if (!hw_user_addr_ok(addr)) {
                return -VIBEOS_EPERM;
            }
            t->fs_base = addr;
            hw_wrmsr(MSR_FS_BASE, addr);
            return 0;
        case ARCH_GET_FS:
            /* Fault-safe: a sibling thread can munmap the page between the
             * range check and here (H-024). */
            if (vibeos_uaccess_copy((void *)(uintptr_t)addr, &t->fs_base,
                                    sizeof(t->fs_base)) != 0) {
                return -VIBEOS_EFAULT;
            }
            return 0;
        case ARCH_SET_GS:
        case ARCH_GET_GS:
            /* %gs holds this CPU's per-CPU block. Handing it to a program
             * would let ring 3 relocate the kernel's own state. */
            return -VIBEOS_EPERM;
        default:
            return -VIBEOS_EINVAL;
    }
}

/* prlimit64(): report the limits that are real here. The stack is the one a
 * runtime acts on - some size a guard region from it. */
static long hw_sys_prlimit64(uint64_t resource, uint64_t new_uptr, uint64_t old_uptr) {
    if (new_uptr != 0u) {
        return -VIBEOS_EPERM;   /* the limits here are fixed by the layout */
    }
    if (old_uptr == 0u) {
        return 0;
    }
    {
        uint64_t *rl = (uint64_t *)(uintptr_t)old_uptr;
        switch (resource) {
            case 3: /* RLIMIT_STACK */
                rl[0] = (uint64_t)VIBEOS_HW_USER_STACK_PAGES * 4096ull;
                rl[1] = rl[0];
                break;
            case 7: /* RLIMIT_NOFILE */
                rl[0] = 3u + VIBEOS_HW_MAX_FDS;
                rl[1] = rl[0];
                break;
            default:
                rl[0] = 0xFFFFFFFFFFFFFFFFull;   /* RLIM64_INFINITY */
                rl[1] = rl[0];
                break;
        }
    }
    return 0;
}

static long hw_futex_wait(uint64_t addr, uint32_t expected) {
    uint32_t slot;
    uint32_t cur = 0;
    int me = g_current_task;

    if (me < 0 || addr == 0u) {
        return -VIBEOS_EINVAL;   /* the address is the row's: IN_IFM_ERR, EINVAL */
    }

    hw_spin_lock_named(&g_futex_lock, __func__);
    /* The compare and the enqueue are one step. Reading the word first and
     * enqueuing after would leave a window in which a waker sees no waiter and
     * the waiter then sleeps on a value that has already changed - the lost
     * wakeup, which presents as a program that stops for no reason. */
    /* Through the fault-tolerant copy: the check above and this read are two
     * instants, and g_futex_lock can spin between them (H-003). */
    if (vibeos_uaccess_copy(&cur, (const void *)(uintptr_t)addr, 4u) != 0) {
        hw_spin_unlock(&g_futex_lock);
        return -VIBEOS_EFAULT;
    }
    if (cur != expected) {
        hw_spin_unlock(&g_futex_lock);
        hw_log(VIBEOS_LOG_DEBUG, 21u, addr, (uint64_t)expected,
               "futex wait: value already moved");
        return -VIBEOS_EAGAIN;
    }
    for (slot = 0; slot < VIBEOS_HW_MAX_FUTEX_WAITERS; slot++) {
        if (!g_futex_waiters[slot].used) {
            break;
        }
        /* Reclaim an entry whose enqueuer's slot has since been reused: the
         * wake's tenancy check will never match it again, so it is only holding
         * a table slot. This is what bounds the table against a thread reaped
         * while blocked, which never runs the cleanup below. */
        if (g_tasks[g_futex_waiters[slot].task].alloc_seq != g_futex_waiters[slot].seq) {
            break;
        }
    }
    if (slot == VIBEOS_HW_MAX_FUTEX_WAITERS) {
        hw_spin_unlock(&g_futex_lock);
        return -VIBEOS_ENOMEM;
    }
    g_futex_waiters[slot].used = 1;
    g_futex_waiters[slot].addr = addr;
    g_futex_waiters[slot].ps = g_tasks[me].ps;
    g_futex_waiters[slot].task = me;
    g_futex_waiters[slot].seq = g_tasks[me].alloc_seq;
    g_futex_waiters[slot].woken = 0;

    hw_spin_lock_named(&g_sched_lock, __func__);
    (void)hw_task_set_state(me, HW_TASK_BLOCKED, __func__);
    hw_spin_unlock(&g_sched_lock);
    hw_spin_unlock(&g_futex_lock);
    hw_log(VIBEOS_LOG_DEBUG, 22u, addr,
           (uint64_t)cur |
           ((uint64_t)g_tasks[me].id.pid << 32),
           "futex wait: sleeping (a1 = value | tid<<32)");

    /* Yield until somebody wakes us. The scheduler runs from the timer, so
     * this is a wait and not a spin: the core is given away on the first
     * interrupt and this task is not runnable again until a wake says so.
     *
     * Or until a signal that must be acted on is pending. The task was made
     * BLOCKED before this loop, so a signal raised after that finds it BLOCKED
     * and makes it runnable, and a signal raised before it is seen by the check
     * on the first pass. Checking before blocking would leave a window in which
     * neither happens. */
    while (!g_futex_waiters[slot].woken) {
        if (hw_signal_interrupts(me)) {
            break;
        }
        hw_sched_point("block");
        __asm__ __volatile__("sti; hlt" ::: "memory");
    }

    {
        int interrupted;

        /* Woken or interrupted is decided under the lock a waker takes. Read
         * outside it, a FUTEX_WAKE landing between the loop and here would count
         * this waiter as woken while it returned EINTR - and the thread that
         * wake was meant for would never be told. A wake that got in first
         * wins: the call returns 0 and the signal is delivered on the way out
         * all the same. */
        hw_spin_lock_named(&g_futex_lock, __func__);
        interrupted = !g_futex_waiters[slot].woken;
        g_futex_waiters[slot].addr = 0;
        g_futex_waiters[slot].used = 0;   /* released by its owner, and only here */
        hw_spin_unlock(&g_futex_lock);
        if (interrupted) {
            /* The signal may have been raised before this task was BLOCKED, in
             * which case nothing made it runnable again. It is running now;
             * say so, the same transition hw_signal_raise uses. */
            hw_spin_lock_named(&g_sched_lock, __func__);
            if (g_tasks[me].state == HW_TASK_BLOCKED) {
                (void)hw_task_set_state(me, HW_TASK_READY, __func__);
                HW_TASK_MARK(me, ready_by, "futex_wait_interrupted");
            }
            hw_spin_unlock(&g_sched_lock);
            hw_log(VIBEOS_LOG_DEBUG, 23u, addr, (uint64_t)g_tasks[me].id.pid,
                   "futex wait: interrupted by a signal");
            return -VIBEOS_EINTR;
        }
    }
    hw_log(VIBEOS_LOG_DEBUG, 23u, addr, (uint64_t)g_tasks[me].id.pid,
           "futex wait: woken");
    return 0;
}

static long hw_sys_futex(uint64_t addr, uint64_t op, uint64_t val) {
    switch (op & FUTEX_CMD_MASK) {
        case FUTEX_WAKE:
            return hw_futex_wake(g_current_task >= 0 ? g_tasks[g_current_task].ps : 0,
                                 addr, (uint32_t)val);
        case FUTEX_WAIT:
            return hw_futex_wait(addr, (uint32_t)val);
        default:
            /* Loudly, because this is how a thread library silently stops
             * working: it asks for an operation, is told it does not exist,
             * and carries on believing the wakeup it requested happened. */
            hw_log(VIBEOS_LOG_WARN, 25u, addr, op,
                   "futex: unsupported operation");
            return -VIBEOS_ENOSYS;
    }
}

/* ---- the calls that were a few lines inside the dispatcher --------------------
 *
 * They are functions now so that a row can name them like any other handler. */

/* The thread group, not the thread. Every thread of a program gets the same
 * answer here, which is the whole point of the distinction: getpid() names the
 * process. */
static long linux_sys_getpid(void) {
    return (g_current_task >= 0) ? (long)g_tasks[g_current_task].id.tgid : 1;
}

/* The thread id proper. Equal to getpid() for a single-threaded program, which is
 * what Linux reports too, and different for every thread of a program that has
 * several. */
static long linux_sys_gettid(void) {
    return (g_current_task >= 0) ? (long)g_tasks[g_current_task].id.pid : 1;
}

static long linux_sys_getppid(void) {
    return (g_current_task >= 0) ? (long)g_tasks[g_current_task].id.ppid : 0;
}

static long linux_sys_getpgrp(void) {
    return (g_current_task >= 0 && g_tasks[g_current_task].id.is_user) ?
        (long)g_tasks[g_current_task].id.pgid : -VIBEOS_EINVAL;
}

/* Where to write zero and wake when this thread exits. A joiner sleeps on that
 * word, so recording it is half of what makes pthread_join return; the other half
 * is exit doing the writing. */
static long linux_sys_set_tid_address(uint64_t addr) {
    if (g_current_task >= 0) {
        g_tasks[g_current_task].id.clear_child_tid = addr;
        return (long)g_tasks[g_current_task].id.pid;
    }
    return 1;
}

/* Give up the rest of this slice honestly: hlt parks the CPU until the next timer
 * interrupt, which is where the switch happens. */
static long linux_sys_yield(void) {
    __asm__ __volatile__("sti; hlt");
    return 0;
}

static long linux_sys_exit(uint64_t code) {
    hw_task_exit(code);   /* retires this task and switches away; no return */
    return 0;
}

static long linux_sys_exit_group(uint64_t code) {
    hw_task_exit_group(code);   /* the whole process; no return */
    return 0;
}

/* A C library does not call fork(); it calls clone() with the flags that happen to
 * mean fork - a new address space, a new process, SIGCHLD to the parent. Sharing
 * the address space *and* being a thread is a thread; a private address space is a
 * process. The other two combinations are refused rather than approximated: a
 * thread with a private address space, or a vfork-like sharing without being a
 * thread, would be something that only looks like what it claims to be. */
static long linux_sys_clone(const vibeos_call_t *c) {
    uint64_t flags = ARG(0);

    if ((flags & CLONE_THREAD) != 0u) {
        if ((flags & CLONE_VM) == 0u) {
            return -VIBEOS_ENOSYS;
        }
        return hw_sys_clone_thread(FRAME, ARG(0), ARG(1), ARG(2), ARG(3), ARG(4));
    }
    if ((flags & CLONE_VM) != 0u) {
        return -VIBEOS_ENOSYS;   /* vfork-like sharing: not supported */
    }
    return hw_sys_fork(FRAME);
}

/* ---- the syscalls this file implements ---------------------------------------
 *
 * Notes that belong to a row rather than to a handler:
 *   vfork    a real vfork shares the parent's address space until exec; a private
 *            fork keeps the observable contract without a parent that a
 *            still-running child can modify. ash uses it for the short child->exec
 *            path, and it is bound by the same exec/exit ABI as vfork's supported
 *            use in this personality.
 *   clone    the op says THREAD_CREATE because that is the one that needs the most
 *            checks; a clone that means fork takes the fork path inside.
 *   uids     everything runs as the one identity this system has.
 *   rseq     an optimisation with a mandatory fallback: ENOSYS makes the libc take
 *            the fallback, where claiming success would make it run a fast path
 *            this kernel does not implement.
 *   getrandom  there is no entropy source yet, and predictable bytes from the
 *            syscall a program uses for keys are worse than refusing: ENOSYS is
 *            visible, weak randomness is not.
 *   set_robust_list  walked only when a thread dies holding a robust mutex; no
 *            such thing exists here, so there is nothing to walk. */
#define LINUX_PROC_SYSCALLS(X) \
    X(24,  sched_yield,      YIELD,           NOPTR, linux_sys_yield()) \
    X(39,  getpid,           GETPID,          NOPTR, linux_sys_getpid()) \
    X(56,  clone,            THREAD_CREATE,   NOPTR, linux_sys_clone(c)) \
    X(57,  fork,             FORK,            NOPTR, hw_sys_fork(FRAME)) \
    X(58,  vfork,            FORK,            NOPTR, hw_sys_fork(FRAME)) \
    X(59,  execve,           EXEC,            NOPTR, hw_sys_execve(FRAME, ARG(0), ARG(1), ARG(2))) \
    X(60,  exit,             EXIT,            NOPTR, linux_sys_exit(ARG(0))) \
    X(61,  wait4,            WAIT,            NOPTR, hw_sys_waitpid(ARG(0), ARG(1), ARG(2))) \
    X(102, getuid,           IDENTITY_GET,    NOPTR, 0) \
    X(104, getgid,           IDENTITY_GET,    NOPTR, 0) \
    X(105, setuid,           IDENTITY_SET,    NOPTR, hw_sys_setresid(ARG(0))) \
    X(106, setgid,           IDENTITY_SET,    NOPTR, hw_sys_setresid(ARG(0))) \
    X(107, geteuid,          IDENTITY_GET,    NOPTR, 0) \
    X(108, getegid,          IDENTITY_GET,    NOPTR, 0) \
    X(109, setpgid,          SETPGID,         NOPTR, hw_sys_setpgid(ARG(0), ARG(1))) \
    X(110, getppid,          GETPPID,         NOPTR, linux_sys_getppid()) \
    X(111, getpgrp,          GETPGRP,         NOPTR, linux_sys_getpgrp()) \
    X(112, setsid,           SETSID,          NOPTR, hw_sys_setsid()) \
    X(124, getsid,           GETSID,          NOPTR, hw_sys_getsid(ARG(0))) \
    X(157, prctl,            PRCTL,           PTRS(IN_IF(0, PR_SET_NAME, 1, 16), OUT_IF(0, PR_GET_NAME, 1, 16)), hw_sys_prctl(ARG(0), ARG(1))) \
    X(158, arch_prctl,       ARCH_PRCTL,      PTRS(OUT_IF(0, ARCH_GET_FS, 1, 8)), hw_sys_arch_prctl(ARG(0), ARG(1))) \
    X(186, gettid,           GETTID,          NOPTR, linux_sys_gettid()) \
    X(202, futex,            FUTEX,           PTRS(IN_IFM_ERR(1, FUTEX_CMD_MASK, FUTEX_WAIT, 0, 4, VIBEOS_EINVAL)), hw_sys_futex(ARG(0), ARG(1), ARG(2))) \
    X(218, set_tid_address,  SET_TID_ADDRESS, NOPTR, linux_sys_set_tid_address(ARG(0))) \
    X(231, exit_group,       EXIT_GROUP,      NOPTR, linux_sys_exit_group(ARG(0))) \
    X(273, set_robust_list,  SET_ROBUST_LIST, NOPTR, 0) \
    X(302, prlimit64,        PRLIMIT,         PTRS(OUT_OPT(3, 16)), hw_sys_prlimit64(ARG(1), ARG(2), ARG(3))) \
    X(318, getrandom,        GETRANDOM,       NOPTR, -VIBEOS_ENOSYS) \
    X(334, rseq,             RSEQ,            NOPTR, -VIBEOS_ENOSYS)

LINUX_DEFINE_SYSCALLS(proc, LINUX_PROC_SYSCALLS)
