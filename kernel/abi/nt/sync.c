/* NT personality: events, mutants, semaphores and the waits (docs/abi/windows.md W2).
 *
 * An event is the simplest object that can be waited on, which is why it comes
 * with the object manager's core: it is how the core is tested. Mutants and
 * semaphores share its wait.
 *
 * The wait looks at its objects once a tick, as the Linux layer's waits do;
 * an object that becomes signalled is seen within a tick. What it cannot see
 * that way is an NtPulseEvent, which signals and resets in one call - so an
 * event counts its pulses, and a waiter that was already waiting when one
 * happened is satisfied by it, as Windows satisfies the waiters present at the
 * pulse. */

#include "nt_internal.h"

/* ---- the event type ----------------------------------------------------------------- */

static int event_signaled(nt_ob_t *o, uint32_t tid) {
    (void)tid;
    return o->u.ev.state != 0;
}

static uint32_t event_acquire(nt_ob_t *o, uint32_t tid) {
    (void)tid;
    if (o->u.ev.kind == VIBEOS_NT_SYNCHRONIZATION_EVENT) {
        o->u.ev.state = 0;   /* auto-reset: one waiter takes it */
    }
    return 0;
}

static const nt_type_t g_event_type = {
    "Event",
    VIBEOS_EVENT_ALL_ACCESS,
    VIBEOS_READ_CONTROL | VIBEOS_EVENT_QUERY_STATE,
    VIBEOS_READ_CONTROL | VIBEOS_EVENT_MODIFY_STATE,
    VIBEOS_READ_CONTROL | VIBEOS_SYNCHRONIZE,
    event_signaled,
    event_acquire,
};

const vibeos_file_ops_t g_nt_event_ops = {
    .name = "nt-event",
    .release = nt_ob_release,
    .nt_type = &g_event_type,
};

static long put_handle(uint64_t uptr, uint64_t h) {
    if (nt_copy_out(uptr, &h, sizeof(h)) != 0) {
        return -1;
    }
    return 0;
}

/* Create or open, and hand out a handle: the part every NtCreate and NtOpen of
 * a type shares. `init` sets a new object's state and is not called for an
 * existing one, whose state is its own. */
long nt_create_common(const vibeos_file_ops_t *ops, uint64_t handle_u, uint32_t want,
                      uint64_t oa, int open_only,
                      void (*init)(nt_ob_t *o, const void *ctx), const void *ctx) {
    nt_name_t name;
    uint32_t attributes = 0, st, done;
    vibeos_file_t *f = 0;
    uint64_t h = 0;
    int existed = 0;

    st = nt_read_attributes(oa, &name, &attributes);
    if (st != VIBEOS_STATUS_SUCCESS) {
        return NT_STATUS(st);
    }
    if (open_only) {
        st = nt_ob_open(ops, &name, &f);
        done = st;
    } else {
        st = nt_ob_create(ops, name.len ? &name : 0, attributes, &f, &existed, init, ctx);
        done = st;
        if (st == VIBEOS_STATUS_OBJECT_NAME_EXISTS) {
            st = VIBEOS_STATUS_SUCCESS;
        }
    }
    if (st != VIBEOS_STATUS_SUCCESS) {
        return NT_STATUS(st);
    }
    st = nt_handle_install(f, nt_map_access(ops, want), attributes, &h);
    if (st != VIBEOS_STATUS_SUCCESS) {
        return NT_STATUS(st);
    }
    if (put_handle(handle_u, h) != 0) {
        /* Nowhere to say which handle: take it back rather than leak it. */
        (void)nt_close_handle(h);
        return NT_STATUS(VIBEOS_STATUS_ACCESS_VIOLATION);
    }
    return NT_STATUS(done);
}

static void event_init(nt_ob_t *o, const void *ctx) {
    const uint64_t *args = (const uint64_t *)ctx;
    o->u.ev.kind = (uint32_t)args[0];
    o->u.ev.state = args[1] ? 1 : 0;
}

/* NtCreateEvent(EventHandle, DesiredAccess, ObjectAttributes, EventType,
 * InitialState). A name that exists opens that event - state untouched - and
 * says OBJECT_NAME_EXISTS, a success. */
static long nt_create_event(uint64_t out, uint64_t want, uint64_t oa, uint64_t type,
                            uint64_t initial) {
    uint64_t args[2];

    if ((uint32_t)type > VIBEOS_NT_SYNCHRONIZATION_EVENT) {
        return NT_STATUS(VIBEOS_STATUS_INVALID_PARAMETER);
    }
    args[0] = (uint32_t)type;
    args[1] = (uint8_t)initial;   /* a BOOLEAN: the low byte is the argument */
    return nt_create_common(&g_nt_event_ops, out, (uint32_t)want, oa, 0, event_init, args);
}

static long nt_open_event(uint64_t out, uint64_t want, uint64_t oa) {
    return nt_create_common(&g_nt_event_ops, out, (uint32_t)want, oa, 1, 0, 0);
}

/* What the set, reset, clear and pulse calls share: the event, with
 * EVENT_MODIFY_STATE, its state changed under the lock and the previous one
 * reported if asked. */
enum { EV_SET, EV_RESET, EV_PULSE };

static long event_change(uint64_t h, uint64_t prev_u, int how) {
    vibeos_file_t *f;
    nt_ob_t *o;
    int32_t prev;
    uint32_t st = nt_handle_ref(h, &g_nt_event_ops, VIBEOS_EVENT_MODIFY_STATE, &f, 0);

    if (st != VIBEOS_STATUS_SUCCESS) {
        return NT_STATUS(st);
    }
    nt_ob_lock();
    o = nt_ob_of(f);
    prev = o->u.ev.state;
    switch (how) {
        case EV_SET:
            o->u.ev.state = 1;
            break;
        case EV_RESET:
            o->u.ev.state = 0;
            break;
        default:
            /* Every waiter present now is released and the event is left
             * reset: a notification event's waiters see the count move; a
             * synchronization event releases one, by being set for it. */
            if (o->u.ev.kind == VIBEOS_NT_NOTIFICATION_EVENT) {
                o->u.ev.pulses++;
                o->u.ev.state = 0;
            } else {
                o->u.ev.state = o->waiters ? 1 : 0;
            }
            break;
    }
    nt_ob_unlock();
    vibeos_file_put(f);
    if (prev_u != 0u &&
        nt_copy_out(prev_u, &prev, sizeof(prev)) != 0) {
        return NT_STATUS(VIBEOS_STATUS_ACCESS_VIOLATION);
    }
    return NT_STATUS(VIBEOS_STATUS_SUCCESS);
}

