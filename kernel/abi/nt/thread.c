/* NT personality: threads (docs/abi/windows.md W5).
 *
 * An NT thread is a task in its process, made through the same door a Linux
 * thread is - a slot, a kernel stack, the process state shared - with what
 * Windows gives a thread on top: a TEB and a stack of its own in the process's
 * memory, made by the kernel, and a start in ntdll. The first thread starts at
 * ntdll's entry with the PEB in rcx; a later one at the same entry with rcx
 * zero and the routine and its parameter in rdx and r8, so the kernel needs
 * to know no export of ntdll.
 *
 * A thread's handle is an object of its own: waited on, signalled when the
 * thread ends, and asked for an exit code after the slot has gone to somebody
 * else. So it keeps the code itself and names the thread by slot and tenancy,
 * never by the slot alone. */

#include "nt_internal.h"

#define NT_THREAD_ALL_ACCESS        0x001FFFFFu
#define NT_THREAD_TERMINATE         0x0001u
#define NT_THREAD_CREATE_SUSPENDED  0x0001u   /* NtCreateThreadEx's flag   */
#define NT_THREAD_STACK_DEFAULT     (1024u * 1024u)   /* Windows' default reserve */
#define NT_STILL_ACTIVE             0x00000103u       /* STATUS_PENDING         */

/* ---- the thread type ----------------------------------------------------------------- */

/* Has the thread ended? Its own record says so when it ended through
 * NtTerminateThread; a slot that is not the tenancy the object names, or is
 * dying, says so for a thread that ended any other way - its process killed. */
static int thread_ended(const nt_ob_t *o) {
    vibeos_task_state_t s;

    if (o->u.th.ended) {
        return 1;
    }
    if (o->u.th.slot < 0) {
        return 0;   /* made, not yet started */
    }
    s = vibeos_task_state((uint32_t)o->u.th.slot);
    return ks_seq(o->u.th.slot) != o->u.th.seq || s == VIBEOS_TASK_ZOMBIE || s == VIBEOS_TASK_FREE;
}

static int thread_signaled(nt_ob_t *o, uint32_t tid) {
    (void)tid;
    return thread_ended(o);
}

static uint32_t thread_acquire(nt_ob_t *o, uint32_t tid) {
    (void)o;
    (void)tid;
    return 0;   /* an ended thread stays signalled for every waiter */
}

static const nt_type_t g_thread_type = {
    "Thread",
    NT_THREAD_ALL_ACCESS,
    0x00020048u,   /* GENERIC_READ: READ_CONTROL, GET_CONTEXT, QUERY_INFORMATION  */
    0x00020037u,   /* GENERIC_WRITE: READ_CONTROL, TERMINATE, SUSPEND_RESUME,
                    * SET_INFORMATION, SET_CONTEXT                                  */
    0x00120000u,   /* GENERIC_EXECUTE: READ_CONTROL, SYNCHRONIZE                   */
    thread_signaled,
    thread_acquire,
};

const vibeos_file_ops_t g_nt_thread_ops = {
    .name = "nt-thread",
    .release = nt_ob_release,
    .nt_type = &g_thread_type,
};

static void thread_init(nt_ob_t *o, const void *ctx) {
    (void)ctx;
    o->u.th.slot = -1;
    o->u.th.seq = 0;
    o->u.th.tid = 0;
    o->u.th.pid = 0;
    o->u.th.teb = 0;
    o->u.th.exit_code = NT_STILL_ACTIVE;
    o->u.th.ended = 0;
}

typedef struct {
    int slot;
    uint32_t seq;
    uint32_t code;
} thread_exit_t;

static void thread_mark(nt_ob_t *o, void *ctx) {
    const thread_exit_t *e = (const thread_exit_t *)ctx;

    if (o->u.th.slot == e->slot && o->u.th.seq == e->seq && !o->u.th.ended) {
        o->u.th.exit_code = e->code;
        o->u.th.ended = 1;
    }
}

/* Every thread object naming the calling thread is told how it ended. */
static void thread_record_exit(uint32_t code) {
    thread_exit_t e;

    e.slot = ks_current();
    e.seq = ks_seq(e.slot);
    e.code = code;
    nt_ob_lock();
    nt_ob_each(&g_nt_thread_ops, thread_mark, &e);
    nt_ob_unlock();
}

/* The live threads of the calling task's process, itself included: a thread
 * that has ended - a zombie leader too - does not count. */
