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

static vibeos_lock_t g_exec_lock;

static int linux_exec_cache_hit(const char *path) {
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

/* fork(): duplicate the calling task, address space and all. The child resumes
 * at the same instruction with a 0 return value. `exit_sig` is what its parent
 * is sent when it ends - SIGCHLD from fork, whatever clone and clone3 were
 * told, 0 for nothing. */
static long linux_fork(const ks_regs_t *frame, uint32_t exit_sig) {
    vibeos_task_t *parent, *child;
    vibeos_procstate_t *pps, *cps;
    int me, idx;
    uint32_t my_tenancy;

    me = ks_current();
    if (me < 0 || !ks_id(me)->is_user) {
        return -VIBEOS_EINVAL;
    }

    /* The guard and the allocation, one critical section. EAGAIN rather than
     * ENOMEM: the refusal is temporary by nature, since a child exiting undoes
     * it, and that is what a C library turns into "resource temporarily
     * unavailable" and what a shell retries on. */
    /* RLIMIT_NPROC (L2 step 4), before a slot is taken. */
    {
        long lim = linux_nproc_check();
        if (lim != 0) {
            return lim;
        }
    }
    idx = ks_task_alloc_for_user("fork");
    if (idx < 0) {
        /* DEBUG, not WARN. ks_task_alloc_for_user already prints a line
         * naming which rule refused, which is strictly more than this said -
         * and a warning that a service provokes on purpose is a warning people
         * learn to scroll past. It stays in the ring for a failure that turns
         * out to be about one of these. */
        ks_log(VIBEOS_LOG_DEBUG, 7u, 0, 0,
               "fork refused: guard or no free task slot");
        return -VIBEOS_EAGAIN;
    }
    parent = ks_id(me);
    child = ks_id(idx);
    pps = ks_ps(me);
    my_tenancy = ks_seq(idx);
    if (!pps) {
        /* A task that has let go of its process state is on its way out and
         * has nothing to fork from. It cannot be here today - only exit lets
         * go, and exit does not return - but everything below dereferences the
         * pointer, and three callers in four of ks_ps check it (code scanning,
         * alert 147). The slot is all that was taken. */
        (void)ks_set_state(idx, VIBEOS_TASK_FREE, __func__);
        return -VIBEOS_EINVAL;
    }

    /* From here to the vma clone below reads the parent's address space - the
     * page tables and the region list - which a sibling thread's brk (and,
     * once converted, mmap/munmap/mprotect) mutates. Held so fork sees one
     * consistent state, not a walk racing a half-finished mapping. */
    ks_mm_lock(pps);
    if (ks_fork_aspace(idx, me) != 0) {
        /* Give the tables back before the slot is published as reusable.
         * Publishing first leaves an address space nothing will ever free -
         * the next tenant overwrites the pointer - and it leaves a FREE slot
         * naming live tables, which is a question the sharing test skips. */
        ks_mm_unlock(pps);
        ks_drop_aspace(idx);
        (void)ks_set_state(idx, VIBEOS_TASK_FREE, __func__);
        return -VIBEOS_ENOMEM;
    }
    if (ks_alloc_kstack(idx) != 0) {
        ks_mm_unlock(pps);
        ks_drop_aspace(idx);
        (void)ks_set_state(idx, VIBEOS_TASK_FREE, __func__);
        return -VIBEOS_ENOMEM;
    }
    /* The child's regions are its own from the first instruction. Copying the
     * proc struct copied the list *head*, which would have left two processes
     * sharing one chain of descriptors - and the first munmap in either would
     * have unlinked descriptors out from under the other. */
    cps = ks_procstate_new();
    ks_set_ps(idx, cps);
    if (!cps || vibeos_vma_clone(&cps->vmas, &pps->vmas) != 0) {
        ks_mm_unlock(pps);
        ks_procstate_put(cps);
        ks_set_ps(idx, 0);
        ks_drop_aspace(idx);
        (void)ks_set_state(idx, VIBEOS_TASK_FREE, __func__);
        return -VIBEOS_ENOMEM;
    }
    /* The break and the mapping cursor, under the same lock as the tables and
     * the list they describe (M-078). They were copied after the release, and
     * a sibling thread's brk or mmap fits in that gap: the child then had the
     * regions and pages of one moment and the break of the next, and its own
     * next brk mapped over pages it already had, or gave back ones it never
     * got. The cursor's load was atomic, which made each read safe and the
     * two of them together no more one snapshot than before. */
    cps->brk_cur = pps->brk_cur;
    __atomic_store_n(&cps->mmap_cur,
                     __atomic_load_n(&pps->mmap_cur, __ATOMIC_ACQUIRE),
                     __ATOMIC_RELEASE);
    /* The parent's tables and list have both been read; release before the
     * child-only setup below. */
    ks_mm_unlock(pps);
    vibeos_task_stats()->forks++;
    /* Where the parent was, and above what it could not climb (A4). */
    {
        uint32_t i;
        ks_lock(&pps->files_lock, __func__);
        for (i = 0; i < VIBEOS_PATH_MAX; i++) {
            cps->cwd[i] = pps->cwd[i];
            cps->root[i] = pps->root[i];
        }
        cps->umask = pps->umask;
        cps->cred = pps->cred;   /* a child is who its parent was (L2) */
        /* And has its limits and personality (L2 step 4). */
        for (i = 0; i < VIBEOS_RLIM_COUNT; i++) {
            cps->rlim_cur[i] = pps->rlim_cur[i];
            cps->rlim_max[i] = pps->rlim_max[i];
        }
        cps->personality = pps->personality;
        ks_unlock(&pps->files_lock);
        /* Its own CPU time counts from zero against an inherited RLIMIT_CPU. */
        if (cps->rlim_cur[LINUX_RLIMIT_CPU] != VIBEOS_RLIM_INFINITY ||
            cps->rlim_max[LINUX_RLIMIT_CPU] != VIBEOS_RLIM_INFINITY) {
            linux_rlimit_cpu_arm(child->tgid, cps, 0);
        }
        (void)ks_task_set_nice(idx, ks_task_nice(me));   /* and its parent's nice */
    }
    /* Resume exactly where the parent is - including the vector registers and
     * the TLS base the copied image expects - except that fork() returns 0 in
     * the child. */
    ks_fork_regs(idx, me, frame);
    child->pid = ks_next_pid();
    ks_log(VIBEOS_LOG_DEBUG, 44u, (uint64_t)parent->pid, (uint64_t)child->pid,
           "fork produced a child (a0 = parent, a1 = child)");
    /* fork makes a process, so the child heads its own thread group. Its
     * parent is the *group*, not the thread that happened to call fork:
     * wait() is a process relationship. */
    child->tgid = child->pid;
    child->ppid = parent->tgid;
    child->pgid = parent->pgid;
    child->sid = parent->sid;
    child->signal_stopped = 0;
    child->is_thread = 0;
    child->clear_child_tid = 0;
    /* Open descriptors are inherited. This is not a refinement: a shell builds
     * a pipeline by creating the pipe, forking, and having the child move an
     * inherited end onto its standard output. Without inheritance the child has
     * no such descriptor, the redirection fails, and its output goes to the
     * console while the reader waits forever. A copy of the table, naming the
     * same descriptions - the child and the parent share each file's offset,
     * which is what fork promises (A3). The copy can need a page, so it can
     * fail, and the child is unwound as the other failures above unwind it. */
    if (linux_fds_copy(cps, pps) != 0) {
        ks_procstate_put(cps);
        ks_set_ps(idx, 0);
        ks_drop_aspace(idx);
        (void)ks_set_state(idx, VIBEOS_TASK_FREE, __func__);
        return -VIBEOS_ENOMEM;
    }
    {
        uint32_t sg;

        child->exit_signal = 0;
        child->exit_sig_other = exit_sig == VIBEOS_SIGCHLD ? 0u
                                : exit_sig == 0u ? (uint8_t)VIBEOS_EXIT_SIG_NONE : (uint8_t)exit_sig;
        child->start_tick = ks_ticks();
        child->cpu_base = linux_cpu_slot(idx);   /* its own CPU time starts now */
        child->sig_pending = 0;   /* pending signals are not inherited */
        child->sig_blocked = parent->sig_blocked;
        child->sig_saved_valid = 0;
        /* The alternate stack is at the same address in the copy (L2). */
        child->sas_sp = parent->sas_sp;
        child->sas_size = parent->sas_size;
        child->sas_flags = parent->sas_flags;
        for (sg = 0; sg < VIBEOS_NSIG; sg++) {
            cps->sig_handler[sg] = pps->sig_handler[sg];
            cps->sig_restorer[sg] = pps->sig_restorer[sg];
            cps->sig_flags[sg] = pps->sig_flags[sg];
            cps->sig_mask[sg] = pps->sig_mask[sg];
        }
    }
    child->is_user = 1;
    child->exit_code = 0;
    /* The slot must still be the one this fork was given.
     *
     * Everything above writes into the slot without the scheduler lock, on the
     * strength of the allocation having marked it SETUP. If that ever fails to
     * hold, two owners fill one slot and the loser's half-written task is what
     * gets scheduled - which is exactly the shape of the wedge this is
     * hunting. Saying so out loud beats inferring it from wreckage. */
    if (ks_seq(idx) != my_tenancy ||
        vibeos_task_state((uint32_t)idx) != VIBEOS_TASK_SETUP) {
        /* One line, one critical section: puts and hex each take the console lock on their own. */
        ks_con_lock();
        ks_con_puts("[SCHED] fork lost its slot: idx=0x");
        ks_con_hex((uint64_t)idx);
        ks_con_puts(" mine=0x");
        ks_con_hex((uint64_t)my_tenancy);
        ks_con_puts(" now=0x");
        ks_con_hex((uint64_t)ks_seq(idx));
        ks_con_puts(" state=0x");
        ks_con_hex((uint64_t)vibeos_task_state((uint32_t)idx));
        ks_con_puts("\n");
        ks_con_unlock();
        ks_panic("two owners filled one task slot");
    }
    (void)ks_set_state(idx, VIBEOS_TASK_READY, __func__);
    ks_mark_ready(idx, "fork");
    return (long)child->pid;
}

static long linux_sys_fork(const ks_regs_t *frame) {
    return linux_fork(frame, VIBEOS_SIGCHLD);
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
static long linux_sys_clone_thread(const ks_regs_t *frame,
                                   uint64_t flags, uint64_t child_stack,
                                   uint64_t ptid, uint64_t ctid, uint64_t tls) {
    vibeos_task_t *parent, *child;
    vibeos_procstate_t *ps;
    int me, idx;
    uint32_t my_tenancy;

    me = ks_current();
    if (me < 0 || !ks_id(me)->is_user) {
        return -VIBEOS_EINVAL;
    }
    /* A thread with no stack of its own would run on its creator's, which is
     * not a degraded thread but two threads writing to one stack. */
    if (child_stack == 0u || !linux_user_ok(child_stack - 8u, 8u, 1)) {
        ks_log(VIBEOS_LOG_WARN, 6u, child_stack, flags,
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
    /* Threads count against RLIMIT_NPROC too, as Linux counts them. */
    {
        long lim = linux_nproc_check();
        if (lim != 0) {
            return lim;
        }
    }
    idx = ks_task_alloc_for_user("clone");
    if (idx < 0) {
        ks_log(VIBEOS_LOG_WARN, 7u, flags, 0,
               "clone refused: no free task slot");
        return -VIBEOS_ENOMEM;
    }
    parent = ks_id(me);
    child = ks_id(idx);
    my_tenancy = ks_seq(idx);
    ps = ks_ps(me);
    if (!ps) {
        /* The creator has let go of its process state: it is exiting, and
         * there is no process for a thread to join. As in fork (code scanning
         * 237): it cannot be here today, and everything below uses the
         * pointer. Asked before anything but the slot is taken. */
        (void)ks_set_state(idx, VIBEOS_TASK_FREE, __func__);
        return -VIBEOS_EINVAL;
    }

    if (ks_alloc_kstack(idx) != 0) {
        (void)ks_set_state(idx, VIBEOS_TASK_FREE, __func__);
        return -VIBEOS_ENOMEM;
    }

    /* The same address space, by sharing the description rather than copying
     * the tables - ks_thread_regs shares the image and its cr3; two sets of page
     * tables would be two processes wearing one name. And the same process: a
     * reference, not a copy. The descriptor table comes with it, so the thread
     * is one more user of it. */
    vibeos_task_stats()->threads++;
    ks_set_ps(idx, ps);
    (void)__atomic_add_fetch(&ps->refs, 1u, __ATOMIC_ACQ_REL);
    (void)__atomic_add_fetch(&ps->files_users, 1u, __ATOMIC_ACQ_REL);

    /* On the stack the library gave it, with no caller frame, a clean FP
     * environment - a new thread begins at a function entry, so there is no
     * partly-built vector value to inherit - and its own thread-local storage.
     * Without that last one every thread reads the creator's errno and its own
     * stack guard, which is the kind of sharing that looks like memory
     * corruption from user space. */
    ks_thread_regs(idx, me, frame, child_stack,
                   (flags & LINUX_CLONE_SETTLS) ? tls : ks_tls_get(me));

    child->pid = ks_next_pid();
    child->tgid = parent->tgid;    /* same process */
    child->ppid = parent->ppid;    /* threads share their creator's parent */
    child->pgid = parent->pgid;
    child->sid = parent->sid;
    child->signal_stopped = 0;
    child->is_thread = 1;
    child->cpu_base = linux_cpu_slot(idx);
    child->start_tick = ks_ticks();
    (void)ks_task_set_nice(idx, ks_task_nice(me));   /* a thread starts at its creator's nice */
    child->is_user = 1;
    child->exit_code = 0;
    child->exit_signal = 0;

    child->clear_child_tid = (flags & LINUX_CLONE_CHILD_CLEARTID) ? ctid : 0;

    /* Descriptors are shared, not copied: the table is in the process state
     * the thread has just taken a reference to. It used to be copied here, so
     * opening a file in one thread was invisible to the others and a close
     * left the siblings holding the descriptor - THREADS_C5_FILES checks both
     * directions. */

    /* Signal dispositions are the process's. That sentence used to sit above
     * a loop that copied them, so a handler installed in one thread was never
     * seen by another - verified by THREADS_C5_SIGACTION, which died by
     * SIGUSR1. They are shared through the process state now. The mask and the
     * pending set stay per thread, which is the Linux model. */
    child->sig_pending = 0;
    child->sig_blocked = parent->sig_blocked;
    /* A new thread has no alternate stack: two threads on one would run their
     * handlers over each other (Linux clears it for CLONE_VM). */
    child->sig_saved_valid = 0;
    child->sas_sp = 0;
    child->sas_size = 0;
    child->sas_flags = 0;

    /* Written through the fault-safe copy: a sibling thread can munmap the page
     * between the range check and the store, faulting in ring 0 (H-021). A
     * failed write is dropped - the thread is created either way, as it is on
     * Linux when these optional stores fault. */
    if ((flags & LINUX_CLONE_PARENT_SETTID) && ptid != 0u &&
        linux_user_ok(ptid, 4u, 1)) {
        uint32_t v = child->pid;
        (void)vibeos_uaccess_copy((void *)(uintptr_t)ptid, &v, sizeof(v));
    }
    if ((flags & LINUX_CLONE_CHILD_SETTID) && ctid != 0u &&
        linux_user_ok(ctid, 4u, 1)) {
        uint32_t v = child->pid;
        (void)vibeos_uaccess_copy((void *)(uintptr_t)ctid, &v, sizeof(v));
    }

    if (ks_seq(idx) != my_tenancy ||
        vibeos_task_state((uint32_t)idx) != VIBEOS_TASK_SETUP) {
        ks_panic("two owners filled one task slot");
    }
    (void)ks_set_state(idx, VIBEOS_TASK_READY, __func__);
    ks_mark_ready(idx, "clone_thread");
    ks_log(VIBEOS_LOG_DEBUG, 8u, (uint64_t)child->pid, ks_tls_get(idx),
           "thread created (a1 = its TLS base)");
    return (long)child->pid;
}

/* ---- waiting for a child (docs/abi/ L2 step 5) -------------------------------------
 *
 * One engine under wait4 and waitid. It used to be wait4 alone, and it heard
 * only "any child" and "this pid": waitpid(0) and waitpid(-pgid) - a shell's
 * job control - were matched as a pid of 0 or of 4294967xxx and answered
 * ECHILD.
 *
 * What is selected: every child, one by pid, or a process group's. A child
 * that has ended is reported and, unless the caller asked only to look
 * (WNOWAIT), reaped. Stopped and continued children are not reported: this
 * kernel does not keep that state for a parent (the registry's gap). */

#define LINUX_WAIT_ANY  0u
#define LINUX_WAIT_PID  1u
#define LINUX_WAIT_PGID 2u

typedef struct {
    uint32_t pid;
    uint32_t uid;
    int status;           /* the wait status word: what wait4 hands back       */
    uint32_t signal;      /* the signal that ended it, or 0                     */
    uint32_t code;        /* the exit code, when it exited                      */
    uint64_t cpu;         /* the ticks it ran, for rusage and times()           */
} linux_waited_t;

/* 1 if a child was found and *out filled, 0 if there were children and none
 * had ended (only with WNOHANG), or a negated errno: ECHILD, or EINTR made a
 * restart by the dispatcher. */
static long linux_wait_child(uint32_t kind, uint32_t id, uint64_t options, int reap,
                             linux_waited_t *out) {
    uint32_t mypid = ks_id(ks_current())->tgid;

    for (;;) {
        int i;
        int have_children = 0;

        ks_irq_off();
        ks_lock(ks_sched_lock(), __func__);
        for (i = 0; i < (int)ks_slots(); i++) {
            vibeos_task_t *t = ks_id(i);

            if (t->ppid != mypid || vibeos_task_state((uint32_t)i) == VIBEOS_TASK_FREE ||
                vibeos_task_state((uint32_t)i) == VIBEOS_TASK_SETUP || t->is_thread) {
                continue;
            }
            if ((kind == LINUX_WAIT_PID && t->tgid != id) ||
                (kind == LINUX_WAIT_PGID && t->pgid != id)) {
                continue;
            }
            have_children = 1;
            if (vibeos_task_state((uint32_t)i) != VIBEOS_TASK_ZOMBIE) {
                continue;
            }
            {
                const vibeos_task_account_t *acct = vibeos_account_task((uint32_t)i);

                out->pid = t->tgid;
                out->uid = t->exit_uid;
                out->signal = t->exit_signal;
                out->code = (uint32_t)t->exit_code;
                /* One encoding, defined once. A wait status is not an exit
                 * code, and an init that read only the code byte reported a
                 * segfault as a clean stop. */
                out->status = vibeos_wait_status_make((uint32_t)t->exit_code, t->exit_signal);
                out->cpu = acct && acct->ticks > t->cpu_base ? acct->ticks - t->cpu_base : 0u;
            }
            if (reap) {
                /* Publishing the slot as FREE is the last thing done to it, and
                 * everything still needed from it was taken above. The kernel
                 * stack is not the reaper's: it was parked on the core the task
                 * exited on, which frees it once it is running on another (a
                 * reaper that freed it was freeing the stack under the dying
                 * core's feet, about one boot in six). */
                (void)vibeos_teardown_step((uint32_t)i, VIBEOS_TEARDOWN_HARVESTED);
                vibeos_task_stats()->reaped++;
                (void)vibeos_teardown_step((uint32_t)i, VIBEOS_TEARDOWN_PUBLISHED);
                (void)ks_set_state(i, VIBEOS_TASK_FREE, __func__); /* reaped; nothing may touch t now */
            }
            ks_unlock(ks_sched_lock());
            ks_irq_on();
            if (reap) {
                vibeos_procstate_t *mps = ks_ps(ks_current());
                if (mps) {
                    __atomic_add_fetch(&mps->cpu_children, out->cpu, __ATOMIC_RELAXED);
                }
            }
            return 1;
        }
        if (!have_children) {
            ks_unlock(ks_sched_lock());
            ks_irq_on();
            return -VIBEOS_ECHILD;
        }
        if (options & LINUX_WNOHANG) {   /* children, none changed */
            ks_unlock(ks_sched_lock());
            ks_irq_on();
            return 0;
        }
        /* Block until a child exit sets us READY again - or until a signal
         * needs acting on. Blocked first, then asked, still under the lock, for
         * the same reason as the futex and console waits. */
        (void)ks_set_state(ks_current(), VIBEOS_TASK_BLOCKED, __func__);
        if (ks_signal_interrupts(ks_current())) {
            (void)ks_set_state(ks_current(), VIBEOS_TASK_READY, __func__);
            ks_mark_ready(ks_current(), "waitpid_interrupted");
            ks_unlock(ks_sched_lock());
            ks_irq_on();
            return -VIBEOS_RESTART_CALL;   /* waits again under SA_RESTART */
        }
        ks_unlock(ks_sched_lock());
        ks_block_point();
    }
}

static void linux_rusage_of(uint64_t ticks, uint64_t uptr) {
    linux_rusage_t ru;
    uint32_t i;

    for (i = 0; i < sizeof(ru); i++) {
        ((unsigned char *)&ru)[i] = 0;
    }
    ru.ru_utime.tv_sec = (int64_t)(ticks / ks_hz());
    ru.ru_utime.tv_usec = (int64_t)((ticks % ks_hz()) * (1000000ull / ks_hz()));
    (void)vibeos_uaccess_copy((void *)(uintptr_t)uptr, &ru, sizeof(ru));
}

/* wait4(): pid > 0 that child, -1 any, 0 the caller's group, < -1 group -pid. */
static long linux_sys_waitpid(uint64_t want_pid, uint64_t status_ptr, uint64_t options,
                              uint64_t rusage_ptr) {
    int32_t p = (int32_t)(uint32_t)want_pid;
    uint32_t kind, id;
    linux_waited_t w;
    long r;

    if (ks_current() < 0 || !ks_id(ks_current())->is_user) {
        return -VIBEOS_EINVAL;
    }
    /* The one pid whose group cannot be negated: Linux says no such process
     * rather than no such child (LTP's waitpid04). */
    if (p == (int32_t)0x80000000) {
        return -VIBEOS_ESRCH;
    }
    /* WNOHANG is honoured. WUNTRACED, WCONTINUED and Linux's __WALL, __WCLONE
     * and __WNOTHREAD are accepted and have no effect: this kernel reports
     * neither stopped nor continued children. Refusing them would break
     * BusyBox's shell, which passes WUNTRACED for job control. Any other bit is
     * refused, as Linux refuses it. */
    if (options & ~(uint64_t)(LINUX_WNOHANG | LINUX_WUNTRACED | LINUX_WCONTINUED |
                              LINUX_WNOTHREAD | LINUX_WALL | LINUX_WCLONE)) {
        return -VIBEOS_EINVAL;
    }
    if (p == -1) {
        kind = LINUX_WAIT_ANY;
        id = 0;
    } else if (p == 0) {
        kind = LINUX_WAIT_PGID;
        id = ks_id(ks_current())->pgid;
    } else if (p < 0) {
        kind = LINUX_WAIT_PGID;
        id = (uint32_t)-p;
    } else {
        kind = LINUX_WAIT_PID;
        id = (uint32_t)p;
    }
    r = linux_wait_child(kind, id, options, 1, &w);
    if (r <= 0) {
        return r;
    }
    if (status_ptr != 0 && linux_user_ok(status_ptr, 4, 1)) {
        /* Written through the fault-safe copy: the reap has already published
         * the slot FREE, so a sibling can munmap this page before the store
         * (H-022). The child stays consumed if it faults - the pid is returned
         * regardless, as Linux does. */
        int st = w.status;
        (void)vibeos_uaccess_copy((void *)(uintptr_t)status_ptr, &st, sizeof(st));
    }
    if (rusage_ptr != 0 && linux_user_ok(rusage_ptr, sizeof(linux_rusage_t), 1)) {
        linux_rusage_of(w.cpu, rusage_ptr);
    }
    return (long)w.pid;
}

/* waitid(): the same, with what happened told as a siginfo_t, a choice of
 * events (WEXITED is the only one this kernel reports), and WNOWAIT to look
 * without reaping. A pidfd names its process as P_PIDFD. */
static long linux_sys_waitid(uint64_t idtype, uint64_t id, uint64_t info_ptr, uint64_t options,
                             uint64_t rusage_ptr) {
    uint32_t kind, sel = (uint32_t)id;
    linux_waited_t w;
    linux_siginfo_t si;
    uint32_t i;
    long r;

    if (ks_current() < 0 || !ks_id(ks_current())->is_user) {
        return -VIBEOS_EINVAL;
    }
    if (options & ~(uint64_t)(LINUX_WNOHANG | LINUX_WSTOPPED | LINUX_WEXITED | LINUX_WCONTINUED |
                              LINUX_WNOWAIT | LINUX_WNOTHREAD | LINUX_WALL | LINUX_WCLONE)) {
        return -VIBEOS_EINVAL;
    }
    if (!(options & (LINUX_WEXITED | LINUX_WSTOPPED | LINUX_WCONTINUED))) {
        return -VIBEOS_EINVAL;   /* nothing asked for */
    }
    switch ((uint32_t)idtype) {
        case LINUX_P_ALL:
            kind = LINUX_WAIT_ANY;
            break;
        case LINUX_P_PID:
            if ((int32_t)sel <= 0) {
                return -VIBEOS_EINVAL;
            }
            kind = LINUX_WAIT_PID;
            break;
        case LINUX_P_PGID:
            if ((int32_t)sel < 0) {
                return -VIBEOS_EINVAL;
            }
            kind = LINUX_WAIT_PGID;
            if (sel == 0u) {
                sel = ks_id(ks_current())->pgid;
            }
            break;
        case LINUX_P_PIDFD: {
            long pid = linux_pidfd_pid(id);
            if (pid < 0) {
                return pid;
            }
            kind = LINUX_WAIT_PID;
            sel = (uint32_t)pid;
            break;
        }
        default:
            return -VIBEOS_EINVAL;
    }
    if (!(options & LINUX_WEXITED)) {
        /* Only stopped or continued children asked for, and those are never
         * reported here: nothing to find, as if no child ever changed. */
        if (options & LINUX_WNOHANG) {
            r = 0;
        } else {
            while (!ks_signal_interrupts(ks_current())) {
                ks_wait_tick();
            }
            return -VIBEOS_EINTR;
        }
    } else {
        r = linux_wait_child(kind, sel, options, (options & LINUX_WNOWAIT) == 0u, &w);
        if (r < 0) {
            return r;
        }
    }
    for (i = 0; i < sizeof(si); i++) {
        ((unsigned char *)&si)[i] = 0;
    }
    if (r > 0) {
        si.si_signo = (int32_t)VIBEOS_SIGCHLD;
        si.si_code = w.signal ? LINUX_CLD_KILLED : LINUX_CLD_EXITED;
        si.pid = (int32_t)w.pid;
        si.uid = w.uid;
        linux_si_set_status(&si, w.signal ? (int32_t)w.signal : (int32_t)(w.code & 0xFFu));
    }
    /* With WNOHANG and nothing to report the record is zeroes, which is how a
     * program tells "nothing yet" from a child: si_pid 0. */
    if (info_ptr != 0u &&
        vibeos_uaccess_copy((void *)(uintptr_t)info_ptr, &si, sizeof(si)) != 0) {
        return -VIBEOS_EFAULT;
    }
    if (r > 0 && rusage_ptr != 0u) {
        linux_rusage_of(w.cpu, rusage_ptr);
    }
    return 0;
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
#define LINUX_MAX_ARGV 16

#define LINUX_ARG_BYTES 512

typedef struct {
    const char *slot[LINUX_MAX_ARGV + 1];
    char store[LINUX_ARG_BYTES];
} linux_argv_t;

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

static long linux_copy_user_argv(uint64_t uvec, linux_argv_t *out) {
    uint32_t count = 0;
    uint32_t used = 0;

    out->slot[0] = 0;
    if (uvec == 0u) {
        return 0;
    }
    for (;;) {
        uint64_t ptr;
        int len;

        if (count == LINUX_MAX_ARGV) {
            g_argv_fail_why = "too_many_entries";
            return -VIBEOS_E2BIG;
        }
        {
            uint32_t why = 0;
            if (!ks_user_range_why(uvec + (uint64_t)count * 8u, 8, 0, &why)) {
                g_argv_fail_why = ks_user_range_why_name(why);
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
        if (used >= LINUX_ARG_BYTES) {
            g_argv_fail_why = "arg_bytes_exhausted";
            return -VIBEOS_E2BIG;
        }
        if (ks_copy_user_string(ptr, &out->store[used],
                                (int)(LINUX_ARG_BYTES - used)) != 0) {
            uint32_t why = 0;
            (void)ks_user_range_why(ptr, 1, 0, &why);
            g_argv_fail_why = ks_user_range_why_name(why);
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

static long linux_sys_execve(ks_regs_t *frame, uint64_t dirfd, uint64_t path_uptr,
                          uint64_t argv_uptr, uint64_t envp_uptr, uint64_t atflags) {
    char name[128];                 /* as the caller wrote it: argv[0] if none   */
    char path[VIBEOS_PATH_MAX];     /* absolute, from the working directory (A4) */
    vibeos_image_t np;
    vibeos_procstate_t *nps, *ops;
    vibeos_task_t *t;
    uint8_t *elf;
    uint32_t elf_cap;
    long n;
    uint32_t k;
    static linux_argv_t g_exec_argv;   /* under g_exec_lock, like the image buffer */
    static linux_argv_t g_exec_envp;
    const char *fallback_argv[2];
    const char *const *argv;

    if (ks_current() < 0 || !ks_id(ks_current())->is_user) {
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
    if (ks_copy_user_string(path_uptr, name, sizeof(name)) != 0) {
        return ks_exec_refuse(VIBEOS_EXEC_BAD_ARGS, "-", "path");
    }
    /* The program is found where the path says from the working directory - a
     * shell that runs ./configure after cd'ing into a directory depends on it.
     * argv[0] stays what the caller wrote: BusyBox decides which applet it is
     * from that name, not from where the file was. */
    /* execveat (L2 step 5): a directory to start from, AT_EMPTY_PATH to run
     * the file a descriptor names, AT_SYMLINK_NOFOLLOW to refuse a link. */
    if (atflags & ~(uint64_t)(LINUX_AT_EMPTY_PATH | LINUX_AT_SYMLINK_NOFOLLOW)) {
        return -VIBEOS_EINVAL;
    }
    /* "Run the program I am" - /proc/self/exe, which BusyBox's shell runs
     * every applet not built into it by - is an ordinary walk since L2 step
     * 6: /proc/self is a link to the process's directory and exe a link to
     * the program. Until then it was recognised here by its spelling, because
     * there was no /proc to hold it. */
    {
        vibeos_path_t w;
        long pr = linux_walk_at_empty(dirfd, path_uptr, atflags,
                                      (atflags & LINUX_AT_SYMLINK_NOFOLLOW) ? VIBEOS_PATH_NOFOLLOW : 0u, &w);
        if (pr == 0 && (w.node.mode & VIBEOS_S_IFMT) == VIBEOS_S_IFLNK) {
            pr = -VIBEOS_ELOOP;   /* AT_SYMLINK_NOFOLLOW, and the name is a link */
        }
        if (pr != 0) {
            (void)ks_exec_refuse(pr == -VIBEOS_ENOENT ? VIBEOS_EXEC_NOT_FOUND
                                                      : VIBEOS_EXEC_BAD_ARGS, name, "path");
            return pr;
        }
        /* The resolved path: a program reached through a symbolic link is read
         * from where the link points. */
        /* Permission to run it (L2): the execute bit for whoever is asking -
         * and for the superuser too, some execute bit, because a file nobody
         * may run is not a program. */
        if (!w.node.is_dir && linux_may(&w.node, VIBEOS_MAY_EXEC) != 0) {
            return -VIBEOS_EACCES;
        }
        for (k = 0; k + 1u < VIBEOS_PATH_MAX && w.path[k]; k++) {
            path[k] = w.path[k];
        }
        path[k] = 0;
    }
    fallback_argv[0] = name;
    fallback_argv[1] = 0;
    /* The exec staging buffer is a single shared one, so the read and the load out
     * of it have to be one critical section: two cores exec'ing at once would
     * otherwise each load the other's image. */
    ks_lock_preemptible(&g_exec_lock);
    elf = ks_exec_buffer(&elf_cap);
    {
        long na = linux_copy_user_argv(argv_uptr, &g_exec_argv);
        long ne = linux_copy_user_argv(envp_uptr, &g_exec_envp);
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
                uint32_t w = 0, ci;
                for (ci = 0; which[ci] && w < sizeof(detail) - 1u; ci++) {
                    detail[w++] = which[ci];
                }
                for (ci = 0; why && why[ci] && w < sizeof(detail) - 1u; ci++) {
                    detail[w++] = why[ci];
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
                    int cur = ks_current();
                    uint64_t cr3 = (cur >= 0) ? ks_cr3(cur) : 0ull;
                    int is_user = (cur >= 0) ? ks_id(cur)->is_user : -1;
                    int is_thread = (cur >= 0) ? ks_id(cur)->is_thread : -1;
                    const char *tag = " task=";
                    int j;

                    for (ci = 0; tag[ci] && w < sizeof(detail) - 1u; ci++) {
                        detail[w++] = tag[ci];
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
                    for (ci = 0; tag[ci] && w < sizeof(detail) - 1u; ci++) {
                        detail[w++] = tag[ci];
                    }
                    if (is_thread > 0) {
                        tag = " thread";
                        for (ci = 0; tag[ci] && w < sizeof(detail) - 1u; ci++) {
                            detail[w++] = tag[ci];
                        }
                    }
                    /* And the address that was refused. A top-level entry
                     * that is absent means the whole 512 GiB region holding
                     * that address is unmapped, which is what a garbage
                     * pointer looks like and is not what a paged-out stack
                     * looks like - so the address separates the two. */
                    tag = " at=";
                    for (ci = 0; tag[ci] && w < sizeof(detail) - 1u; ci++) {
                        detail[w++] = tag[ci];
                    }
                    for (j = 15; j >= 0; j--) {
                        if (w < sizeof(detail) - 1u) {
                            detail[w++] =
                                hexd[(g_argv_fail_addr >> (j * 4)) & 0xFu];
                        }
                    }
                    tag = " poisonw=";
                    for (ci = 0; tag[ci] && w < sizeof(detail) - 1u; ci++) {
                        detail[w++] = tag[ci];
                    }
                    for (j = 1; j >= 0; j--) {
                        if (w < sizeof(detail) - 1u) {
                            detail[w++] =
                                hexd[(g_argv_poison_words >> (j * 4)) & 0xFu];
                        }
                    }
                    tag = " cr3=";
                    for (ci = 0; tag[ci] && w < sizeof(detail) - 1u; ci++) {
                        detail[w++] = tag[ci];
                    }
                    for (j = 15; j >= 0; j--) {
                        if (w < sizeof(detail) - 1u) {
                            detail[w++] = hexd[(cr3 >> (j * 4)) & 0xFu];
                        }
                    }
                }
                detail[w] = 0;
                (void)ks_exec_refuse(VIBEOS_EXEC_BAD_ARGS, path, detail);
            }
            ks_unlock_preemptible(&g_exec_lock);
            return (na < 0) ? na : ne;
        }
        /* A caller that passes no argv still gets an argv[0]: the path it was
         * started from, which is what a program prints as its own name - and
         * what BusyBox uses to decide which applet it is. */
        argv = (na > 0) ? g_exec_argv.slot : (const char *const *)fallback_argv;
    }
    if (linux_exec_cache_hit(path)) {
        n = g_exec_cached_len;   /* already staged, byte for byte */
    } else {
        linux_exec_cache_drop();
        n = ks_read_file_cached(path, elf, elf_cap, &g_exec_cached_id);
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
                for (z = (uint32_t)n; z < elf_cap; z++) {
                    elf[z] = 0;
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
        ks_log(VIBEOS_LOG_DEBUG, 3u, (uint64_t)n, 0,
               "execve could not read the program image");
        /* Counted as well as printed. This one had a message of its own, which
         * is exactly how it stayed outside the tally: a sentence in the log is
         * not a reason anything can assert on. It is also the *expected*
         * refusal on a healthy boot - a shell resolving a bare command name
         * tries it as a path first, and a C runtime asks for /proc/self/exe -
         * so the gate's expected set is not empty, and a set that must contain
         * this and nothing else is a much stronger assertion than a count. */
        (void)ks_exec_refuse(VIBEOS_EXEC_NOT_FOUND, path, "read_file");
        ks_unlock_preemptible(&g_exec_lock);
        return -VIBEOS_ENOENT;
    }
    /* The new image gets a new process. The program break, the mapping cursor,
     * the regions and the dispositions all describe the old image, and a
     * sibling thread still running that image keeps the old process. Built
     * before the old one is let go, which is why the pool has room for an exec
     * per core. */
    ops = ks_ps(ks_current());
    if (!ops) {
        ks_unlock_preemptible(&g_exec_lock);
        return -VIBEOS_EINVAL;
    }
    nps = ks_procstate_new();
    if (!nps) {
        ks_unlock_preemptible(&g_exec_lock);
        return -VIBEOS_ENOMEM;
    }
    if (ks_image_create(&np, nps, elf, (uint64_t)n,
                        (uint64_t)elf_cap, argv,
                        g_exec_envp.slot[0] ? g_exec_envp.slot : 0, path,
                        g_exec_cached_id) != 0) {
        ks_procstate_put(nps);
        ks_log(VIBEOS_LOG_ERROR, 4u, (uint64_t)n, 0,
               "execve rejected the program image");
        ks_con_lock();
        ks_con_puts("[EXEC] image rejected: ");
        ks_con_puts(path);
        ks_con_puts(" bytes=0x");
        ks_con_hex((uint64_t)n);
        ks_con_puts("\n");
        ks_con_unlock();
        ks_unlock_preemptible(&g_exec_lock);
        return -VIBEOS_ENOMEM;
    }
    ks_unlock_preemptible(&g_exec_lock);

    t = ks_id(ks_current());

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
        int me = ks_current();
        uint32_t tgid = t->tgid;
        int i, any;

        /* The leader must die quietly. Exit reads is_thread under
         * g_sched_lock to choose between "released" and "zombie, wake the
         * parent", so setting it under the same lock is decided before that
         * choice or not at all. A leader that is already a zombie is handled
         * in the wait below. */
        ks_lock(ks_sched_lock(), __func__);
        for (i = 0; i < (int)ks_slots(); i++) {
            if (i != me && ks_id(i)->is_user && ks_id(i)->tgid == tgid &&
                ks_id(i)->pid == tgid &&
                vibeos_task_state((uint32_t)(i)) != VIBEOS_TASK_FREE &&
                vibeos_task_state((uint32_t)(i)) != VIBEOS_TASK_ZOMBIE) {
                ks_id(i)->is_thread = 1;
            }
        }

        /* Still under g_sched_lock: slots found by tgid can be reused the moment
         * the lock is dropped (H-007). This used to drop it first, under a note
         * that raising a signal reaches ks_set_state - which takes no lock.
         * And not through exit_group's flag: nothing that dies here should
         * report a group exit code - they are all threads now, and threads
         * report nothing. */
        for (i = 0; i < (int)ks_slots(); i++) {
            if (i != me && ks_id(i)->is_user && ks_id(i)->tgid == tgid &&
                vibeos_task_state((uint32_t)(i)) != VIBEOS_TASK_FREE &&
                vibeos_task_state((uint32_t)(i)) != VIBEOS_TASK_SETUP &&
                vibeos_task_state((uint32_t)(i)) != VIBEOS_TASK_ZOMBIE) {
                (void)ks_signal_raise(i, VIBEOS_SIGKILL);
            }
        }
        ks_unlock(ks_sched_lock());

        /* Wait until they are gone, sleeping. Unbounded on purpose: a sibling in
         * a syscall finishes it and dies on the way out, and one blocked in a
         * wait is interrupted since waits notice signals. A bound would only
         * turn a slow exec into a half-done one; if this ever hangs, the wait
         * that ignored a signal is the defect. */
        for (;;) {
            any = 0;
            ks_lock(ks_sched_lock(), __func__);
            for (i = 0; i < (int)ks_slots(); i++) {
                if (i == me || !ks_id(i)->is_user || ks_id(i)->tgid != tgid ||
                    vibeos_task_state((uint32_t)(i)) == VIBEOS_TASK_FREE) {
                    continue;
                }
                if (vibeos_task_state((uint32_t)(i)) == VIBEOS_TASK_ZOMBIE && ks_id(i)->pid == tgid) {
                    /* A leader that exited before this exec. Its id is the one
                     * this task is about to take, so the slot cannot stay: a
                     * parent reaping it afterwards would see the process end
                     * while the new image runs. Released under the lock waitpid
                     * reaps under, with the same final step the thread branch of
                     * exit uses. */
                    ks_task_release(i);
                    continue;
                }
                any = 1;
            }
            ks_unlock(ks_sched_lock());
            if (!any) {
                break;
            }
            if (ks_signal_interrupts(me)) {
                /* Something must be acted on here first - a SIGKILL from a
                 * sibling's exit_group, say. The new image is not committed:
                 * give back what was built for it and let the signal be
                 * delivered on the way out. */
                ks_image_drop(&np, "exec_interrupted");
                ks_procstate_put(nps);
                return -VIBEOS_EINTR;
            }
            ks_block_point();
        }

        /* Alone now, and the old leader's slot is gone, so no two tasks ever
         * hold pid == tgid at once. ppid is already the leader's: threads
         * inherit their creator's parent. */
        if (t->pid != tgid) {
            t->pid = tgid;
            ks_mark_ready(me, "exec_took_leader_id");
        }
        t->is_thread = 0;
    }
    /* Absolute, as the path has been since A4 resolved it from the working
     * directory. /proc/self/exe is defined to be absolute, and a C runtime does
     * not merely prefer that: glibc asserts on it during startup and aborts the
     * process, which is how the relative form was found. */
    for (k = 0; k + 1u < (uint32_t)sizeof(np.exe_path) && path[k]; k++) {
        np.exe_path[k] = path[k];
    }
    np.exe_path[k] = 0;
    {
        int me = ks_current();

        /* Caught signals revert to their default in the new image, because
         * the handler addresses point into the one that is going away. Ignored
         * stays ignored and the per-signal masks carry over, which is what
         * execve is defined to do.
         *
         * Derived here, from the outgoing process, and not after the commit:
         * the reference to it is given back below, and once the last one is
         * gone its slot can be handed to an exec or a fork on another core -
         * so reading it afterwards could copy a stranger's dispositions.
         *
         * ops is the pointer the check above looked at, not a second read:
         * asked twice, the answer checked was not the answer used (code
         * scanning 243). */
        {
            uint32_t sg;
            for (sg = 0; sg < VIBEOS_NSIG; sg++) {
                nps->sig_handler[sg] = (ops->sig_handler[sg] == SIG_IGN_ADDR)
                                       ? SIG_IGN_ADDR : SIG_DFL_ADDR;
                nps->sig_restorer[sg] = 0;
                nps->sig_flags[sg] = 0;
                nps->sig_mask[sg] = ops->sig_mask[sg];
            }
        }
        /* An exec changes the program, not where it is (A4). */
        {
            uint32_t i;
            ks_lock(&ops->files_lock, __func__);
            for (i = 0; i < VIBEOS_PATH_MAX; i++) {
                nps->cwd[i] = ops->cwd[i];
                nps->root[i] = ops->root[i];
            }
            nps->umask = ops->umask;
            /* Nor who is running it (L2). The first version copied this in
             * fork and not here, so a program was whoever had the slot before
             * it: one boot in six, root's shell was the self-test's user 1000
             * and could not create a file. */
            nps->cred = ops->cred;
            nps->cpu_children = ops->cpu_children;   /* times() survives exec too */
            {
                uint32_t lim;   /* limits and personality too (L2 step 4) */
                for (lim = 0; lim < VIBEOS_RLIM_COUNT; lim++) {
                    nps->rlim_cur[lim] = ops->rlim_cur[lim];
                    nps->rlim_max[lim] = ops->rlim_max[lim];
                }
                nps->personality = ops->personality;
            }
            ks_unlock(&ops->files_lock);
        }

        /* The descriptors survive the exec - that is how a shell hands a
         * program its redirected output - so the new process gets a copy of
         * the table and this thread stops using the old one. A threaded exec
         * leaves the siblings the old table, still open; a single-threaded one
         * was its last user and closes it, which the copy's own references
         * balance. */
        if (linux_fds_copy(nps, ops) != 0) {
            /* Nothing is committed yet - the old image and process are still
             * this task's - so the exec fails and the program carries on,
             * without the siblings it has already been made to lose. */
            ks_image_drop(&np, "exec_no_fd_page");
            ks_procstate_put(nps);
            return -VIBEOS_ENOMEM;
        }
        /* Close-on-exec is the point of the flag: a descriptor the program
         * marked is not the new image's to inherit. On the copy, which nobody
         * else can reach yet. */
        (void)vibeos_fdtable_drop_cloexec(&nps->files);
        (void)linux_files_leave(ops);

        vibeos_task_stats()->execs++;
        /* nps already carries the regions the loader built for the new image,
         * and the outgoing regions stay with the outgoing process. Clearing the
         * list here - which the first version did - throws away the description
         * of the program that is about to run, and mprotect then refuses the
         * RELRO the C library performs on its own image during startup. */
        ks_set_ps(me, nps);
        /* The address a thread asked to have zeroed on exit belongs to the image
         * that asked. Left set, the new image's exit would write zero into
         * whatever now lives at that address - found while writing H-006, and
         * true of every exec, not only a threaded one. A C library sets it
         * again at startup if it wants it. */
        t->clear_child_tid = 0;
        /* The new image is loaded, and the old address space goes - but only
         * if nobody else is running in there.
         *
         * This used to free the old tables unconditionally, and a threaded
         * process that execs shares them with its siblings: their PML4 went
         * back to the allocator, was handed to some other allocation, and was
         * written over while they were still executing. The symptom is a
         * ring-3 instruction fetch that faults with cr2 equal to rip on a page
         * the process was already running from, and a walk that stops at level
         * one with a PML4 entry holding a kernel pointer instead of a table.
         *
         * Exit has asked this question since the wedge it was written for;
         * exec never did, which is the whole defect. Not destroying is safe:
         * the last sibling to exit takes the same path and frees them then.
         * ks_exec_switch asks it. */
        ks_exec_switch(me, &np);

        /* The outgoing regions go with the last reference to the outgoing
         * process: now, for a single-threaded exec, and when the last sibling
         * still running the old image exits, for a threaded one. That is the
         * same moment the address space above is freed or kept, because the
         * siblings hold both. */
        ks_procstate_put(ops);
    }

    /* The old image is gone and its thread-local storage with it; exec returns
     * straight to user space without passing through the scheduler's restore,
     * so the registers are rewritten here: every one cleared, the entry and the
     * stack set, the TLS base zero. The dispositions were derived into the new
     * process before the commit, while the outgoing one still existed. Pending
     * signals do not survive an exec: they were raised against the old image. */
    ks_exec_regs(ks_current(), frame, np.entry, np.user_sp);
    t->sig_pending = 0;
    /* The new program does not inherit the old one's POSIX timers; its
     * interval timers it does, alarm() included (L2 step 3). */
    vibeos_ptimer_exec(t->tgid);
    /* The alternate stack was memory of the old image. */
    t->sas_sp = 0;
    t->sas_size = 0;
    t->sas_flags = 0;
    t->sig_saved_valid = 0;

    /* One line per exec: what was loaded, how big it was, and where it
      * starts. Enough to tell a failed load from a failed program without
      * being enough to drown the log. */
    /* One line, one critical section. puts and print_hex each take the
     * lock on their own, so an unbracketed multi-part message is several
     * critical sections and another core lands in the middle of it. The
     * boot gate's log-integrity check found this by seeing a hex field
     * cut off right after its "0x". */
    ks_log(VIBEOS_LOG_DEBUG, 41u, (uint64_t)n, np.entry,
           "execve loaded a program (a0 = bytes, a1 = entry)");
    ks_con_lock();
    ks_con_puts("[EXEC] ");
    ks_con_puts(path);
    ks_con_puts(" bytes=0x");
    ks_con_hex((uint64_t)n);
    ks_con_puts(" entry=0x");
    ks_con_hex(np.entry);
    ks_con_puts("\n");
    ks_con_unlock();
    return 0; /* frame replaced; syscall return enters the new image */
}

/* prctl(): the process name is the operation programs actually use, and it is
 * stored rather than acknowledged - it costs sixteen bytes and makes the
 * scheduler's log say which program a pid is. */
static long linux_sys_prctl(uint64_t op, uint64_t arg) {
    vibeos_task_t *t;

    if (ks_current() < 0) {
        return -VIBEOS_EINVAL;
    }
    t = ks_id(ks_current());
    if (op == LINUX_PR_SET_NAME) {
        char kname[16];
        /* Copy in fault-safe, then terminate: a sibling munmap between the
         * check and the read would fault in ring 0 (uaccess follow-up). */
        if (vibeos_uaccess_copy(kname, (const void *)(uintptr_t)arg, 16) != 0) {
            return -VIBEOS_EFAULT;
        }
        vibeos_task_set_comm(t, kname, sizeof(kname));
        return 0;
    }
    if (op == LINUX_PR_GET_NAME) {
        if (vibeos_uaccess_copy((void *)(uintptr_t)arg, t->comm, 16) != 0) {
            return -VIBEOS_EFAULT;
        }
        return 0;
    }
    return -VIBEOS_EINVAL;
}

/* Is this slot a user task that exists? */
static int linux_task_live(int i) {
    return ks_id(i)->is_user && vibeos_task_state((uint32_t)i) != VIBEOS_TASK_FREE &&
           vibeos_task_state((uint32_t)i) != VIBEOS_TASK_SETUP;
}

/* setpgid(pid, pgid): put a process in a group - its own, or one that exists.
 *
 * The group is the process's, and the process is every thread of it (M-083).
 * The number lives in each task, so it is written to every task of the thread
 * group, as setsid below does; it was written to the one task the pid named,
 * and the threads of a process could then be found in different groups - by
 * getpgid, and by a signal sent to one of the groups.
 *
 * A group exists while something is in it (M-079, M-083). Joining used to ask
 * whether a *task* had the group's number as its pid, which accepted a thread's
 * id, refused a group whose leader had exited, and - once M-079 required that
 * task to lead - still asked a task about a group. What is asked now is whether
 * any process of the session is in that group. EPERM if none is, as Linux says.
 *
 * Every lookup and the writes are under g_sched_lock: a slot found and a slot
 * written must be the same tenant (H-007). */
static long linux_sys_setpgid(uint64_t requested_pid, uint64_t requested_pgid) {
    uint32_t pid, tgid, sid, group;
    int target, i, found = 0;
    long r = 0;

    if (ks_current() < 0 || !ks_id(ks_current())->is_user) {
        return -VIBEOS_EINVAL;
    }
    if (VIBEOS_ARG_INT(requested_pgid) < 0) {
        return -VIBEOS_EINVAL;
    }
    pid = requested_pid == 0 ? ks_id(ks_current())->tgid : (uint32_t)requested_pid;
    ks_lock(ks_sched_lock(), __func__);
    target = ks_task_by_pid(pid);
    if (target < 0) {
        r = -VIBEOS_ESRCH;
    } else if (ks_id(target)->sid != ks_id(ks_current())->sid) {
        r = -VIBEOS_EPERM;
    } else {
        tgid = ks_id(target)->tgid;
        sid = ks_id(target)->sid;
        group = requested_pgid == 0 ? tgid : (uint32_t)requested_pgid;
        if (group != tgid) {
            for (i = 0; i < (int)ks_slots() && !found; i++) {
                found = linux_task_live(i) && ks_id(i)->pgid == group && ks_id(i)->sid == sid;
            }
            if (!found) {
                r = -VIBEOS_EPERM;
            }
        }
        for (i = 0; r == 0 && i < (int)ks_slots(); i++) {
            if (linux_task_live(i) && ks_id(i)->tgid == tgid) {
                ks_id(i)->pgid = group;
            }
        }
    }
    ks_unlock(ks_sched_lock());
    return r;
}

static long linux_sys_setsid(void) {
    int i;
    vibeos_task_t *current;
    if (ks_current() < 0 || !ks_id(ks_current())->is_user) {
        return -VIBEOS_EINVAL;
    }
    current = ks_id(ks_current());
    /* The check and the whole transition under g_sched_lock, so the "not a
     * group leader" test cannot race the assignment that follows it and no
     * sibling - setpgid, getsid, kill's group resolution, all of which read
     * these fields under the lock - observes pgid/sid mid-change (H-027, the
     * same reasoning as H-007). */
    ks_lock(ks_sched_lock(), __func__);
    if (current->pgid == current->tgid) {
        ks_unlock(ks_sched_lock());
        return -VIBEOS_EPERM;
    }
    current->sid = current->tgid;
    current->pgid = current->tgid;
    for (i = 0; i < (int)ks_slots(); i++) {
        if (i != ks_current() && ks_id(i)->is_user &&
            vibeos_task_state((uint32_t)(i)) != VIBEOS_TASK_FREE &&
            vibeos_task_state((uint32_t)(i)) != VIBEOS_TASK_SETUP &&
            ks_id(i)->tgid == current->tgid) {
            ks_id(i)->sid = current->sid;
            ks_id(i)->pgid = current->pgid;
        }
    }
    ks_unlock(ks_sched_lock());
    return (long)current->tgid;
}

static long linux_sys_getsid(uint64_t requested_pid) {
    int target;
    uint32_t pid;
    long r;
    if (ks_current() < 0 || !ks_id(ks_current())->is_user) {
        return -VIBEOS_EINVAL;
    }
    pid = requested_pid == 0 ? ks_id(ks_current())->tgid : (uint32_t)requested_pid;
    /* The sid is read from the slot the lookup found, so the two are one
     * critical section: otherwise the answer can be another process's (H-007). */
    ks_lock(ks_sched_lock(), __func__);
    target = ks_task_by_pid(pid);
    r = target < 0 ? -VIBEOS_ESRCH : (long)ks_id(target)->sid;
    ks_unlock(ks_sched_lock());
    return r;
}

/* getpgid(): the group a process is in, 0 meaning the caller. Taken from L2
 * step 5 early: musl's getpgrp() is getpgid(0), so without it every musl
 * program's getpgrp() was -ENOSYS - and LTP's kill06 killed `-getpgrp()`, which
 * is process 38, and got ESRCH. */
static long linux_sys_getpgid(uint64_t requested_pid) {
    int target;
    uint32_t pid;
    long r;

    if (ks_current() < 0 || !ks_id(ks_current())->is_user) {
        return -VIBEOS_EINVAL;
    }
    pid = (uint32_t)requested_pid == 0u ? ks_id(ks_current())->tgid : (uint32_t)requested_pid;
    /* Lookup and read as one critical section, as getsid (H-007). */
    ks_lock(ks_sched_lock(), __func__);
    target = ks_task_by_pid(pid);
    if (target < 0) {
        target = ks_task_by_tid(pid);   /* any thread's id names its process */
    }
    r = target < 0 ? -VIBEOS_ESRCH : (long)ks_id(target)->pgid;
    ks_unlock(ks_sched_lock());
    return r;
}

/* ---- who the process is (docs/abi/ L2 step 1) --------------------------------------
 *
 * The get and set calls over vibeos/cred.h, which holds the rules. Every one
 * works on the process's one copy under files_lock: read, changed and written
 * back in one critical section, so two threads changing ids cannot leave a
 * mixture neither asked for. An id of -1 means "leave it" where a call has
 * that meaning; the 32-bit argument is read as one (VIBEOS_ARG_INT is how an
 * `int` arrives). */

enum { ID_RUID, ID_EUID, ID_RGID, ID_EGID };

static long linux_sys_getid(int which) {
    vibeos_cred_t c;

    linux_cred(&c);
    switch (which) {
        case ID_RUID: return (long)c.uid;
        case ID_EUID: return (long)c.euid;
        case ID_RGID: return (long)c.gid;
        default:      return (long)c.egid;
    }
}

/* Run one of the set functions on the process's credentials. */
typedef struct {
    int op;
    uint32_t a, b, c;
    const uint32_t *list;
} linux_setid_t;

enum { SET_UID, SET_GID, SET_REUID, SET_REGID, SET_RESUID, SET_RESGID, SET_FSUID, SET_FSGID, SET_GROUPS };

static long linux_setid(const linux_setid_t *s) {
    vibeos_procstate_t *ps;
    vibeos_cred_t *c;
    long r;

    if (ks_current() < 0 || !ks_id(ks_current())->is_user || !(ps = ks_ps(ks_current()))) {
        return -VIBEOS_EINVAL;
    }
    ks_lock(&ps->files_lock, __func__);
    c = &ps->cred;
    switch (s->op) {
        case SET_UID:    r = vibeos_cred_setuid(c, s->a); break;
        case SET_GID:    r = vibeos_cred_setgid(c, s->a); break;
        case SET_REUID:  r = vibeos_cred_setreuid(c, s->a, s->b); break;
        case SET_REGID:  r = vibeos_cred_setregid(c, s->a, s->b); break;
        case SET_RESUID: r = vibeos_cred_setresuid(c, s->a, s->b, s->c); break;
        case SET_RESGID: r = vibeos_cred_setresgid(c, s->a, s->b, s->c); break;
        case SET_FSUID:  r = (long)vibeos_cred_setfsuid(c, s->a); break;
        case SET_FSGID:  r = (long)vibeos_cred_setfsgid(c, s->a); break;
        default:         r = vibeos_cred_setgroups(c, s->a, s->list); break;
    }
    ks_unlock(&ps->files_lock);
    return r;
}

static long linux_set1(int op, uint64_t a) {
    linux_setid_t s = {op, (uint32_t)a, 0, 0, 0};
    return linux_setid(&s);
}

static long linux_set2(int op, uint64_t a, uint64_t b) {
    linux_setid_t s = {op, (uint32_t)a, (uint32_t)b, 0, 0};
    return linux_setid(&s);
}

static long linux_set3(int op, uint64_t a, uint64_t b, uint64_t c) {
    linux_setid_t s = {op, (uint32_t)a, (uint32_t)b, (uint32_t)c, 0};
    return linux_setid(&s);
}

/* getresuid and getresgid: three ids, each to its own pointer. */
static long linux_sys_getres(int gids, uint64_t r_uptr, uint64_t e_uptr, uint64_t s_uptr) {
    vibeos_cred_t c;
    uint32_t v[3];

    linux_cred(&c);
    v[0] = gids ? c.gid : c.uid;
    v[1] = gids ? c.egid : c.euid;
    v[2] = gids ? c.sgid : c.suid;
    if (vibeos_uaccess_copy((void *)(uintptr_t)r_uptr, &v[0], 4u) != 0 ||
        vibeos_uaccess_copy((void *)(uintptr_t)e_uptr, &v[1], 4u) != 0 ||
        vibeos_uaccess_copy((void *)(uintptr_t)s_uptr, &v[2], 4u) != 0) {
        return -VIBEOS_EFAULT;
    }
    return 0;
}

/* getgroups(size, list): how many supplementary groups, and which. A size of
 * zero asks only how many; a size too small for them is EINVAL. */
static long linux_sys_getgroups(uint64_t size, uint64_t list_uptr) {
    vibeos_cred_t c;

    linux_cred(&c);
    if (VIBEOS_ARG_INT(size) < 0) {
        return -VIBEOS_EINVAL;
    }
    if ((uint32_t)size == 0u) {
        return (long)c.ngroups;
    }
    if ((uint32_t)size < c.ngroups) {
        return -VIBEOS_EINVAL;
    }
    if (c.ngroups != 0u &&
        (!linux_user_ok(list_uptr, (uint64_t)c.ngroups * 4u, 1) ||
         vibeos_uaccess_copy((void *)(uintptr_t)list_uptr, c.groups, (uint64_t)c.ngroups * 4u) != 0)) {
        return -VIBEOS_EFAULT;
    }
    return (long)c.ngroups;
}

/* setgroups(size, list): the superuser's alone. Thirty-two groups are kept,
 * where Linux keeps sixty-five thousand: more than that is EINVAL here. */
static long linux_sys_setgroups(uint64_t size, uint64_t list_uptr) {
    uint32_t list[VIBEOS_NGROUPS];
    linux_setid_t s = {SET_GROUPS, (uint32_t)size, 0, 0, list};

    if (size > VIBEOS_NGROUPS) {
        vibeos_cred_t c;
        linux_cred(&c);
        return c.euid == 0u ? -VIBEOS_EINVAL : -VIBEOS_EPERM;
    }
    if (size != 0u &&
        (!linux_user_ok(list_uptr, size * 4u, 0) ||
         vibeos_uaccess_copy(list, (const void *)(uintptr_t)list_uptr, size * 4u) != 0)) {
        return -VIBEOS_EFAULT;
    }
    return linux_setid(&s);
}

/* arch_prctl(): the one everything else depends on.
 *
 * On x86-64 a C runtime addresses its thread state through %fs - errno, the
 * stack-protector cookie, the locale pointer. The base of that segment lives
 * in a model-specific register, so setting it is privileged and a program
 * cannot do it itself. Until this works, a libc faults on its first line. */
static long linux_sys_arch_prctl(uint64_t code, uint64_t addr) {
    int me = ks_current();

    if (me < 0 || !ks_id(me)->is_user) {
        return -VIBEOS_EINVAL;
    }

    switch (code) {
        case LINUX_ARCH_SET_FS:
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
            if (!ks_user_addr_ok(addr)) {
                return -VIBEOS_EPERM;
            }
            ks_tls_set(me, addr);
            return 0;
        case LINUX_ARCH_GET_FS:
            /* Fault-safe: a sibling thread can munmap the page between the
             * range check and here (H-024). */
            {
                uint64_t base = ks_tls_get(me);
                if (vibeos_uaccess_copy((void *)(uintptr_t)addr, &base,
                                        sizeof(base)) != 0) {
                    return -VIBEOS_EFAULT;
                }
            }
            return 0;
        case LINUX_ARCH_SET_GS:
        case LINUX_ARCH_GET_GS:
            /* %gs holds this CPU's per-CPU block. Handing it to a program
             * would let ring 3 relocate the kernel's own state. */
            return -VIBEOS_EPERM;
        default:
            return -VIBEOS_EINVAL;
    }
}

/* ---- the calls that were a few lines inside the dispatcher --------------------
 *
 * They are functions now so that a row can name them like any other handler. */

/* The thread group, not the thread. Every thread of a program gets the same
 * answer here, which is the whole point of the distinction: getpid() names the
 * process. */
static long linux_sys_getpid(void) {
    return (ks_current() >= 0) ? (long)ks_id(ks_current())->tgid : 1;
}

/* The thread id proper. Equal to getpid() for a single-threaded program, which is
 * what Linux reports too, and different for every thread of a program that has
 * several. */
static long linux_sys_gettid(void) {
    return (ks_current() >= 0) ? (long)ks_id(ks_current())->pid : 1;
}

static long linux_sys_getppid(void) {
    return (ks_current() >= 0) ? (long)ks_id(ks_current())->ppid : 0;
}

static long linux_sys_getpgrp(void) {
    return (ks_current() >= 0 && ks_id(ks_current())->is_user) ?
        (long)ks_id(ks_current())->pgid : -VIBEOS_EINVAL;
}

/* Where to write zero and wake when this thread exits. A joiner sleeps on that
 * word, so recording it is half of what makes pthread_join return; the other half
 * is exit doing the writing. */
static long linux_sys_set_tid_address(uint64_t addr) {
    if (ks_current() >= 0) {
        ks_id(ks_current())->clear_child_tid = addr;
        return (long)ks_id(ks_current())->pid;
    }
    return 1;
}

/* Give up the rest of this slice honestly: hlt parks the CPU until the next timer
 * interrupt, which is where the switch happens. */
static long linux_sys_yield(void) {
    ks_idle();
    return 0;
}

static long linux_sys_exit(uint64_t code) {
    ks_task_exit(code);   /* retires this task and switches away; no return */
    return 0;
}

static long linux_sys_exit_group(uint64_t code) {
    ks_task_exit_group(code);   /* the whole process; no return */
    return 0;
}

/* A C library does not call fork(); it calls clone() with the flags that happen to
 * mean fork - a new address space, a new process, SIGCHLD to the parent. Sharing
 * the address space *and* being a thread is a thread; a private address space is a
 * process. The other two combinations are refused rather than approximated: a
 * thread with a private address space, or a vfork-like sharing without being a
 * thread, would be something that only looks like what it claims to be. */
/* clone3(): clone with its arguments in a structure (docs/abi/ L2 step 5). The
 * C library tries it first for threads and falls back to clone on ENOSYS, so
 * it is clone's two paths again - a thread, or a fork - with the stack given
 * as its lowest address and size rather than its top, and CLONE_PIDFD, which
 * hands the parent a pidfd for the child. What clone does not do it does not
 * do either: a CLONE_VM process (vfork's sharing), a chosen pid (set_tid), a
 * cgroup. */
static long linux_sys_clone3(const vibeos_call_t *c) {
    linux_clone_args_t a;
    uint64_t size = ARG(1), top = 0;
    uint32_t i;
    long r;

    if (ks_current() < 0 || !ks_id(ks_current())->is_user) {
        return -VIBEOS_EINVAL;
    }
    if (size < LINUX_CLONE_ARGS_SIZE_VER0) {
        return -VIBEOS_EINVAL;
    }
    if (size > 4096u) {
        return -VIBEOS_E2BIG;
    }
    /* Judged before it is read (M-082): ring 0 reads a page the program may
     * not, and a structure on one (LTP's clone302) was taken as arguments. */
    if (!linux_user_ok(ARG(0), size, 0)) {
        return -VIBEOS_EFAULT;
    }
    for (i = 0; i < sizeof(a); i++) {
        ((unsigned char *)&a)[i] = 0;
    }
    if (vibeos_uaccess_copy(&a, (const void *)(uintptr_t)ARG(0), size < sizeof(a) ? size : sizeof(a)) != 0) {
        return -VIBEOS_EFAULT;
    }
    /* A newer structure than this kernel knows is accepted if what it does not
     * know is zero, as Linux's copy_struct_from_user accepts it. */
    for (i = (uint32_t)sizeof(a); i < size; i++) {
        uint8_t b = 0;
        if (vibeos_uaccess_copy(&b, (const void *)(uintptr_t)(ARG(0) + i), 1u) != 0) {
            return -VIBEOS_EFAULT;
        }
        if (b != 0u) {
            return -VIBEOS_E2BIG;
        }
    }
    if ((a.flags >> 32) != 0u || a.exit_signal > VIBEOS_SIG_MAX ||
        a.set_tid != 0u || a.set_tid_size != 0u || (a.flags & LINUX_CLONE_INTO_CGROUP)) {
        return -VIBEOS_EINVAL;
    }
    /* Linux's rules between the flags: handlers are shared only with the
     * memory they point into, a filesystem view is not both shared and new,
     * and the pidfd and the parent's tid are not written to one place. */
    if (((a.flags & LINUX_CLONE_SIGHAND) && !(a.flags & LINUX_CLONE_VM)) ||
        ((a.flags & LINUX_CLONE_FS) && (a.flags & LINUX_CLONE_NEWNS)) ||
        ((a.flags & LINUX_CLONE_PIDFD) && (a.flags & LINUX_CLONE_PARENT_SETTID) &&
         a.pidfd == a.parent_tid)) {
        return -VIBEOS_EINVAL;
    }
    if (a.stack != 0u) {
        if (a.stack_size == 0u) {
            return -VIBEOS_EINVAL;
        }
        top = a.stack + a.stack_size;
    } else if (a.stack_size != 0u) {
        return -VIBEOS_EINVAL;
    }
    if (a.flags & LINUX_CLONE_THREAD) {
        if (!(a.flags & LINUX_CLONE_VM) || a.exit_signal != 0u || (a.flags & LINUX_CLONE_PIDFD)) {
            return -VIBEOS_EINVAL;
        }
        return linux_sys_clone_thread(FRAME, a.flags, top, a.parent_tid, a.child_tid, a.tls);
    }
    if (a.flags & LINUX_CLONE_VM) {
        return -VIBEOS_ENOSYS;   /* vfork-like sharing: not supported, as clone */
    }
    /* Where the pidfd goes is judged before there is a child, as Linux writes
     * it before its point of no return: EFAULT, and nothing made (clone302). */
    if ((a.flags & LINUX_CLONE_PIDFD) && !linux_user_ok(a.pidfd, sizeof(int), 1)) {
        return -VIBEOS_EFAULT;
    }
    r = linux_fork(FRAME, (uint32_t)a.exit_signal);
    if (r > 0 && (a.flags & LINUX_CLONE_PIDFD)) {
        /* The child exists and nobody can have reaped it: its parent is here. */
        vibeos_file_t *f;
        int slot, fd = -1;
        uint32_t seq = 0;

        ks_lock(ks_sched_lock(), __func__);
        slot = ks_task_by_tid((uint32_t)r);
        if (slot >= 0) {
            seq = ks_seq(slot);
        }
        ks_unlock(ks_sched_lock());
        f = slot >= 0 ? vibeos_open_pidfd((uint32_t)r, seq, 0) : 0;
        if (f) {
            long installed = linux_fd_install(f, VIBEOS_FD_CLOEXEC, 0);
            fd = installed >= 0 ? (int)installed : -1;
        }
        /* The child runs either way, as on Linux; a pidfd that could not be
         * made or written leaves the parent with -1 where it looks. */
        (void)vibeos_uaccess_copy((void *)(uintptr_t)a.pidfd, &fd, sizeof(fd));
    }
    return r;
}

static long linux_sys_clone(const vibeos_call_t *c) {
    uint64_t flags = ARG(0);

    if ((flags & LINUX_CLONE_THREAD) != 0u) {
        if ((flags & LINUX_CLONE_VM) == 0u) {
            return -VIBEOS_ENOSYS;
        }
        return linux_sys_clone_thread(FRAME, ARG(0), ARG(1), ARG(2), ARG(3), ARG(4));
    }
    if ((flags & LINUX_CLONE_VM) != 0u) {
        return -VIBEOS_ENOSYS;   /* vfork-like sharing: not supported */
    }
    if ((flags & LINUX_CSIGNAL) > VIBEOS_SIG_MAX) {
        return -VIBEOS_EINVAL;
    }
    return linux_fork(FRAME, (uint32_t)(flags & LINUX_CSIGNAL));
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
 *   set_robust_list  walked only when a thread dies holding a robust mutex; no
 *            such thing exists here, so there is nothing to walk. */
#define LINUX_PROC_SYSCALLS(X) \
    X(24,  sched_yield,      YIELD,           NOPTR, linux_sys_yield()) \
    X(39,  getpid,           GETPID,          NOPTR, linux_sys_getpid()) \
    X(56,  clone,            THREAD_CREATE,   NOPTR, linux_sys_clone(c)) \
    X(57,  fork,             FORK,            NOPTR, linux_sys_fork(FRAME)) \
    X(58,  vfork,            FORK,            NOPTR, linux_sys_fork(FRAME)) \
    X(59,  execve,           EXEC,            NOPTR, linux_sys_execve(FRAME, (uint64_t)(uint32_t)LINUX_AT_FDCWD, ARG(0), ARG(1), ARG(2), 0)) \
    X(322, execveat,         EXECVEAT,        NOPTR, linux_sys_execve(FRAME, ARG(0), ARG(1), ARG(2), ARG(3), ARG(4))) \
    X(435, clone3,           CLONE3,          NOPTR, linux_sys_clone3(c)) \
    X(247, waitid,           WAITID,          PTRS(OUT_OPT(2, sizeof(linux_siginfo_t)), OUT_OPT(4, sizeof(linux_rusage_t))), linux_sys_waitid(ARG(0), ARG(1), ARG(2), ARG(3), ARG(4))) \
    X(60,  exit,             EXIT,            NOPTR, linux_sys_exit(ARG(0))) \
    X(61,  wait4,            WAIT,            NOPTR, linux_sys_waitpid(ARG(0), ARG(1), ARG(2), ARG(3))) \
    X(102, getuid,           IDENTITY_GET,    NOPTR, linux_sys_getid(ID_RUID)) \
    X(104, getgid,           IDENTITY_GET,    NOPTR, linux_sys_getid(ID_RGID)) \
    X(105, setuid,           IDENTITY_SET,    NOPTR, linux_set1(SET_UID, ARG(0))) \
    X(106, setgid,           IDENTITY_SET,    NOPTR, linux_set1(SET_GID, ARG(0))) \
    X(107, geteuid,          IDENTITY_GET,    NOPTR, linux_sys_getid(ID_EUID)) \
    X(108, getegid,          IDENTITY_GET,    NOPTR, linux_sys_getid(ID_EGID)) \
    X(113, setreuid,         IDENTITY_SET,    NOPTR, linux_set2(SET_REUID, ARG(0), ARG(1))) \
    X(114, setregid,         IDENTITY_SET,    NOPTR, linux_set2(SET_REGID, ARG(0), ARG(1))) \
    X(115, getgroups,        IDENTITY_GET,    NOPTR, linux_sys_getgroups(ARG(0), ARG(1))) \
    X(116, setgroups,        IDENTITY_SET,    NOPTR, linux_sys_setgroups(ARG(0), ARG(1))) \
    X(117, setresuid,        IDENTITY_SET,    NOPTR, linux_set3(SET_RESUID, ARG(0), ARG(1), ARG(2))) \
    X(118, getresuid,        IDENTITY_GET,    PTRS(OUT(0, 4), OUT(1, 4), OUT(2, 4)), linux_sys_getres(0, ARG(0), ARG(1), ARG(2))) \
    X(119, setresgid,        IDENTITY_SET,    NOPTR, linux_set3(SET_RESGID, ARG(0), ARG(1), ARG(2))) \
    X(120, getresgid,        IDENTITY_GET,    PTRS(OUT(0, 4), OUT(1, 4), OUT(2, 4)), linux_sys_getres(1, ARG(0), ARG(1), ARG(2))) \
    X(122, setfsuid,         IDENTITY_SET,    NOPTR, linux_set1(SET_FSUID, ARG(0))) \
    X(123, setfsgid,         IDENTITY_SET,    NOPTR, linux_set1(SET_FSGID, ARG(0))) \
    X(109, setpgid,          SETPGID,         NOPTR, linux_sys_setpgid(ARG(0), ARG(1))) \
    X(110, getppid,          GETPPID,         NOPTR, linux_sys_getppid()) \
    X(111, getpgrp,          GETPGRP,         NOPTR, linux_sys_getpgrp()) \
    X(112, setsid,           SETSID,          NOPTR, linux_sys_setsid()) \
    X(124, getsid,           GETSID,          NOPTR, linux_sys_getsid(ARG(0))) \
    X(121, getpgid,          GETPGID,         NOPTR, linux_sys_getpgid(ARG(0))) \
    X(157, prctl,            PRCTL,           PTRS(IN_IF(0, LINUX_PR_SET_NAME, 1, 16), OUT_IF(0, LINUX_PR_GET_NAME, 1, 16)), linux_sys_prctl(ARG(0), ARG(1))) \
    X(158, arch_prctl,       ARCH_PRCTL,      PTRS(OUT_IF(0, LINUX_ARCH_GET_FS, 1, 8)), linux_sys_arch_prctl(ARG(0), ARG(1))) \
    X(186, gettid,           GETTID,          NOPTR, linux_sys_gettid()) \
    X(218, set_tid_address,  SET_TID_ADDRESS, NOPTR, linux_sys_set_tid_address(ARG(0))) \
    X(231, exit_group,       EXIT_GROUP,      NOPTR, linux_sys_exit_group(ARG(0))) \
    X(273, set_robust_list,  SET_ROBUST_LIST, NOPTR, 0) \
    X(334, rseq,             RSEQ,            NOPTR, -VIBEOS_ENOSYS)

LINUX_DEFINE_SYSCALLS(proc, LINUX_PROC_SYSCALLS)