static long nt_set_event(uint64_t h, uint64_t prev) { return event_change(h, prev, EV_SET); }

/* For the file services (W4): the event an I/O was given is reset when it
 * starts and set when it completes, as Windows does. A status, 0 for success. */
uint32_t nt_io_event(uint64_t h, int set) {
    return (uint32_t)event_change(h, 0, set ? EV_SET : EV_RESET);   /* NT_STATUS: the status itself */
}
static long nt_reset_event(uint64_t h, uint64_t prev) { return event_change(h, prev, EV_RESET); }
static long nt_clear_event(uint64_t h) { return event_change(h, 0, EV_RESET); }
static long nt_pulse_event(uint64_t h, uint64_t prev) { return event_change(h, prev, EV_PULSE); }

/* NtQueryEvent(Event, Class, Buffer, Length, ReturnLength): EventBasicInformation. */
static long nt_query_event(uint64_t h, uint64_t cls, uint64_t buf, uint64_t len, uint64_t ret) {
    vibeos_file_t *f;
    vibeos_nt_event_basic_t info;
    uint32_t got = (uint32_t)sizeof(info), st;
    nt_ob_t *o;

    if ((uint32_t)cls != 0u) {
        return NT_STATUS(VIBEOS_STATUS_INVALID_INFO_CLASS);
    }
    if ((uint32_t)len != sizeof(info)) {
        return NT_STATUS(VIBEOS_STATUS_INFO_LENGTH_MISMATCH);
    }
    st = nt_handle_ref(h, &g_nt_event_ops, VIBEOS_EVENT_QUERY_STATE, &f, 0);
    if (st != VIBEOS_STATUS_SUCCESS) {
        return NT_STATUS(st);
    }
    nt_ob_lock();
    o = nt_ob_of(f);
    info.event_type = o->u.ev.kind;
    info.event_state = o->u.ev.state;
    nt_ob_unlock();
    vibeos_file_put(f);
    if (nt_copy_out(buf, &info, sizeof(info)) != 0 ||
        (ret != 0u && nt_copy_out(ret, &got, sizeof(got)) != 0)) {
        return NT_STATUS(VIBEOS_STATUS_ACCESS_VIOLATION);
    }
    return NT_STATUS(VIBEOS_STATUS_SUCCESS);
}

/* ---- mutants ----------------------------------------------------------------------------
 *
 * A mutex in NT's word: owned by a thread, taken again by the same thread as
 * many times as it likes, released as many times. "Abandoned" - its owner
 * ended holding it - needs the exit path to say so, which is W5's with
 * threads; the state and the status it gives a waiter are here already. */


static int mutant_signaled(nt_ob_t *o, uint32_t tid) {
    return o->u.mu.owner == 0u || o->u.mu.owner == tid;
}

static uint32_t mutant_acquire(nt_ob_t *o, uint32_t tid) {
    uint32_t abandoned = o->u.mu.abandoned;

    o->u.mu.owner = tid;
    o->u.mu.count++;
    o->u.mu.abandoned = 0;
    return abandoned;
}

static const nt_type_t g_mutant_type = {
    "Mutant",
    VIBEOS_MUTANT_ALL_ACCESS,
    VIBEOS_READ_CONTROL | VIBEOS_MUTANT_QUERY_STATE,
    VIBEOS_READ_CONTROL,
    VIBEOS_READ_CONTROL | VIBEOS_SYNCHRONIZE,
    mutant_signaled,
    mutant_acquire,
};

const vibeos_file_ops_t g_nt_mutant_ops = {
    .name = "nt-mutant",
    .release = nt_ob_release,
    .nt_type = &g_mutant_type,
};

static void mutant_init(nt_ob_t *o, const void *ctx) {
    const uint32_t *owner = (const uint32_t *)ctx;   /* the creator, or 0 */
    o->u.mu.owner = *owner;
    o->u.mu.count = *owner ? 1u : 0u;
}

static uint32_t my_tid(void) {
    int me = ks_current();
    return me >= 0 ? ks_id(me)->pid : 0u;
}

/* NtCreateMutant(MutantHandle, DesiredAccess, ObjectAttributes, InitialOwner). */
static long nt_create_mutant(uint64_t out, uint64_t want, uint64_t oa, uint64_t initial) {
    uint32_t owner = (uint8_t)initial ? my_tid() : 0u;
    return nt_create_common(&g_nt_mutant_ops, out, (uint32_t)want, oa, 0, mutant_init, &owner);
}

static long nt_open_mutant(uint64_t out, uint64_t want, uint64_t oa) {
    return nt_create_common(&g_nt_mutant_ops, out, (uint32_t)want, oa, 1, 0, 0);
}

/* NtReleaseMutant(Mutant, PreviousCount): only by its owner. The count it
 * reports is the mutant's signal state before the release, as Windows keeps
 * it - 1 free, 0 held once, -1 held twice - not the recursion. */
static long nt_release_mutant(uint64_t h, uint64_t prev_u) {
    vibeos_file_t *f;
    nt_ob_t *o;
    int32_t prev;
    uint32_t st = nt_handle_ref(h, &g_nt_mutant_ops, 0, &f, 0);

    if (st != VIBEOS_STATUS_SUCCESS) {
        return NT_STATUS(st);
    }
    nt_ob_lock();
    o = nt_ob_of(f);
    if (o->u.mu.owner != my_tid() || o->u.mu.count == 0u) {
        nt_ob_unlock();
        vibeos_file_put(f);
        return NT_STATUS(VIBEOS_STATUS_MUTANT_NOT_OWNED);
    }
    prev = 1 - (int32_t)o->u.mu.count;
    if (--o->u.mu.count == 0u) {
        o->u.mu.owner = 0;
    }
    nt_ob_unlock();
    vibeos_file_put(f);
    if (prev_u != 0u && nt_copy_out(prev_u, &prev, sizeof(prev)) != 0) {
        return NT_STATUS(VIBEOS_STATUS_ACCESS_VIOLATION);
    }
    return NT_STATUS(VIBEOS_STATUS_SUCCESS);
}

/* NtQueryMutant(Mutant, Class, Buffer, Length, ReturnLength):
 * MutantBasicInformation - the signal state, whether the caller owns it,
 * whether it was abandoned. */