static uint32_t threads_in_process(int me) {
    uint32_t i, n = 0, tgid = ks_id(me)->tgid;

    for (i = 0; i < ks_slots(); i++) {
        vibeos_task_state_t s = vibeos_task_state(i);

        if (s != VIBEOS_TASK_FREE && s != VIBEOS_TASK_ZOMBIE && s != VIBEOS_TASK_SETUP &&
            ks_id((int)i)->is_user && ks_id((int)i)->tgid == tgid) {
            n++;
        }
    }
    return n;
}

/* ---- creating one ---------------------------------------------------------------------- */

/* The TEB the new thread finds through %gs: the fields the first thread's
 * carries (vibeos_nt_build_blocks), its own stack and ids. A status. */
static uint32_t thread_teb(uint64_t teb, uint64_t peb, uint64_t stack_lo, uint64_t stack_hi,
                           uint32_t pid, uint32_t tid) {
    struct {
        uint64_t at;
        uint64_t v;
    } f[] = {
        { VIBEOS_TEB_STACK_BASE, stack_hi },
        { VIBEOS_TEB_STACK_LIMIT, stack_lo },
        { VIBEOS_TEB_SELF, teb },
        { VIBEOS_TEB_CLIENT_PROCESS, pid },
        { VIBEOS_TEB_CLIENT_THREAD, tid },
        { VIBEOS_TEB_PEB, peb },
        { VIBEOS_TEB_DEALLOCATION_STACK, stack_lo },
    };
    uint32_t i;

    for (i = 0; i < sizeof(f) / sizeof(f[0]); i++) {
        if (nt_copy_out(teb + f[i].at, &f[i].v, 8u) != 0) {
            return VIBEOS_STATUS_NO_MEMORY;
        }
    }
    return VIBEOS_STATUS_SUCCESS;
}

/* Give back a slot that never ran: the stack and process reference too, if it
 * got that far. As Linux's clone unwinds (#141). */
static void thread_unwind(int idx, int with_ps) {
    vibeos_procstate_t *ps = ks_ps(idx);

    if (with_ps && ps) {
        (void)__atomic_sub_fetch(&ps->files_users, 1u, __ATOMIC_ACQ_REL);
        ks_procstate_put(ps);
        ks_set_ps(idx, 0);
    }
    ks_free_kstack(idx);
    (void)ks_set_state(idx, VIBEOS_TASK_FREE, __func__);
}

/* The thread proper: a task in this process, started at ntdll's entry with
 * (0, start, param), on `stack_top`, with `teb` as its %gs. Its slot and tenancy
 * in *slot and *seq, its id in *tid. A status. */
static uint32_t thread_spawn(ks_regs_t *frame, uint64_t start, uint64_t param, uint64_t stack_top,
                             uint64_t teb, int suspended, int *slot, uint32_t *seq, uint32_t *tid) {
    int me = ks_current(), idx;
    vibeos_procstate_t *ps = ks_ps(me);
    vibeos_task_t *child, *parent = ks_id(me);
    const vibeos_image_t *img = ks_image(me);
    uint64_t entry = img ? img->entry : 0u;   /* ntdll's: where every thread starts */

    if (suspended) {
        return VIBEOS_STATUS_NOT_IMPLEMENTED;   /* W5 step 3: suspend and resume */
    }
    if (!ps || entry == 0u) {
        return VIBEOS_STATUS_INVALID_PARAMETER;
    }
    idx = ks_task_alloc_for_user("NtCreateThreadEx");
    if (idx < 0) {
        return VIBEOS_STATUS_INSUFFICIENT_RESOURCES;
    }
    if (ks_alloc_kstack(idx) != 0) {
        (void)ks_set_state(idx, VIBEOS_TASK_FREE, __func__);
        return VIBEOS_STATUS_NO_MEMORY;
    }
    ks_set_ps(idx, ps);
    (void)__atomic_add_fetch(&ps->refs, 1u, __ATOMIC_ACQ_REL);
    (void)__atomic_add_fetch(&ps->files_users, 1u, __ATOMIC_ACQ_REL);
    ks_thread_regs(idx, me, frame, stack_top, 0u);
    if (ks_task_start_win64(idx, entry, stack_top, 0u, start, param) != 0 || ks_gs_set(idx, teb) != 0) {
        thread_unwind(idx, 1);
        return VIBEOS_STATUS_INVALID_PARAMETER;
    }
    child = ks_id(idx);
    child->pid = ks_next_pid();
    child->tgid = parent->tgid;
    child->ppid = parent->ppid;
    child->pgid = parent->pgid;
    child->sid = parent->sid;
    child->is_thread = 1;
    child->is_user = 1;
    child->signal_stopped = 0;
    child->exit_code = 0;
    child->exit_signal = 0;
    child->clear_child_tid = 0;
    child->robust_head = 0;
    child->sig_pending = 0;
    child->sig_blocked = parent->sig_blocked;
    child->sig_saved_valid = 0;
    child->sas_sp = 0;
    child->sas_size = 0;
    child->sas_flags = 0;
    child->start_tick = ks_ticks();
    if (thread_teb(teb, img->start_arg /* the PEB */, 0u, 0u, parent->tgid, child->pid) !=
        VIBEOS_STATUS_SUCCESS) {
        thread_unwind(idx, 1);
        return VIBEOS_STATUS_NO_MEMORY;
    }
    *slot = idx;
    *seq = ks_seq(idx);
    *tid = child->pid;
    return VIBEOS_STATUS_SUCCESS;
}

