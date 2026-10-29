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
#define NT_THREAD_HIDE_FROM_DEBUGGER 0x0004u
#define NT_THREAD_SKIP_ATTACH       0x0002u   /* accepted and taken as asked: */
#define NT_THREAD_LOADER_WORKER     0x0010u   /* no DLL is told of a thread   */
#define NT_THREAD_SKIP_LOADER_INIT  0x0020u   /* here yet, nor is there a     */
#define NT_THREAD_BYPASS_SECURITY   0x0040u   /* loader to keep it from       */
#define NT_THREAD_QUERY_INFORMATION 0x0040u
#define NT_THREAD_SET_INFORMATION   0x0020u
#define NT_THREAD_SUSPEND_RESUME    0x0002u
#define NT_THREAD_GET_CONTEXT       0x0008u
#define NT_THREAD_SET_CONTEXT       0x0010u
#define NT_THREAD_ALERT             0x0004u
#define NT_MAXIMUM_SUSPEND_COUNT    127u

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

typedef struct {
    int slot;
} thread_of_slot_t;

static void thread_init_slot(nt_ob_t *o, const void *ctx) {
    int slot = ((const thread_of_slot_t *)ctx)->slot;

    thread_init(o, 0);
    o->u.th.slot = slot;
    o->u.th.seq = ks_seq(slot);
    o->u.th.tid = ks_id(slot)->pid;
    o->u.th.pid = ks_id(slot)->tgid;
}