static long nt_query_mutant(uint64_t h, uint64_t cls, uint64_t buf, uint64_t len, uint64_t ret) {
    vibeos_file_t *f;
    nt_ob_t *o;
    vibeos_nt_mutant_basic_t info = {0};
    uint32_t got = (uint32_t)sizeof(info), st;

    if ((uint32_t)cls != 0u) {
        return NT_STATUS(VIBEOS_STATUS_INVALID_INFO_CLASS);
    }
    if ((uint32_t)len != sizeof(info)) {
        return NT_STATUS(VIBEOS_STATUS_INFO_LENGTH_MISMATCH);
    }
    st = nt_handle_ref(h, &g_nt_mutant_ops, VIBEOS_MUTANT_QUERY_STATE, &f, 0);
    if (st != VIBEOS_STATUS_SUCCESS) {
        return NT_STATUS(st);
    }
    nt_ob_lock();
    o = nt_ob_of(f);
    info.current_count = 1 - (int32_t)o->u.mu.count;
    info.owned_by_caller = o->u.mu.owner != 0u && o->u.mu.owner == my_tid();
    info.abandoned_state = o->u.mu.abandoned ? 1u : 0u;
    nt_ob_unlock();
    vibeos_file_put(f);
    if (nt_copy_out(buf, &info, sizeof(info)) != 0 ||
        (ret != 0u && nt_copy_out(ret, &got, sizeof(got)) != 0)) {
        return NT_STATUS(VIBEOS_STATUS_ACCESS_VIOLATION);
    }
    return NT_STATUS(VIBEOS_STATUS_SUCCESS);
}

/* ---- semaphores ------------------------------------------------------------------------- */


static int semaphore_signaled(nt_ob_t *o, uint32_t tid) {
    (void)tid;
    return o->u.sem.count > 0;
}

static uint32_t semaphore_acquire(nt_ob_t *o, uint32_t tid) {
    (void)tid;
    o->u.sem.count--;
    return 0;
}

static const nt_type_t g_semaphore_type = {
    "Semaphore",
    VIBEOS_SEMAPHORE_ALL_ACCESS,
    VIBEOS_READ_CONTROL | VIBEOS_SEMAPHORE_QUERY_STATE,
    VIBEOS_READ_CONTROL | VIBEOS_SEMAPHORE_MODIFY_STATE,
    VIBEOS_READ_CONTROL | VIBEOS_SYNCHRONIZE,
    semaphore_signaled,
    semaphore_acquire,
};

const vibeos_file_ops_t g_nt_semaphore_ops = {
    .name = "nt-semaphore",
    .release = nt_ob_release,
    .nt_type = &g_semaphore_type,
};

static void semaphore_init(nt_ob_t *o, const void *ctx) {
    const int32_t *v = (const int32_t *)ctx;
    o->u.sem.count = v[0];
    o->u.sem.max = v[1];
}

/* NtCreateSemaphore(SemaphoreHandle, DesiredAccess, ObjectAttributes,
 * InitialCount, MaximumCount): a maximum of at least one, an initial count
 * between zero and it. */
static long nt_create_semaphore(uint64_t out, uint64_t want, uint64_t oa, uint64_t initial,
                                uint64_t maximum) {
    int32_t v[2];

    v[0] = (int32_t)(uint32_t)initial;
    v[1] = (int32_t)(uint32_t)maximum;
    if (v[1] <= 0 || v[0] < 0 || v[0] > v[1]) {
        return NT_STATUS(VIBEOS_STATUS_INVALID_PARAMETER);
    }
    return nt_create_common(&g_nt_semaphore_ops, out, (uint32_t)want, oa, 0, semaphore_init, v);
}

static long nt_open_semaphore(uint64_t out, uint64_t want, uint64_t oa) {
    return nt_create_common(&g_nt_semaphore_ops, out, (uint32_t)want, oa, 1, 0, 0);
}

/* NtReleaseSemaphore(Semaphore, ReleaseCount, PreviousCount): past the maximum
 * is refused and changes nothing. */
static long nt_release_semaphore(uint64_t h, uint64_t count, uint64_t prev_u) {
    vibeos_file_t *f;
    nt_ob_t *o;
    int32_t n = (int32_t)(uint32_t)count, prev;
    uint32_t st;

    if (n <= 0) {
        return NT_STATUS(VIBEOS_STATUS_INVALID_PARAMETER);
    }
    st = nt_handle_ref(h, &g_nt_semaphore_ops, VIBEOS_SEMAPHORE_MODIFY_STATE, &f, 0);
    if (st != VIBEOS_STATUS_SUCCESS) {
        return NT_STATUS(st);
    }
    nt_ob_lock();
    o = nt_ob_of(f);
    prev = o->u.sem.count;
    if (n > o->u.sem.max - o->u.sem.count) {
        nt_ob_unlock();
        vibeos_file_put(f);
        return NT_STATUS(VIBEOS_STATUS_SEMAPHORE_LIMIT_EXCEEDED);
    }
    o->u.sem.count += n;
    nt_ob_unlock();
    vibeos_file_put(f);
    if (prev_u != 0u && nt_copy_out(prev_u, &prev, sizeof(prev)) != 0) {
        return NT_STATUS(VIBEOS_STATUS_ACCESS_VIOLATION);
    }
    return NT_STATUS(VIBEOS_STATUS_SUCCESS);
}

/* NtQuerySemaphore(Semaphore, Class, Buffer, Length, ReturnLength):
 * SemaphoreBasicInformation - the count and the maximum. */
static long nt_query_semaphore(uint64_t h, uint64_t cls, uint64_t buf, uint64_t len,
                               uint64_t ret) {
    vibeos_file_t *f;
    nt_ob_t *o;
    vibeos_nt_semaphore_basic_t info;
    uint32_t got = (uint32_t)sizeof(info), st;

    if ((uint32_t)cls != 0u) {
        return NT_STATUS(VIBEOS_STATUS_INVALID_INFO_CLASS);
    }
    if ((uint32_t)len != sizeof(info)) {
        return NT_STATUS(VIBEOS_STATUS_INFO_LENGTH_MISMATCH);
    }
    st = nt_handle_ref(h, &g_nt_semaphore_ops, VIBEOS_SEMAPHORE_QUERY_STATE, &f, 0);
    if (st != VIBEOS_STATUS_SUCCESS) {
        return NT_STATUS(st);
    }
    nt_ob_lock();
    o = nt_ob_of(f);
    info.current_count = o->u.sem.count;
    info.maximum_count = o->u.sem.max;
    nt_ob_unlock();
    vibeos_file_put(f);
    if (nt_copy_out(buf, &info, sizeof(info)) != 0 ||
        (ret != 0u && nt_copy_out(ret, &got, sizeof(got)) != 0)) {
        return NT_STATUS(VIBEOS_STATUS_ACCESS_VIOLATION);
    }
    return NT_STATUS(VIBEOS_STATUS_SUCCESS);
}

/* ---- the waits ---------------------------------------------------------------------- */