/* The work both create services share, once their arguments are read. The
 * handle is made first, so a thread is never running without one to report
 * it; its memory next; the task last, and only then made runnable. */
static long thread_create(ks_regs_t *frame, uint64_t out, uint32_t want, uint64_t oa, uint64_t process,
                          uint64_t start, uint64_t param, int suspended, uint64_t reserve,
                          uint64_t client_u) {
    vibeos_file_t *f = 0;
    nt_ob_t *o;
    uint64_t h = 0, teb = 0, stack = 0, top;
    uint32_t st, seq = 0, tid = 0;
    int idx = -1, me = ks_current();
    long r;

    if (process != VIBEOS_NT_CURRENT_PROCESS) {
        return NT_STATUS(VIBEOS_STATUS_INVALID_HANDLE);   /* another process's: W6 */
    }
    if (start == 0u || !nt_user_ok(start, 1u, 0)) {
        return NT_STATUS(VIBEOS_STATUS_ACCESS_VIOLATION);
    }
    r = nt_create_common(&g_nt_thread_ops, out, want, oa, 0, thread_init, 0);
    if (r != NT_STATUS(VIBEOS_STATUS_SUCCESS)) {
        return r;
    }
    if (nt_copy_in(&h, out, sizeof(h)) != 0 ||
        nt_handle_ref(h, &g_nt_thread_ops, 0u, &f, 0) != VIBEOS_STATUS_SUCCESS) {
        return NT_STATUS(VIBEOS_STATUS_ACCESS_VIOLATION);
    }
    reserve = reserve ? (reserve + 0xFFFFu) & ~0xFFFFull : NT_THREAD_STACK_DEFAULT;
    st = nt_alloc_rw(VIBEOS_TEB_PAGES * 4096u, &teb);
    if (st == VIBEOS_STATUS_SUCCESS) {
        st = nt_alloc_rw(reserve, &stack);
    }
    top = stack + reserve - 0x28u;   /* a call's frame: aligned to 16 once the return is pushed */
    if (st == VIBEOS_STATUS_SUCCESS) {
        st = thread_spawn(frame, start, param, top, teb, suspended, &idx, &seq, &tid);
    }
    if (st == VIBEOS_STATUS_SUCCESS) {
        /* The stack's own range, now that it is known. */
        uint64_t lo = stack, hi = stack + reserve;

        (void)nt_copy_out(teb + VIBEOS_TEB_STACK_BASE, &hi, 8u);
        (void)nt_copy_out(teb + VIBEOS_TEB_STACK_LIMIT, &lo, 8u);
        (void)nt_copy_out(teb + VIBEOS_TEB_DEALLOCATION_STACK, &lo, 8u);
        nt_ob_lock();
        o = nt_ob_of(f);
        o->u.th.slot = idx;
        o->u.th.seq = seq;
        o->u.th.tid = tid;
        o->u.th.pid = ks_id(me)->tgid;
        o->u.th.teb = teb;
        nt_ob_unlock();
        if (client_u != 0u) {
            uint64_t cid[2];

            cid[0] = ks_id(me)->tgid;
            cid[1] = tid;
            (void)nt_copy_out(client_u, cid, sizeof(cid));
        }
        (void)ks_set_state(idx, VIBEOS_TASK_READY, __func__);
        ks_mark_ready(idx, "NtCreateThreadEx");
    } else {
        if (stack) {
            nt_free_alloc(stack, reserve);
        }
        if (teb) {
            nt_free_alloc(teb, VIBEOS_TEB_PAGES * 4096u);
        }
    }
    vibeos_file_put(f);
    if (st != VIBEOS_STATUS_SUCCESS) {
        (void)nt_close_handle(h);
        return NT_STATUS(st);
    }
    return NT_STATUS(VIBEOS_STATUS_SUCCESS);
}