uint32_t nt_thread_handle_for(int slot, uint32_t want, uint64_t out) {
    thread_of_slot_t c;

    c.slot = slot;
    return (uint32_t)nt_create_common(&g_nt_thread_ops, out, want, 0, 0, thread_init_slot, &c);
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
    child->nt_suspend = suspended ? 1u : 0u;   /* SETUP until a resume takes it to zero */
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
                          uint64_t start, uint64_t param, int suspended, int hidden, uint64_t reserve,
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
        o->u.th.start = start;
        o->u.th.hidden = hidden;
        nt_ob_unlock();
        if (client_u != 0u) {
            uint64_t cid[2];

            cid[0] = ks_id(me)->tgid;
            cid[1] = tid;
            (void)nt_copy_out(client_u, cid, sizeof(cid));
        }
        if (!suspended) {
            (void)ks_set_state(idx, VIBEOS_TASK_READY, __func__);
            ks_mark_ready(idx, "NtCreateThreadEx");
        }
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
    if ((flags & ~(uint64_t)(NT_THREAD_CREATE_SUSPENDED | NT_THREAD_HIDE_FROM_DEBUGGER |
                             NT_THREAD_SKIP_ATTACH | NT_THREAD_LOADER_WORKER |
                             NT_THREAD_SKIP_LOADER_INIT | NT_THREAD_BYPASS_SECURITY)) != 0u) {
        return NT_STATUS(VIBEOS_STATUS_INVALID_PARAMETER);
    }
    return thread_create(frame, out, (uint32_t)want, oa, process, start, param,
                         (flags & NT_THREAD_CREATE_SUSPENDED) != 0u,
                         (flags & NT_THREAD_HIDE_FROM_DEBUGGER) != 0u,
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
    return thread_create(frame, out, (uint32_t)want, oa, process, rip, rcx, suspended != 0u, 0, 0u,
                         client_u);
}

/* ---- naming one: the calling thread or a handle's --------------------------------------- */

/* What a service knows of the thread a handle names: the pseudo-handle and 0
 * name the caller, anything else must be a thread object granted `need`. */
typedef struct {
    int slot;
    uint32_t seq;
    uint32_t tid;
    uint32_t pid;
    uint64_t teb;
    uint64_t start;
    uint32_t exit_code;
    int ended;
    int hidden;
    vibeos_file_t *f;      /* the object's description, or 0 for the caller */
} thread_view_t;

static uint32_t thread_view(uint64_t h, uint32_t need, thread_view_t *v) {
    int me = ks_current();
    nt_ob_t *o;
    uint32_t st;

    v->f = 0;
    if (h == VIBEOS_NT_CURRENT_THREAD) {
        v->slot = me;
        v->seq = ks_seq(me);
        v->tid = ks_id(me)->pid;
        v->pid = ks_id(me)->tgid;
        v->teb = ks_gs_get(me);
        v->start = 0;
        v->exit_code = NT_STILL_ACTIVE;
        v->ended = 0;
        v->hidden = 0;
        return VIBEOS_STATUS_SUCCESS;
    }
    st = nt_handle_ref(h, &g_nt_thread_ops, need, &v->f, 0);
    if (st != VIBEOS_STATUS_SUCCESS) {
        return st;
    }
    nt_ob_lock();
    o = nt_ob_of(v->f);
    v->slot = o->u.th.slot;
    v->seq = o->u.th.seq;
    v->tid = o->u.th.tid;
    v->pid = o->u.th.pid;
    v->teb = o->u.th.teb;
    v->start = o->u.th.start;
    v->exit_code = o->u.th.exit_code;
    v->ended = thread_ended(o);
    v->hidden = (int)o->u.th.hidden;
    nt_ob_unlock();
    if (v->ended && v->exit_code == NT_STILL_ACTIVE) {
        v->exit_code = 0;   /* ended some way that left no code: its process was killed */
    }
    return VIBEOS_STATUS_SUCCESS;
}

static void thread_unview(thread_view_t *v) {
    if (v->f) {
        vibeos_file_put(v->f);
    }
}

uint32_t nt_thread_target(uint64_t h, uint32_t need, int *slot, uint32_t *seq) {
    thread_view_t v;
    uint32_t st = thread_view(h, need, &v);

    if (st != VIBEOS_STATUS_SUCCESS) {
        return st;
    }
    st = !v.ended && v.slot >= 0 && ks_seq(v.slot) == v.seq ? VIBEOS_STATUS_SUCCESS
                                                             : VIBEOS_STATUS_THREAD_IS_TERMINATING;
    *slot = v.slot;
    *seq = v.seq;
    thread_unview(&v);
    return st;
}

/* Is the view's thread still the task its slot holds? */
static int thread_live(const thread_view_t *v) {
    return !v->ended && v->slot >= 0 && ks_seq(v->slot) == v->seq;
}

/* A new handle to the thread in `slot`: an object of its own, as NtOpenThread
 * and NtGetNextThread give, with what is known of the thread. */
static long thread_open_slot(int slot, uint64_t out, uint32_t want, uint64_t oa) {
    vibeos_file_t *f = 0;
    nt_ob_t *o;
    uint64_t h = 0;
    long r = nt_create_common(&g_nt_thread_ops, out, want, oa, 0, thread_init, 0);

    if (r != NT_STATUS(VIBEOS_STATUS_SUCCESS)) {
        return r;
    }
    if (nt_copy_in(&h, out, sizeof(h)) != 0 ||
        nt_handle_ref(h, &g_nt_thread_ops, 0u, &f, 0) != VIBEOS_STATUS_SUCCESS) {
        return NT_STATUS(VIBEOS_STATUS_ACCESS_VIOLATION);
    }
    nt_ob_lock();
    o = nt_ob_of(f);
    o->u.th.slot = slot;
    o->u.th.seq = ks_seq(slot);
    o->u.th.tid = ks_id(slot)->pid;
    o->u.th.pid = ks_id(slot)->tgid;
    o->u.th.teb = ks_gs_get(slot);
    nt_ob_unlock();
    vibeos_file_put(f);
    return NT_STATUS(VIBEOS_STATUS_SUCCESS);
}

/* A live thread of the calling process, by its id; -1 for none. */
static int thread_by_tid(uint32_t tid) {
    int me = ks_current(), slot;

    ks_lock(ks_sched_lock(), __func__);
    slot = ks_task_by_tid(tid);
    if (slot >= 0) {
        vibeos_task_state_t s = vibeos_task_state((uint32_t)slot);

        if (ks_id(slot)->tgid != ks_id(me)->tgid || !ks_id(slot)->is_user || s == VIBEOS_TASK_ZOMBIE ||
            s == VIBEOS_TASK_FREE) {
            slot = -1;
        }
    }
    ks_unlock(ks_sched_lock());
    return slot;
}

/* NtOpenThread(ThreadHandle, DesiredAccess, ObjectAttributes, ClientId): a
 * thread of this process, by its id; another process's is W6's. */
static long nt_open_thread(uint64_t out, uint64_t want, uint64_t oa, uint64_t client_u) {
    uint64_t cid[2];
    int slot;

    if (client_u == 0u || nt_copy_in(cid, client_u, sizeof(cid)) != 0) {
        return NT_STATUS(VIBEOS_STATUS_ACCESS_VIOLATION);
    }
    if (cid[0] != 0u && cid[0] != ks_id(ks_current())->tgid) {
        return NT_STATUS(VIBEOS_STATUS_INVALID_CID);
    }
    slot = thread_by_tid((uint32_t)cid[1]);
    if (slot < 0) {
        return NT_STATUS(VIBEOS_STATUS_INVALID_CID);
    }
    return thread_open_slot(slot, out, (uint32_t)want, oa);
}

/* NtGetNextThread(ProcessHandle, ThreadHandle, DesiredAccess, HandleAttributes,
 * Flags, NewThreadHandle): this process's threads, in the order of their
 * slots, after the one `prev` names (0: from the first). */
static long nt_get_next_thread(uint64_t process, uint64_t prev, uint64_t want, uint64_t out) {
    thread_view_t v;
    uint32_t i, tgid, start = 0;

    if (process != VIBEOS_NT_CURRENT_PROCESS) {
        return NT_STATUS(VIBEOS_STATUS_INVALID_HANDLE);
    }
    tgid = ks_id(ks_current())->tgid;
    if (prev != 0u) {
        uint32_t st = thread_view(prev, 0u, &v);

        if (st != VIBEOS_STATUS_SUCCESS) {
            return NT_STATUS(st);
        }
        start = v.slot < 0 ? 0u : (uint32_t)v.slot + 1u;
        thread_unview(&v);
    }
    for (i = start; i < ks_slots(); i++) {
        vibeos_task_state_t s = vibeos_task_state(i);

        if (s != VIBEOS_TASK_FREE && s != VIBEOS_TASK_ZOMBIE && ks_id((int)i)->is_user &&
            ks_id((int)i)->tgid == tgid) {
            return thread_open_slot((int)i, out, (uint32_t)want, 0u);
        }
    }
    return NT_STATUS(VIBEOS_STATUS_NO_MORE_ENTRIES);
}

/* ---- what a thread says of itself ------------------------------------------------------ */

#define NT_TI_BASIC          0u
#define NT_TI_TIMES          1u
#define NT_TI_PRIORITY       2u
#define NT_TI_BASE_PRIORITY  3u
#define NT_TI_AFFINITY       4u
#define NT_TI_START_ADDRESS  9u
#define NT_TI_LAST_THREAD   12u
#define NT_TI_HIDE          17u
#define NT_TI_IS_TERMINATED 20u

static void put32_(uint8_t *b, uint32_t at, uint32_t v) {
    uint32_t i;

    for (i = 0; i < 4u; i++) {
        b[at + i] = (uint8_t)(v >> (8u * i));
    }
}

static void put64_(uint8_t *b, uint32_t at, uint64_t v) {
    put32_(b, at, (uint32_t)v);
    put32_(b, at + 4u, (uint32_t)(v >> 32));
}

/* NtQueryInformationThread(ThreadHandle, Class, Information, Length,
 * ReturnLength): basic (exit status, TEB, client id, affinity, priorities),
 * times, the start address, whether it is the last thread, whether it is
 * hidden from a debugger, whether it has ended. */
static long nt_query_information_thread(uint64_t h, uint64_t cls, uint64_t out, uint64_t len,
                                        uint64_t ret_u) {
    uint8_t b[48];
    thread_view_t v;
    uint32_t need = 0, i, st;

    switch ((uint32_t)cls) {
        case NT_TI_BASIC: need = 48u; break;
        case NT_TI_TIMES: need = 32u; break;
        case NT_TI_AFFINITY: need = 8u; break;
        case NT_TI_START_ADDRESS: need = 8u; break;
        case NT_TI_LAST_THREAD: need = 4u; break;
        case NT_TI_HIDE: need = 1u; break;
        case NT_TI_IS_TERMINATED: need = 4u; break;
        default: return NT_STATUS(VIBEOS_STATUS_INVALID_INFO_CLASS);
    }
    if ((uint32_t)len != need) {
        return NT_STATUS(VIBEOS_STATUS_INFO_LENGTH_MISMATCH);
    }
    st = thread_view(h, NT_THREAD_QUERY_INFORMATION, &v);
    if (st != VIBEOS_STATUS_SUCCESS) {
        return NT_STATUS(st);
    }
    for (i = 0; i < sizeof(b); i++) {
        b[i] = 0;
    }
    switch ((uint32_t)cls) {
        case NT_TI_BASIC:
            put32_(b, 0, v.ended ? v.exit_code : NT_STILL_ACTIVE);
            put64_(b, 8, v.teb);
            put64_(b, 16, v.pid);
            put64_(b, 24, v.tid);
            put64_(b, 32, 0xFu);   /* the four cores */
            put32_(b, 40, 8u);     /* THREAD_PRIORITY_NORMAL in a normal class */
            put32_(b, 44, 8u);
            break;
        case NT_TI_TIMES: {
            /* Creation from the boot's start in 100 ns, CPU time from the
             * account; no exit time while it runs. */
            uint64_t created = thread_live(&v) ? ks_id(v.slot)->start_tick : 0u;

            put64_(b, 0, nt_ticks_to_filetime(created));
            break;
        }
        case NT_TI_AFFINITY:
            put64_(b, 0, 0xFu);
            break;
        case NT_TI_START_ADDRESS:
            put64_(b, 0, v.start);
            break;
        case NT_TI_LAST_THREAD:
            put32_(b, 0, threads_in_process(ks_current()) <= 1u);
            break;
        case NT_TI_HIDE:
            b[0] = (uint8_t)(v.hidden != 0);
            break;
        default:
            put32_(b, 0, (uint32_t)(v.ended != 0));
            break;
    }
    thread_unview(&v);
    if (nt_copy_out(out, b, need) != 0 ||
        (ret_u != 0u && nt_copy_out(ret_u, &need, 4u) != 0)) {
        return NT_STATUS(VIBEOS_STATUS_ACCESS_VIOLATION);
    }
    return NT_STATUS(VIBEOS_STATUS_SUCCESS);
}

/* NtSetInformationThread(ThreadHandle, Class, Information, Length): hiding a
 * thread from a debugger (no length, no data), and the priorities and
 * affinity, which are taken and kept as Windows reports them back only through
 * calls W5 does not answer from the scheduler. */
static long nt_set_information_thread(uint64_t h, uint64_t cls, uint64_t in, uint64_t len) {
    thread_view_t v;
    uint32_t st;

    switch ((uint32_t)cls) {
        case NT_TI_HIDE:
            if (len != 0u) {
                return NT_STATUS(VIBEOS_STATUS_INFO_LENGTH_MISMATCH);
            }
            break;
        case NT_TI_PRIORITY:
        case NT_TI_BASE_PRIORITY:
            if (len != 4u) {
                return NT_STATUS(VIBEOS_STATUS_INFO_LENGTH_MISMATCH);
            }
            break;
        case NT_TI_AFFINITY:
            if (len != 8u) {
                return NT_STATUS(VIBEOS_STATUS_INFO_LENGTH_MISMATCH);
            }
            break;
        default:
            return NT_STATUS(VIBEOS_STATUS_INVALID_INFO_CLASS);
    }
    st = thread_view(h, NT_THREAD_SET_INFORMATION, &v);
    if (st != VIBEOS_STATUS_SUCCESS) {
        return NT_STATUS(st);
    }
    if ((uint32_t)cls == NT_TI_AFFINITY) {
        uint64_t mask = 0;

        if (nt_copy_in(&mask, in, 8u) != 0) {
            st = VIBEOS_STATUS_ACCESS_VIOLATION;
        } else if ((mask & 0xFu) == 0u) {
            st = VIBEOS_STATUS_INVALID_PARAMETER;   /* no core the machine has */
        }
    } else if ((uint32_t)cls == NT_TI_HIDE && v.f) {
        nt_ob_lock();
        nt_ob_of(v.f)->u.th.hidden = 1;
        nt_ob_unlock();
    }
    thread_unview(&v);
    return NT_STATUS(st);
}

/* NtGetCurrentProcessorNumber(): the core the caller is on. */
static long nt_get_current_processor_number(void) {
    return (long)ks_cpu_id();
}

/* ---- suspend, resume, alert ------------------------------------------------------------ */

/* NtSuspendThread(ThreadHandle, PreviousSuspendCount). The count goes up; the
 * first suspension stops the thread, by the machinery SIGSTOP uses - on its
 * way out of the kernel, at its next tick or system call. */
static long nt_suspend_thread(uint64_t h, uint64_t prev_u) {
    thread_view_t v;
    uint32_t st = thread_view(h, NT_THREAD_SUSPEND_RESUME, &v), prev = 0;

    if (st != VIBEOS_STATUS_SUCCESS) {
        return NT_STATUS(st);
    }
    ks_lock(ks_sched_lock(), __func__);
    if (!thread_live(&v)) {
        st = VIBEOS_STATUS_THREAD_IS_TERMINATING;
    } else if ((prev = ks_id(v.slot)->nt_suspend) >= NT_MAXIMUM_SUSPEND_COUNT) {
        st = VIBEOS_STATUS_SUSPEND_COUNT_EXCEEDED;
    } else {
        ks_id(v.slot)->nt_suspend = prev + 1u;
    }
    ks_unlock(ks_sched_lock());
    if (st == VIBEOS_STATUS_SUCCESS && prev == 0u &&
        vibeos_task_state((uint32_t)v.slot) != VIBEOS_TASK_SETUP) {
        (void)ks_signal_send(v.slot, VIBEOS_SIGSTOP, 0);
    }
    thread_unview(&v);
    if (st == VIBEOS_STATUS_SUCCESS && prev_u != 0u && nt_copy_out(prev_u, &prev, 4u) != 0) {
        return NT_STATUS(VIBEOS_STATUS_ACCESS_VIOLATION);
    }
    return NT_STATUS(st);
}

/* The count goes down; at zero a thread that never started is started, and
 * one that was stopped is continued. The previous count in *prev. A status. */
static uint32_t thread_resume(const thread_view_t *v, uint32_t *prev) {
    int start = 0, cont = 0;

    ks_lock(ks_sched_lock(), __func__);
    if (!thread_live(v)) {
        ks_unlock(ks_sched_lock());
        return VIBEOS_STATUS_THREAD_IS_TERMINATING;
    }
    *prev = ks_id(v->slot)->nt_suspend;
    if (*prev > 0u) {
        ks_id(v->slot)->nt_suspend = *prev - 1u;
        if (*prev == 1u) {
            start = vibeos_task_state((uint32_t)v->slot) == VIBEOS_TASK_SETUP;
            cont = !start;
        }
    }
    ks_unlock(ks_sched_lock());
    if (start) {
        (void)ks_set_state(v->slot, VIBEOS_TASK_READY, __func__);
        ks_mark_ready(v->slot, "NtResumeThread");
    } else if (cont) {
        (void)ks_signal_send(v->slot, VIBEOS_SIGCONT, 0);
    }
    return VIBEOS_STATUS_SUCCESS;
}

/* NtResumeThread(ThreadHandle, PreviousSuspendCount). */
static long nt_resume_thread(uint64_t h, uint64_t prev_u) {
    thread_view_t v;
    uint32_t st = thread_view(h, NT_THREAD_SUSPEND_RESUME, &v), prev = 0;

    if (st != VIBEOS_STATUS_SUCCESS) {
        return NT_STATUS(st);
    }
    st = thread_resume(&v, &prev);
    thread_unview(&v);
    if (st == VIBEOS_STATUS_SUCCESS && prev_u != 0u && nt_copy_out(prev_u, &prev, 4u) != 0) {
        return NT_STATUS(VIBEOS_STATUS_ACCESS_VIOLATION);
    }
    return NT_STATUS(st);
}

/* NtAlertThread(ThreadHandle): the thread is alerted; its next alertable wait
 * ends at once with STATUS_ALERTED (step 4). */
static long nt_alert_thread(uint64_t h) {
    thread_view_t v;
    uint32_t st = thread_view(h, NT_THREAD_ALERT, &v);

    if (st != VIBEOS_STATUS_SUCCESS) {
        return NT_STATUS(st);
    }
    if (thread_live(&v)) {
        __atomic_store_n(&ks_id(v.slot)->nt_alerted, 1u, __ATOMIC_RELEASE);
    }
    thread_unview(&v);
    return NT_STATUS(VIBEOS_STATUS_SUCCESS);
}

/* NtAlertResumeThread(ThreadHandle, PreviousSuspendCount): both. */
static long nt_alert_resume_thread(uint64_t h, uint64_t prev_u) {
    thread_view_t v;
    uint32_t st = thread_view(h, NT_THREAD_SUSPEND_RESUME, &v), prev = 0;

    if (st != VIBEOS_STATUS_SUCCESS) {
        return NT_STATUS(st);
    }
    if (thread_live(&v)) {
        __atomic_store_n(&ks_id(v.slot)->nt_alerted, 1u, __ATOMIC_RELEASE);
    }
    st = thread_resume(&v, &prev);
    thread_unview(&v);
    if (st == VIBEOS_STATUS_SUCCESS && prev_u != 0u && nt_copy_out(prev_u, &prev, 4u) != 0) {
        return NT_STATUS(VIBEOS_STATUS_ACCESS_VIOLATION);
    }
    return NT_STATUS(st);
}

/* ---- a thread's registers ----------------------------------------------------------------
 *
 * CONTEXT for the calling thread, from the registers it entered the call with.
 * Another thread's saved registers are on its kernel stack where the switch
 * left them, and reaching them is a ks_ service of its own: NOT_IMPLEMENTED
 * until then, said rather than invented. */

#define NT_CONTEXT_CONTROL  0x00100001u
#define NT_CONTEXT_INTEGER  0x00100002u

static const struct {
    uint32_t at;
    uint32_t reg;   /* index into vibeos_uregs_t, as uint64_t */
} g_ctx_int[] = {
    { 0x78u, 13u },  /* rax */ { 0x80u, 14u },  /* rcx */ { 0x88u, 12u },  /* rdx */
    { 0x90u, 11u },  /* rbx */ { 0xA0u, 10u },  /* rbp */ { 0xA8u, 9u },   /* rsi */
    { 0xB0u, 8u },   /* rdi */ { 0xB8u, 0u },   /* r8  */ { 0xC0u, 1u },   /* r9  */
    { 0xC8u, 2u },   /* r10 */ { 0xD0u, 3u },   /* r11 */ { 0xD8u, 4u },   /* r12 */
    { 0xE0u, 5u },   /* r13 */ { 0xE8u, 6u },   /* r14 */ { 0xF0u, 7u },   /* r15 */
};

/* The registers into a CONTEXT's integer and control fields, as `flags` asks. */
void nt_context_from_regs(const vibeos_uregs_t *r, uint8_t *ctx, uint32_t flags) {
    const uint64_t *g = (const uint64_t *)(const void *)r;
    uint32_t i;

    if ((flags & NT_CONTEXT_INTEGER) == NT_CONTEXT_INTEGER) {
        for (i = 0; i < sizeof(g_ctx_int) / sizeof(g_ctx_int[0]); i++) {
            put64_(ctx, g_ctx_int[i].at, g[g_ctx_int[i].reg]);
        }
    }
    if ((flags & NT_CONTEXT_CONTROL) == NT_CONTEXT_CONTROL) {
        ctx[0x38] = (uint8_t)r->cs;
        ctx[0x39] = (uint8_t)(r->cs >> 8);
        ctx[0x42] = (uint8_t)r->ss;
        ctx[0x43] = (uint8_t)(r->ss >> 8);
        put32_(ctx, 0x44u, (uint32_t)r->rflags);
        put64_(ctx, 0x98u, r->rsp);
        put64_(ctx, 0xF8u, r->rip);
    }
}

/* And back: what the CONTEXT says, for the parts `flags` names, over `r`. */
void nt_context_to_regs(const uint8_t *ctx, vibeos_uregs_t *r, uint32_t flags) {
    uint64_t *g = (uint64_t *)(void *)r;
    uint32_t i, k;

    if ((flags & NT_CONTEXT_INTEGER) == NT_CONTEXT_INTEGER) {
        for (i = 0; i < sizeof(g_ctx_int) / sizeof(g_ctx_int[0]); i++) {
            uint64_t v = 0;

            for (k = 0; k < 8u; k++) {
                v |= (uint64_t)ctx[g_ctx_int[i].at + k] << (8u * k);
            }
            g[g_ctx_int[i].reg] = v;
        }
    }
    if ((flags & NT_CONTEXT_CONTROL) == NT_CONTEXT_CONTROL) {
        uint64_t sp = 0, ip = 0, fl = 0;

        for (k = 0; k < 8u; k++) {
            sp |= (uint64_t)ctx[0x98u + k] << (8u * k);
            ip |= (uint64_t)ctx[0xF8u + k] << (8u * k);
        }
        for (k = 0; k < 4u; k++) {
            fl |= (uint64_t)ctx[0x44u + k] << (8u * k);
        }
        r->rsp = sp;
        r->rip = ip;
        r->rflags = fl;   /* ks_regs_set forces the privileged bits */
    }
}

/* NtGetContextThread(ThreadHandle, Context). */
static long nt_get_context_thread(ks_regs_t *frame, uint64_t h, uint64_t ctx_u) {
    uint8_t ctx[0x100];
    vibeos_uregs_t r;
    uint32_t flags = 0, i;

    if (nt_copy_in(&flags, ctx_u + 0x30u, 4u) != 0) {
        return NT_STATUS(VIBEOS_STATUS_ACCESS_VIOLATION);
    }
    if (h != VIBEOS_NT_CURRENT_THREAD) {
        return NT_STATUS(VIBEOS_STATUS_NOT_IMPLEMENTED);   /* another thread's: see above */
    }
    if (nt_copy_in(ctx, ctx_u, sizeof(ctx)) != 0) {
        return NT_STATUS(VIBEOS_STATUS_ACCESS_VIOLATION);
    }
    for (i = 0x48u; i < 0x78u; i++) {
        ctx[i] = 0;   /* no debug registers */
    }
    ks_regs_get(frame, &r);
    nt_context_from_regs(&r, ctx, flags);
    return NT_STATUS(nt_copy_out(ctx_u, ctx, sizeof(ctx)) != 0 ? VIBEOS_STATUS_ACCESS_VIOLATION
                                                               : VIBEOS_STATUS_SUCCESS);
}

/* NtSetContextThread(ThreadHandle, Context): the calling thread resumes from
 * the context - as NtContinue does - for the parts its flags name. */
static long nt_set_context_thread(ks_regs_t *frame, uint64_t h, uint64_t ctx_u) {
    uint8_t ctx[0x100];
    vibeos_uregs_t r;
    uint32_t flags;

    if (h != VIBEOS_NT_CURRENT_THREAD) {
        return NT_STATUS(VIBEOS_STATUS_NOT_IMPLEMENTED);
    }
    if (nt_copy_in(ctx, ctx_u, sizeof(ctx)) != 0) {
        return NT_STATUS(VIBEOS_STATUS_ACCESS_VIOLATION);
    }
    flags = (uint32_t)ctx[0x30] | (uint32_t)ctx[0x31] << 8 | (uint32_t)ctx[0x32] << 16 |
            (uint32_t)ctx[0x33] << 24;
    ks_regs_get(frame, &r);
    nt_context_to_regs(ctx, &r, flags);
    if (ks_regs_set(frame, &r) != 0) {
        return NT_STATUS(VIBEOS_STATUS_INVALID_PARAMETER);
    }
    return (long)r.rax;   /* the frame is the context now, rax included */
}

/* The end of another thread of this process: its handles told the status first,
 * then the thread ended alone, by the path a sibling's exec ends one by. */
static uint32_t thread_terminate_other(uint64_t h, uint32_t status) {
    thread_view_t v;
    thread_exit_t e;
    uint32_t st = thread_view(h, NT_THREAD_TERMINATE, &v);

    if (st != VIBEOS_STATUS_SUCCESS) {
        return st;
    }
    if (thread_live(&v)) {
        e.slot = v.slot;
        e.seq = v.seq;
        e.code = status;
        nt_ob_lock();
        nt_ob_each(&g_nt_thread_ops, thread_mark, &e);
        nt_ob_unlock();
        ks_id(v.slot)->exec_killed = 1;
        ks_id(v.slot)->nt_suspend = 0;
        if (vibeos_task_state((uint32_t)v.slot) == VIBEOS_TASK_SETUP) {
            (void)ks_set_state(v.slot, VIBEOS_TASK_READY, __func__);   /* to take the kill */
        }
        (void)ks_signal_send(v.slot, VIBEOS_SIGKILL, 0);
    }
    thread_unview(&v);
    return VIBEOS_STATUS_SUCCESS;
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
            return NT_STATUS(thread_terminate_other(h, (uint32_t)status));
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
      nt_terminate_thread(ARG(0), NT_ARG32(1))) \
    X(VIBEOS_NT_NR_NtOpenThread, NtOpenThread, GETPID, PTRS(OUT(0, 8), IN(3, 16)), \
      nt_open_thread(ARG(0), NT_ARG32(1), ARG(2), ARG(3))) \
    X(VIBEOS_NT_NR_NtGetNextThread, NtGetNextThread, GETPID, PTRS(OUT(5, 8)), \
      nt_get_next_thread(ARG(0), ARG(1), NT_ARG32(2), ARG(5))) \
    X(VIBEOS_NT_NR_NtQueryInformationThread, NtQueryInformationThread, GETPID, \
      PTRS(NT_OUT_BUF(2, 3), OUT_OPT(4, 4)), \
      nt_query_information_thread(ARG(0), NT_ARG32(1), ARG(2), NT_ARG32(3), ARG(4))) \
    X(VIBEOS_NT_NR_NtSetInformationThread, NtSetInformationThread, GETPID, PTRS(NT_IN_BUF(2, 3)), \
      nt_set_information_thread(ARG(0), NT_ARG32(1), ARG(2), NT_ARG32(3))) \
    X(VIBEOS_NT_NR_NtGetCurrentProcessorNumber, NtGetCurrentProcessorNumber, GETPID, NOPTR, \
      nt_get_current_processor_number()) \
    X(VIBEOS_NT_NR_NtSuspendThread, NtSuspendThread, KILL, PTRS(OUT_OPT(1, 4)), \
      nt_suspend_thread(ARG(0), ARG(1))) \
    X(VIBEOS_NT_NR_NtResumeThread, NtResumeThread, KILL, PTRS(OUT_OPT(1, 4)), \
      nt_resume_thread(ARG(0), ARG(1))) \
    X(VIBEOS_NT_NR_NtAlertThread, NtAlertThread, KILL, NOPTR, nt_alert_thread(ARG(0))) \
    X(VIBEOS_NT_NR_NtAlertResumeThread, NtAlertResumeThread, KILL, PTRS(OUT_OPT(1, 4)), \
      nt_alert_resume_thread(ARG(0), ARG(1))) \
    X(VIBEOS_NT_NR_NtGetContextThread, NtGetContextThread, GETPID, PTRS(IN(1, 0x100)), \
      nt_get_context_thread(FRAME, ARG(0), ARG(1))) \
    X(VIBEOS_NT_NR_NtSetContextThread, NtSetContextThread, GETPID, PTRS(IN(1, 0x100)), \
      nt_set_context_thread(FRAME, ARG(0), ARG(1)))

NT_DEFINE_SERVICES(thread, NT_THREAD_SERVICES)