#define VIBEOS_STATUS_INVALID_PARAMETER_3   0xC00000F1u
#define VIBEOS_STATUS_INVALID_PARAMETER_MIX 0xC0000030u

/* A timeout as a tick to give up at: none (0 returned in *forever), a relative
 * one (negative, in 100 ns) from now, or an absolute system time. */
/* Must a wait end, for a signal? Not for the stop a suspension sends, or the
 * continue that ends it: a suspended thread's wait goes on, and the thread
 * stops on its way out of the kernel (W5 step 3). Taken for an interruption,
 * a wait ended STATUS_USER_APC the moment its thread was suspended. */
static int nt_interrupted(int me) {
    const uint64_t quiet = (1ull << VIBEOS_SIGSTOP) | (1ull << VIBEOS_SIGCONT);

    return me >= 0 && ks_signal_interrupts(me) &&
           (__atomic_load_n(&ks_id(me)->sig_pending, __ATOMIC_ACQUIRE) & ~quiet) != 0u;
}

static uint64_t wait_deadline(int64_t t, int *forever_out, int forever) {
    const uint64_t per = VIBEOS_NT_PERF_FREQUENCY / (uint64_t)ks_hz();
    uint64_t now = ks_ticks();

    *forever_out = forever;
    if (forever) {
        return 0;
    }
    if (t < 0) {
        uint64_t rel = (uint64_t)(-(t + 1)) + 1u;
        return now + (rel + per - 1u) / per;
    }
    {
        uint64_t sys = VIBEOS_NT_EPOCH_DELTA + now * per;
        return (uint64_t)t <= sys ? now : now + ((uint64_t)t - sys + per - 1u) / per;
    }
}

/* Is object i signalled for this waiter: its type says so, or - for a
 * notification event - a pulse happened while it waited. */
static int signaled_now(nt_ob_t *o, uint32_t tid, uint32_t pulses_at_start) {
    if (o->file->ops->nt_type->signaled(o, tid)) {
        return 1;
    }
    return o->file->ops == &g_nt_event_ops && o->u.ev.kind == VIBEOS_NT_NOTIFICATION_EVENT &&
           o->u.ev.pulses != pulses_at_start;
}

long nt_wait(uint32_t n, const uint64_t *handles, int all, int alertable, uint64_t timeout_u) {
    vibeos_file_t *f[VIBEOS_NT_MAX_WAIT_OBJECTS];
    nt_ob_t *o[VIBEOS_NT_MAX_WAIT_OBJECTS];
    uint32_t pulses[VIBEOS_NT_MAX_WAIT_OBJECTS];
    uint32_t i, got = 0, st = VIBEOS_STATUS_SUCCESS;
    int me = ks_current();
    uint32_t tid = my_tid();
    int64_t t = 0;
    int forever;
    uint64_t deadline;
    long result;

    if (timeout_u != 0u &&
        nt_copy_in(&t, timeout_u, sizeof(t)) != 0) {
        return NT_STATUS(VIBEOS_STATUS_ACCESS_VIOLATION);
    }
    for (i = 0; i < n; i++) {
        st = nt_handle_ref(handles[i], 0, VIBEOS_SYNCHRONIZE, &f[i], 0);
        if (st == VIBEOS_STATUS_SUCCESS && (!f[i]->ops->nt_type || !f[i]->ops->nt_type->signaled)) {
            vibeos_file_put(f[i]);
            st = VIBEOS_STATUS_OBJECT_TYPE_MISMATCH;
        }
        if (st != VIBEOS_STATUS_SUCCESS) {
            break;
        }
        got++;
    }
    if (st == VIBEOS_STATUS_SUCCESS && all) {
        uint32_t j;
        /* WaitAll naming one object twice is a request no single acquisition
         * can answer: refused, as Windows refuses it. */
        for (i = 0; i < n && st == VIBEOS_STATUS_SUCCESS; i++) {
            for (j = i + 1u; j < n; j++) {
                if (f[i] == f[j]) {
                    st = VIBEOS_STATUS_INVALID_PARAMETER_MIX;
                    break;
                }
            }
        }
    }
    if (st != VIBEOS_STATUS_SUCCESS) {
        for (i = 0; i < got; i++) {
            vibeos_file_put(f[i]);
        }
        return NT_STATUS(st);
    }
    deadline = wait_deadline(t, &forever, timeout_u == 0u);

    nt_ob_lock();
    for (i = 0; i < n; i++) {
        o[i] = nt_ob_of(f[i]);
        o[i]->waiters++;
        pulses[i] = o[i]->u.ev.pulses;
    }
    nt_ob_unlock();

    for (;;) {
        int done = 0;

        nt_ob_lock();
        if (all) {
            for (i = 0; i < n && signaled_now(o[i], tid, pulses[i]); i++) {
            }
            if (i == n) {
                uint32_t abandoned = 0;
                for (i = 0; i < n; i++) {
                    abandoned |= o[i]->file->ops->nt_type->acquire(o[i], tid);
                }
                result = NT_STATUS(abandoned ? VIBEOS_STATUS_ABANDONED_WAIT_0
                                             : VIBEOS_STATUS_WAIT_0);
                done = 1;
            }
        } else {
            for (i = 0; i < n; i++) {
                if (signaled_now(o[i], tid, pulses[i])) {
                    uint32_t ab = o[i]->file->ops->nt_type->acquire(o[i], tid);
                    result = NT_STATUS((ab ? VIBEOS_STATUS_ABANDONED_WAIT_0
                                           : VIBEOS_STATUS_WAIT_0) + i);
                    done = 1;
                    break;
                }
            }
        }
        if (done) {
            for (i = 0; i < n; i++) {
                o[i]->waiters--;
            }
            nt_ob_unlock();
            break;
        }
        nt_ob_unlock();
        /* An alertable wait ends for an APC or the alert, after the objects
         * were asked and before the time: Windows' order. */
        if (alertable && (st = nt_alertable_end(me)) != 0u) {
            result = NT_STATUS(st);
            break;
        }
        if (!forever && ks_ticks() >= deadline) {
            result = NT_STATUS(VIBEOS_STATUS_TIMEOUT);
            break;
        }
        /* A signal that needs acting on ends the wait - a kill must be able
         * to end a process parked here. What Windows has instead, an APC,
         * is W5's; the status is the one an alertable wait ends with. */
        if (nt_interrupted(me)) {
            result = NT_STATUS(VIBEOS_STATUS_USER_APC);
            break;
        }
        ks_wait_tick();
    }
    if (result == NT_STATUS(VIBEOS_STATUS_TIMEOUT) || result == NT_STATUS(VIBEOS_STATUS_USER_APC) ||
        result == NT_STATUS(VIBEOS_STATUS_ALERTED)) {
        nt_ob_lock();
        for (i = 0; i < n; i++) {
            o[i]->waiters--;
        }
        nt_ob_unlock();
    }
    for (i = 0; i < n; i++) {
        vibeos_file_put(f[i]);
    }
    return result;
}