/* NtCreateThreadEx(ThreadHandle, DesiredAccess, ObjectAttributes, ProcessHandle,
 * StartRoutine, Argument, CreateFlags, ZeroBits, StackSize, MaximumStackSize,
 * AttributeList). The stack is MaximumStackSize, or StackSize, or a megabyte,
 * committed as it is touched; the attribute list - client id, TEB address -
 * is read for the client id only. */
static long nt_create_thread_ex(ks_regs_t *frame, uint64_t out, uint64_t want, uint64_t oa,
                                uint64_t process, uint64_t start, uint64_t param, uint64_t flags,
                                uint64_t stack_size, uint64_t max_stack) {
    return thread_create(frame, out, (uint32_t)want, oa, process, start, param,
                         (flags & NT_THREAD_CREATE_SUSPENDED) != 0u,
                         max_stack ? max_stack : stack_size, 0u);
}

/* NtCreateThread(ThreadHandle, DesiredAccess, ObjectAttributes, ProcessHandle,
 * ClientId, ThreadContext, InitialTeb, CreateSuspended): the older form, which
 * passes a whole CONTEXT. Its rip and rcx are the routine and the argument the
 * Win32 start in kernel32 put there; the stack the caller made is not used -
 * the kernel makes one, as NtCreateThreadEx does. */
static long nt_create_thread(ks_regs_t *frame, uint64_t out, uint64_t want, uint64_t oa,
                             uint64_t process, uint64_t client_u, uint64_t ctx_u,
                             uint64_t suspended) {
    uint64_t rcx = 0, rip = 0;

    if (ctx_u == 0u || nt_copy_in(&rcx, ctx_u + VIBEOS_CONTEXT_RCX, 8u) != 0 ||
        nt_copy_in(&rip, ctx_u + VIBEOS_CONTEXT_RIP, 8u) != 0) {
        return NT_STATUS(VIBEOS_STATUS_ACCESS_VIOLATION);
    }
    return thread_create(frame, out, (uint32_t)want, oa, process, rip, rcx, suspended != 0u, 0u,
                         client_u);
}

/* ---- ending one ------------------------------------------------------------------------ */

/* NtTerminateThread(ThreadHandle, ExitStatus). The calling thread - by 0, by
 * the pseudo-handle, or by a handle naming it - ends with the status, and the
 * process with it when it is the last. Another thread is W5 step 3's. */
static long nt_terminate_thread(uint64_t h, uint64_t status) {
    int me = ks_current();

    if (h != 0u && h != VIBEOS_NT_CURRENT_THREAD) {
        vibeos_file_t *f = 0;
        nt_ob_t *o;
        int mine;
        uint32_t st = nt_handle_ref(h, &g_nt_thread_ops, NT_THREAD_TERMINATE, &f, 0);

        if (st != VIBEOS_STATUS_SUCCESS) {
            return NT_STATUS(st);
        }
        nt_ob_lock();
        o = nt_ob_of(f);
        mine = o->u.th.slot == me && o->u.th.seq == ks_seq(me);
        nt_ob_unlock();
        vibeos_file_put(f);
        if (!mine) {
            return NT_STATUS(VIBEOS_STATUS_NOT_IMPLEMENTED);   /* another thread: step 3 */
        }
    }
    thread_record_exit((uint32_t)status);
    if (threads_in_process(me) <= 1u) {
        ks_task_exit_group((uint64_t)(uint32_t)status);   /* the last thread: the process */
    }
    ks_task_exit((uint64_t)(uint32_t)status);
    return NT_STATUS(VIBEOS_STATUS_SUCCESS);   /* not reached */
}

#define NT_THREAD_SERVICES(X) \
    X(VIBEOS_NT_NR_NtCreateThreadEx, NtCreateThreadEx, THREAD_CREATE, \
      PTRS(OUT(0, 8)), \
      nt_create_thread_ex(FRAME, ARG(0), NT_ARG32(1), ARG(2), ARG(3), ARG(4), ARG(5), NT_ARG32(6), \
                          ARG(8), ARG(9))) \
    X(VIBEOS_NT_NR_NtCreateThread, NtCreateThread, THREAD_CREATE, \
      PTRS(OUT(0, 8)), \
      nt_create_thread(FRAME, ARG(0), NT_ARG32(1), ARG(2), ARG(3), ARG(4), ARG(5), ARG(7) & 0xFFu)) \
    X(VIBEOS_NT_NR_NtTerminateThread, NtTerminateThread, EXIT, NOPTR, \
      nt_terminate_thread(ARG(0), NT_ARG32(1)))

NT_DEFINE_SERVICES(thread, NT_THREAD_SERVICES)
