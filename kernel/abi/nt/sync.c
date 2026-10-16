/* NT personality: events and the waits (docs/abi/windows.md W2).
 *
 * An event is the simplest object that can be waited on, which is why it comes
 * with the object manager's core: it is how the core is tested. Mutants and
 * semaphores follow with the same wait.
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
    if (vibeos_uaccess_copy((void *)(uintptr_t)uptr, &h, sizeof(h)) != 0) {
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
        st = nt_ob_create(ops, name.len ? &name : 0, attributes, &f, &existed);
        done = st;
        if (st == VIBEOS_STATUS_OBJECT_NAME_EXISTS) {
            st = VIBEOS_STATUS_SUCCESS;
        }
    }
    if (st != VIBEOS_STATUS_SUCCESS) {
        return NT_STATUS(st);
    }
    if (!existed && !open_only && init) {
        nt_ob_lock();
        init(nt_ob_of(f), ctx);
        nt_ob_unlock();
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
        vibeos_uaccess_copy((void *)(uintptr_t)prev_u, &prev, sizeof(prev)) != 0) {
        return NT_STATUS(VIBEOS_STATUS_ACCESS_VIOLATION);
    }
    return NT_STATUS(VIBEOS_STATUS_SUCCESS);
}

static long nt_set_event(uint64_t h, uint64_t prev) { return event_change(h, prev, EV_SET); }
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
    if (vibeos_uaccess_copy((void *)(uintptr_t)buf, &info, sizeof(info)) != 0 ||
        (ret != 0u && vibeos_uaccess_copy((void *)(uintptr_t)ret, &got, sizeof(got)) != 0)) {
        return NT_STATUS(VIBEOS_STATUS_ACCESS_VIOLATION);
    }
    return NT_STATUS(VIBEOS_STATUS_SUCCESS);
}

/* ---- the waits ---------------------------------------------------------------------- */

#define VIBEOS_STATUS_INVALID_PARAMETER_1   0xC00000EFu
#define VIBEOS_STATUS_INVALID_PARAMETER_3   0xC00000F1u
#define VIBEOS_STATUS_INVALID_PARAMETER_MIX 0xC0000030u

/* A timeout as a tick to give up at: none (0 returned in *forever), a relative
 * one (negative, in 100 ns) from now, or an absolute system time. */
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

long nt_wait(uint32_t n, const uint64_t *handles, int all, uint64_t timeout_u) {
    vibeos_file_t *f[VIBEOS_NT_MAX_WAIT_OBJECTS];
    nt_ob_t *o[VIBEOS_NT_MAX_WAIT_OBJECTS];
    uint32_t pulses[VIBEOS_NT_MAX_WAIT_OBJECTS];
    uint32_t i, got = 0, st = VIBEOS_STATUS_SUCCESS;
    int me = ks_current();
    uint32_t tid = me >= 0 ? ks_id(me)->pid : 0u;
    int64_t t = 0;
    int forever;
    uint64_t deadline;
    long result;

    if (timeout_u != 0u &&
        vibeos_uaccess_copy(&t, (const void *)(uintptr_t)timeout_u, sizeof(t)) != 0) {
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
        if (!forever && ks_ticks() >= deadline) {
            result = NT_STATUS(VIBEOS_STATUS_TIMEOUT);
            break;
        }
        /* A signal that needs acting on ends the wait - a kill must be able
         * to end a process parked here. What Windows has instead, an APC,
         * is W5's; the status is the one an alertable wait ends with. */
        if (me >= 0 && ks_signal_interrupts(me)) {
            result = NT_STATUS(VIBEOS_STATUS_USER_APC);
            break;
        }
        ks_wait_tick();
    }
    if (result == NT_STATUS(VIBEOS_STATUS_TIMEOUT) || result == NT_STATUS(VIBEOS_STATUS_USER_APC)) {
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

/* NtWaitForSingleObject(Handle, Alertable, Timeout). Alertable waits wait the
 * same: there are no APCs until W5. */
static long nt_wait_single(uint64_t h, uint64_t alertable, uint64_t timeout) {
    (void)alertable;
    return nt_wait(1u, &h, 0, timeout);
}

/* NtWaitForMultipleObjects(Count, Handles, WaitType, Alertable, Timeout):
 * WaitType 0 is WaitAll, 1 WaitAny. */
static long nt_wait_multiple(uint64_t count, uint64_t handles_u, uint64_t type,
                             uint64_t alertable, uint64_t timeout) {
    uint64_t handles[VIBEOS_NT_MAX_WAIT_OBJECTS];
    uint32_t n = (uint32_t)count;

    (void)alertable;
    if (n == 0u || n > VIBEOS_NT_MAX_WAIT_OBJECTS) {
        return NT_STATUS(VIBEOS_STATUS_INVALID_PARAMETER_1);
    }
    if ((uint32_t)type > 1u) {
        return NT_STATUS(VIBEOS_STATUS_INVALID_PARAMETER_3);
    }
    /* Judged here, not by the row: the length is the count's, in handles. */
    if (!nt_user_ok(handles_u, (uint64_t)n * 8u, 0) ||
        vibeos_uaccess_copy(handles, (const void *)(uintptr_t)handles_u, (uint64_t)n * 8u) != 0) {
        return NT_STATUS(VIBEOS_STATUS_ACCESS_VIOLATION);
    }
    return nt_wait(n, handles, (uint32_t)type == 0u, timeout);
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
    X(VIBEOS_NT_NR_NtWaitForSingleObject, NtWaitForSingleObject, FUTEX, PTRS(IN_OPT(2, 8)), \
      nt_wait_single(ARG(0), ARG(1), ARG(2))) \
    X(VIBEOS_NT_NR_NtWaitForMultipleObjects, NtWaitForMultipleObjects, FUTEX, \
      PTRS(IN_OPT(4, 8)), \
      nt_wait_multiple(NT_ARG32(0), ARG(1), NT_ARG32(2), ARG(3), ARG(4)))

NT_DEFINE_SERVICES(sync, NT_SYNC_SERVICES)