/* NtDelayExecution(Alertable, DelayInterval): Sleep. The interval is an NT
 * time - relative when negative - and zero gives the processor up for a tick,
 * as a wait does. Like the waits, a signal that needs acting on ends it early;
 * there are no APCs until W5. */
static long nt_delay_execution(uint64_t alertable, uint64_t interval_u) {
    int64_t t;
    int forever;
    uint64_t deadline;
    int me = ks_current();

    if (nt_copy_in(&t, interval_u, sizeof(t)) != 0) {
        return NT_STATUS(VIBEOS_STATUS_ACCESS_VIOLATION);
    }
    deadline = wait_deadline(t, &forever, 0);
    do {
        uint32_t why;

        if (alertable && me >= 0 && (why = nt_alertable_end(me)) != 0u) {
            return NT_STATUS(why);
        }
        if (nt_interrupted(me)) {
            return NT_STATUS(VIBEOS_STATUS_USER_APC);
        }
        ks_wait_tick();
    } while (ks_ticks() < deadline);
    return NT_STATUS(VIBEOS_STATUS_SUCCESS);
}

/* NtWaitForSingleObject(Handle, Alertable, Timeout). */
static long nt_wait_single(uint64_t h, uint64_t alertable, uint64_t timeout) {
    return nt_wait(1u, &h, 0, (int)(alertable & 0xFFu), timeout);
}

/* NtWaitForMultipleObjects(Count, Handles, WaitType, Alertable, Timeout):
 * WaitType 0 is WaitAll, 1 WaitAny. */
static long nt_wait_multiple(uint64_t count, uint64_t handles_u, uint64_t type,
                             uint64_t alertable, uint64_t timeout) {
    uint64_t handles[VIBEOS_NT_MAX_WAIT_OBJECTS];
    uint32_t n = (uint32_t)count;

    if (n == 0u || n > VIBEOS_NT_MAX_WAIT_OBJECTS) {
        return NT_STATUS(VIBEOS_STATUS_INVALID_PARAMETER_1);
    }
    if ((uint32_t)type > 1u) {
        return NT_STATUS(VIBEOS_STATUS_INVALID_PARAMETER_3);
    }
    /* Judged here, not by the row: the length is the count's, in handles. */
    if (!nt_user_ok(handles_u, (uint64_t)n * 8u, 0) ||
        nt_copy_in(handles, handles_u, (uint64_t)n * 8u) != 0) {
        return NT_STATUS(VIBEOS_STATUS_ACCESS_VIOLATION);
    }
    return nt_wait(n, handles, (uint32_t)type == 0u, (int)(alertable & 0xFFu), timeout);
}

/* ---- timers (W5 step 7) ------------------------------------------------------------------
 *
 * A waitable timer is signalled once its due time has passed; a notification
 * timer stays so until it is set again, a synchronization timer is taken by
 * the first waiter. A period sets it due again that many milliseconds on.
 * Times are the waits' - the tick - and the state is brought up to date
 * whenever somebody looks, so no interrupt has to touch it. */

#define NT_TIMER_ALL_ACCESS   0x001F0003u
#define NT_TIMER_QUERY_STATE  0x0001u
#define NT_TIMER_MODIFY_STATE 0x0002u

static void timer_update(nt_ob_t *o) {
    uint64_t now = ks_ticks();

    if (o->u.tm.armed && now >= o->u.tm.due) {
        o->u.tm.state = 1;
        if (o->u.tm.period != 0u) {
            while (o->u.tm.due <= now) {
                o->u.tm.due += o->u.tm.period;
            }
        } else {
            o->u.tm.armed = 0;
        }
    }
}

static int timer_signaled(nt_ob_t *o, uint32_t tid) {
    (void)tid;
    timer_update(o);
    return o->u.tm.state != 0;
}

static uint32_t timer_acquire(nt_ob_t *o, uint32_t tid) {
    (void)tid;
    if (o->u.tm.kind == 1u) {
        o->u.tm.state = 0;   /* a synchronization timer: one waiter takes it */
    }
    return 0;
}

static const nt_type_t g_timer_type = {
    "Timer",
    NT_TIMER_ALL_ACCESS,
    VIBEOS_READ_CONTROL | NT_TIMER_QUERY_STATE,
    VIBEOS_READ_CONTROL | NT_TIMER_MODIFY_STATE,
    VIBEOS_READ_CONTROL | VIBEOS_SYNCHRONIZE,
    timer_signaled,
    timer_acquire,
};

const vibeos_file_ops_t g_nt_timer_ops = {
    .name = "nt-timer",
    .release = nt_ob_release,
    .nt_type = &g_timer_type,
};

static void timer_init(nt_ob_t *o, const void *ctx) {
    o->u.tm.kind = (uint32_t)*(const uint64_t *)ctx;
    o->u.tm.armed = 0;
    o->u.tm.state = 0;
    o->u.tm.due = 0;
    o->u.tm.period = 0;
}

/* NtCreateTimer(TimerHandle, DesiredAccess, ObjectAttributes, TimerType). */
static long nt_create_timer(uint64_t out, uint64_t want, uint64_t oa, uint64_t type) {
    if ((uint32_t)type > 1u) {
        return NT_STATUS(VIBEOS_STATUS_INVALID_PARAMETER_4);
    }
    return nt_create_common(&g_nt_timer_ops, out, (uint32_t)want, oa, 0, timer_init, &type);
}

static long nt_open_timer(uint64_t out, uint64_t want, uint64_t oa) {
    return nt_create_common(&g_nt_timer_ops, out, (uint32_t)want, oa, 1, 0, 0);
}

/* NtSetTimer(TimerHandle, DueTime, TimerApcRoutine, TimerContext, ResumeTimer,
 * Period, PreviousState): due at the NT time given - relative when negative -
 * and every `Period` milliseconds after. The previous state is whether it was
 * signalled. A completion routine is an APC, queued when the timer fires: W5
 * step 4's. */
static long nt_set_timer(uint64_t h, uint64_t due_u, uint64_t apc, uint64_t period, uint64_t prev_u) {
    vibeos_file_t *f = 0;
    nt_ob_t *o;
    int64_t t = 0;
    int forever;
    uint32_t st, prev;

    if (apc != 0u) {
        return NT_STATUS(VIBEOS_STATUS_NOT_IMPLEMENTED);
    }
    if ((int32_t)(uint32_t)period < 0) {
        return NT_STATUS(VIBEOS_STATUS_INVALID_PARAMETER_6);
    }
    if (nt_copy_in(&t, due_u, sizeof(t)) != 0) {
        return NT_STATUS(VIBEOS_STATUS_ACCESS_VIOLATION);
    }
    st = nt_handle_ref(h, &g_nt_timer_ops, NT_TIMER_MODIFY_STATE, &f, 0);
    if (st != VIBEOS_STATUS_SUCCESS) {
        return NT_STATUS(st);
    }
    nt_ob_lock();
    o = nt_ob_of(f);
    timer_update(o);
    prev = o->u.tm.state;
    o->u.tm.state = 0;
    o->u.tm.due = wait_deadline(t, &forever, 0);
    o->u.tm.period = ((uint64_t)(uint32_t)period * (uint64_t)ks_hz() + 999u) / 1000u;
    if (period != 0u && o->u.tm.period == 0u) {
        o->u.tm.period = 1;
    }
    o->u.tm.armed = 1;
    nt_ob_unlock();
    vibeos_file_put(f);
    if (prev_u != 0u && nt_copy_out(prev_u, &prev, 1u) != 0) {
        return NT_STATUS(VIBEOS_STATUS_ACCESS_VIOLATION);
    }
    return NT_STATUS(VIBEOS_STATUS_SUCCESS);
}

/* NtCancelTimer(TimerHandle, CurrentState). It stops; it stays signalled if it was. */
static long nt_cancel_timer(uint64_t h, uint64_t cur_u) {
    vibeos_file_t *f = 0;
    nt_ob_t *o;
    uint32_t st = nt_handle_ref(h, &g_nt_timer_ops, NT_TIMER_MODIFY_STATE, &f, 0), cur;

    if (st != VIBEOS_STATUS_SUCCESS) {
        return NT_STATUS(st);
    }
    nt_ob_lock();
    o = nt_ob_of(f);
    timer_update(o);
    cur = o->u.tm.state;
    o->u.tm.armed = 0;
    nt_ob_unlock();
    vibeos_file_put(f);
    if (cur_u != 0u && nt_copy_out(cur_u, &cur, 1u) != 0) {
        return NT_STATUS(VIBEOS_STATUS_ACCESS_VIOLATION);
    }
    return NT_STATUS(VIBEOS_STATUS_SUCCESS);
}

/* NtQueryTimer(TimerHandle, TimerBasicInformation 0, Information, Length,
 * ReturnLength): the time left, in 100 ns, and whether it is signalled. */
static long nt_query_timer(uint64_t h, uint64_t cls, uint64_t out, uint64_t len, uint64_t ret_u) {
    vibeos_file_t *f = 0;
    nt_ob_t *o;
    uint8_t b[16];
    uint64_t left = 0;
    uint32_t st, i, n = 12u;

    if ((uint32_t)cls != 0u) {
        return NT_STATUS(VIBEOS_STATUS_INVALID_INFO_CLASS);
    }
    if ((uint32_t)len != 12u) {
        return NT_STATUS(VIBEOS_STATUS_INFO_LENGTH_MISMATCH);
    }
    st = nt_handle_ref(h, &g_nt_timer_ops, NT_TIMER_QUERY_STATE, &f, 0);
    if (st != VIBEOS_STATUS_SUCCESS) {
        return NT_STATUS(st);
    }
    for (i = 0; i < sizeof(b); i++) {
        b[i] = 0;
    }
    nt_ob_lock();
    o = nt_ob_of(f);
    timer_update(o);
    if (o->u.tm.armed && o->u.tm.due > ks_ticks()) {
        left = (o->u.tm.due - ks_ticks()) * (VIBEOS_NT_PERF_FREQUENCY / (uint64_t)ks_hz());
    }
    b[8] = (uint8_t)(o->u.tm.state != 0u);
    nt_ob_unlock();
    vibeos_file_put(f);
    for (i = 0; i < 8u; i++) {
        b[i] = (uint8_t)(left >> (8u * i));
    }
    if (nt_copy_out(out, b, 12u) != 0 || (ret_u != 0u && nt_copy_out(ret_u, &n, 4u) != 0)) {
        return NT_STATUS(VIBEOS_STATUS_ACCESS_VIOLATION);
    }
    return NT_STATUS(VIBEOS_STATUS_SUCCESS);
}

/* NtQueryTimerResolution(Maximum, Minimum, Current) and NtSetTimerResolution:
 * the tick, in 100 ns, is all three; a request to change it is answered with
 * what it is. */
static long nt_query_timer_resolution(uint64_t max_u, uint64_t min_u, uint64_t cur_u) {
    uint32_t r = (uint32_t)(VIBEOS_NT_PERF_FREQUENCY / (uint64_t)ks_hz());

    if (nt_copy_out(max_u, &r, 4u) != 0 || nt_copy_out(min_u, &r, 4u) != 0 ||
        nt_copy_out(cur_u, &r, 4u) != 0) {
        return NT_STATUS(VIBEOS_STATUS_ACCESS_VIOLATION);
    }
    return NT_STATUS(VIBEOS_STATUS_SUCCESS);
}

static long nt_set_timer_resolution(uint64_t cur_u) {
    uint32_t r = (uint32_t)(VIBEOS_NT_PERF_FREQUENCY / (uint64_t)ks_hz());

    return NT_STATUS(nt_copy_out(cur_u, &r, 4u) != 0 ? VIBEOS_STATUS_ACCESS_VIOLATION
                                                     : VIBEOS_STATUS_TIMER_RESOLUTION_NOT_SET);
}

/* ---- keyed events (W5 step 7) -----------------------------------------------------------
 *
 * A rendezvous on a key, which is any pointer-sized value: a release is taken
 * by one waiter on its key, and either side waits for the other. The handle
 * may be 0, Windows' process-wide keyed event, which here is one static
 * object. Not signalled in the waits' sense: nobody NtWaitForSingleObject's
 * one. */

#define NT_KEYED_EVENT_ALL_ACCESS 0x000F0003u
#define NT_KEYED_EVENT_WAIT       0x0001u
#define NT_KEYED_EVENT_WAKE       0x0002u

static const nt_type_t g_keyed_type = {
    "KeyedEvent",
    NT_KEYED_EVENT_ALL_ACCESS,
    VIBEOS_READ_CONTROL | NT_KEYED_EVENT_WAIT,
    VIBEOS_READ_CONTROL | NT_KEYED_EVENT_WAKE,
    VIBEOS_READ_CONTROL,
    0,
    0,
};

const vibeos_file_ops_t g_nt_keyed_ops = {
    .name = "nt-keyed-event",
    .release = nt_ob_release,
    .nt_type = &g_keyed_type,
};

static nt_ob_t g_keyed_default;   /* the handle-0 keyed event, under the object lock */

static void keyed_init(nt_ob_t *o, const void *ctx) {
    uint32_t i;

    (void)ctx;
    for (i = 0; i < NT_KEYED_SLOTS; i++) {
        o->u.ke.key[i] = 0;
        o->u.ke.waiting[i] = 0;
        o->u.ke.released[i] = 0;
    }
}

static long nt_create_keyed_event(uint64_t out, uint64_t want, uint64_t oa, uint64_t flags) {
    if (flags != 0u) {
        return NT_STATUS(VIBEOS_STATUS_INVALID_PARAMETER);
    }
    return nt_create_common(&g_nt_keyed_ops, out, (uint32_t)want, oa, 0, keyed_init, 0);
}

static long nt_open_keyed_event(uint64_t out, uint64_t want, uint64_t oa) {
    return nt_create_common(&g_nt_keyed_ops, out, (uint32_t)want, oa, 1, 0, 0);
}

/* The slot for `key`, made if `make`; -1 when there is none, or no room. */
static int keyed_slot(nt_ob_t *o, uint64_t key, int make) {
    uint32_t i;
    int free_at = -1;

    for (i = 0; i < NT_KEYED_SLOTS; i++) {
        if ((o->u.ke.waiting[i] != 0u || o->u.ke.released[i] != 0u) && o->u.ke.key[i] == key) {
            return (int)i;
        }
        if (free_at < 0 && o->u.ke.waiting[i] == 0u && o->u.ke.released[i] == 0u) {
            free_at = (int)i;
        }
    }
    if (make && free_at >= 0) {
        o->u.ke.key[free_at] = key;
    }
    return make ? free_at : -1;
}

/* Wait (release = 0) for a release of `key`, or release (1) to a waiter on
 * it, until the deadline. A status. */
static long keyed_meet(uint64_t h, uint64_t key, int release, int alertable, uint64_t timeout_u) {
    vibeos_file_t *f = 0;
    nt_ob_t *o;
    int64_t t = 0;
    int forever, me = ks_current(), slot;
    uint64_t deadline;
    uint32_t st = VIBEOS_STATUS_SUCCESS;
    long result = NT_STATUS(VIBEOS_STATUS_SUCCESS);

    if (key & 1u) {
        return NT_STATUS(VIBEOS_STATUS_INVALID_PARAMETER_1);   /* Windows' rule: an even key */
    }
    if (timeout_u != 0u && nt_copy_in(&t, timeout_u, sizeof(t)) != 0) {
        return NT_STATUS(VIBEOS_STATUS_ACCESS_VIOLATION);
    }
    if (h != 0u) {
        st = nt_handle_ref(h, &g_nt_keyed_ops, release ? NT_KEYED_EVENT_WAKE : NT_KEYED_EVENT_WAIT, &f, 0);
        if (st != VIBEOS_STATUS_SUCCESS) {
            return NT_STATUS(st);
        }
    }
    deadline = wait_deadline(t, &forever, timeout_u == 0u);
    nt_ob_lock();
    o = f ? nt_ob_of(f) : &g_keyed_default;
    slot = keyed_slot(o, key, 1);
    if (slot < 0) {
        nt_ob_unlock();
        if (f) {
            vibeos_file_put(f);
        }
        return NT_STATUS(VIBEOS_STATUS_INSUFFICIENT_RESOURCES);
    }
    if (release) {
        o->u.ke.released[slot]++;
    } else {
        o->u.ke.waiting[slot]++;
    }
    nt_ob_unlock();
    for (;;) {
        int done = 0;

        nt_ob_lock();
        if (release) {
            /* Taken when a waiter has consumed it. */
            done = o->u.ke.released[slot] == 0u || o->u.ke.key[slot] != key;
        } else if (o->u.ke.released[slot] != 0u) {
            o->u.ke.released[slot]--;
            o->u.ke.waiting[slot]--;
            done = 1;
        }
        nt_ob_unlock();
        if (done) {
            break;
        }
        if (!forever && ks_ticks() >= deadline) {
            nt_ob_lock();
            if (release) {
                o->u.ke.released[slot]--;
            } else {
                o->u.ke.waiting[slot]--;
            }
            nt_ob_unlock();
            result = NT_STATUS(VIBEOS_STATUS_TIMEOUT);
            break;
        }
        if ((alertable && nt_alertable_end(me) != 0u) || nt_interrupted(me)) {
            nt_ob_lock();
            if (release) {
                o->u.ke.released[slot]--;
            } else {
                o->u.ke.waiting[slot]--;
            }
            nt_ob_unlock();
            result = NT_STATUS(VIBEOS_STATUS_USER_APC);
            break;
        }
        ks_wait_tick();
    }
    if (f) {
        vibeos_file_put(f);
    }
    return result;
}

/* NtWaitForKeyedEvent / NtReleaseKeyedEvent(KeyedEventHandle, Key, Alertable,
 * Timeout). */
static long nt_wait_keyed(uint64_t h, uint64_t key, uint64_t alertable, uint64_t timeout_u) {
    return keyed_meet(h, key, 0, (int)(alertable & 0xFFu), timeout_u);
}

static long nt_release_keyed(uint64_t h, uint64_t key, uint64_t alertable, uint64_t timeout_u) {
    return keyed_meet(h, key, 1, (int)(alertable & 0xFFu), timeout_u);
}

#define NT_SYNC_SERVICES(X) \
    X(VIBEOS_NT_NR_NtCreateEvent, NtCreateEvent, EVENTFD, PTRS(OUT(0, 8)), \
      nt_create_event(ARG(0), NT_ARG32(1), ARG(2), NT_ARG32(3), ARG(4))) \
    X(VIBEOS_NT_NR_NtOpenEvent, NtOpenEvent, EVENTFD, PTRS(OUT(0, 8)), \
      nt_open_event(ARG(0), NT_ARG32(1), ARG(2))) \
    X(VIBEOS_NT_NR_NtSetEvent, NtSetEvent, EVENTFD, PTRS(OUT_OPT(1, 4)), \
      nt_set_event(ARG(0), ARG(1))) \
    X(VIBEOS_NT_NR_NtResetEvent, NtResetEvent, EVENTFD, PTRS(OUT_OPT(1, 4)), \
      nt_reset_event(ARG(0), ARG(1))) \
    X(VIBEOS_NT_NR_NtClearEvent, NtClearEvent, EVENTFD, NOPTR, nt_clear_event(ARG(0))) \
    X(VIBEOS_NT_NR_NtPulseEvent, NtPulseEvent, EVENTFD, PTRS(OUT_OPT(1, 4)), \
      nt_pulse_event(ARG(0), ARG(1))) \
    X(VIBEOS_NT_NR_NtQueryEvent, NtQueryEvent, EVENTFD, \
      PTRS(NT_OUT_BUF(2, 3), OUT_OPT(4, 4)), \
      nt_query_event(ARG(0), NT_ARG32(1), ARG(2), NT_ARG32(3), ARG(4))) \
    X(VIBEOS_NT_NR_NtCreateMutant, NtCreateMutant, EVENTFD, PTRS(OUT(0, 8)), \
      nt_create_mutant(ARG(0), NT_ARG32(1), ARG(2), ARG(3))) \
    X(VIBEOS_NT_NR_NtOpenMutant, NtOpenMutant, EVENTFD, PTRS(OUT(0, 8)), \
      nt_open_mutant(ARG(0), NT_ARG32(1), ARG(2))) \
    X(VIBEOS_NT_NR_NtReleaseMutant, NtReleaseMutant, EVENTFD, PTRS(OUT_OPT(1, 4)), \
      nt_release_mutant(ARG(0), ARG(1))) \
    X(VIBEOS_NT_NR_NtQueryMutant, NtQueryMutant, EVENTFD, \
      PTRS(NT_OUT_BUF(2, 3), OUT_OPT(4, 4)), \
      nt_query_mutant(ARG(0), NT_ARG32(1), ARG(2), NT_ARG32(3), ARG(4))) \
    X(VIBEOS_NT_NR_NtCreateSemaphore, NtCreateSemaphore, EVENTFD, PTRS(OUT(0, 8)), \
      nt_create_semaphore(ARG(0), NT_ARG32(1), ARG(2), NT_ARG32(3), NT_ARG32(4))) \
    X(VIBEOS_NT_NR_NtOpenSemaphore, NtOpenSemaphore, EVENTFD, PTRS(OUT(0, 8)), \
      nt_open_semaphore(ARG(0), NT_ARG32(1), ARG(2))) \
    X(VIBEOS_NT_NR_NtReleaseSemaphore, NtReleaseSemaphore, EVENTFD, PTRS(OUT_OPT(2, 4)), \
      nt_release_semaphore(ARG(0), NT_ARG32(1), ARG(2))) \
    X(VIBEOS_NT_NR_NtQuerySemaphore, NtQuerySemaphore, EVENTFD, \
      PTRS(NT_OUT_BUF(2, 3), OUT_OPT(4, 4)), \
      nt_query_semaphore(ARG(0), NT_ARG32(1), ARG(2), NT_ARG32(3), ARG(4))) \
    X(VIBEOS_NT_NR_NtDelayExecution, NtDelayExecution, NANOSLEEP, PTRS(IN(1, 8)), \
      nt_apc_after(FRAME, nt_delay_execution(ARG(0) & 0xFFu, ARG(1)))) \
    X(VIBEOS_NT_NR_NtWaitForSingleObject, NtWaitForSingleObject, FUTEX, PTRS(IN_OPT(2, 8)), \
      nt_apc_after(FRAME, nt_wait_single(ARG(0), ARG(1), ARG(2)))) \
    X(VIBEOS_NT_NR_NtWaitForMultipleObjects, NtWaitForMultipleObjects, FUTEX, \
      PTRS(IN_OPT(4, 8)), \
      nt_apc_after(FRAME, nt_wait_multiple(NT_ARG32(0), ARG(1), NT_ARG32(2), ARG(3), ARG(4)))) \
    X(VIBEOS_NT_NR_NtCreateTimer, NtCreateTimer, EVENTFD, PTRS(OUT(0, 8)), \
      nt_create_timer(ARG(0), NT_ARG32(1), ARG(2), NT_ARG32(3))) \
    X(VIBEOS_NT_NR_NtOpenTimer, NtOpenTimer, EVENTFD, PTRS(OUT(0, 8)), \
      nt_open_timer(ARG(0), NT_ARG32(1), ARG(2))) \
    X(VIBEOS_NT_NR_NtSetTimer, NtSetTimer, EVENTFD, PTRS(IN(1, 8)), \
      nt_set_timer(ARG(0), ARG(1), ARG(2), NT_ARG32(5), ARG(6))) \
    X(VIBEOS_NT_NR_NtCancelTimer, NtCancelTimer, EVENTFD, PTRS(OUT_OPT(1, 1)), \
      nt_cancel_timer(ARG(0), ARG(1))) \
    X(VIBEOS_NT_NR_NtQueryTimer, NtQueryTimer, EVENTFD, PTRS(NT_OUT_BUF(2, 3), OUT_OPT(4, 4)), \
      nt_query_timer(ARG(0), NT_ARG32(1), ARG(2), NT_ARG32(3), ARG(4))) \
    X(VIBEOS_NT_NR_NtQueryTimerResolution, NtQueryTimerResolution, EVENTFD, \
      PTRS(OUT(0, 4), OUT(1, 4), OUT(2, 4)), \
      nt_query_timer_resolution(ARG(0), ARG(1), ARG(2))) \
    X(VIBEOS_NT_NR_NtSetTimerResolution, NtSetTimerResolution, EVENTFD, PTRS(OUT(2, 4)), \
      nt_set_timer_resolution(ARG(2))) \
    X(VIBEOS_NT_NR_NtCreateKeyedEvent, NtCreateKeyedEvent, FUTEX, PTRS(OUT(0, 8)), \
      nt_create_keyed_event(ARG(0), NT_ARG32(1), ARG(2), NT_ARG32(3))) \
    X(VIBEOS_NT_NR_NtOpenKeyedEvent, NtOpenKeyedEvent, FUTEX, PTRS(OUT(0, 8)), \
      nt_open_keyed_event(ARG(0), NT_ARG32(1), ARG(2))) \
    X(VIBEOS_NT_NR_NtWaitForKeyedEvent, NtWaitForKeyedEvent, FUTEX, PTRS(IN_OPT(3, 8)), \
      nt_apc_after(FRAME, nt_wait_keyed(ARG(0), ARG(1), ARG(2), ARG(3)))) \
    X(VIBEOS_NT_NR_NtReleaseKeyedEvent, NtReleaseKeyedEvent, FUTEX, PTRS(IN_OPT(3, 8)), \
      nt_apc_after(FRAME, nt_release_keyed(ARG(0), ARG(1), ARG(2), ARG(3))))

NT_DEFINE_SERVICES(sync, NT_SYNC_SERVICES)
