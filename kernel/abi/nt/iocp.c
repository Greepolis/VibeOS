/* SPDX-License-Identifier: GPL-3.0-or-later */
/* NT personality: I/O completion ports (docs/shell/s7.md step 3).
 *
 * A port is a queue of completions - a key, a context, a status and a count,
 * which is all Windows' FILE_IO_COMPLETION_INFORMATION carries - that threads
 * post to and remove from. Rust's asynchronous runtimes on Windows are built
 * on one: the driver thread parks in NtRemoveIoCompletionEx and every waker
 * is an NtSetIoCompletion. That is the whole of what this file does; a file
 * associated with a port, whose overlapped I/O would complete into it, is not
 * yet (WO-113), and kernel32 refuses the association rather than accepting
 * one that never delivers.
 *
 * The queue lives in a table of its own, not in the object, so that the 256
 * object slots do not each carry a queue nobody uses. A remove looks once a
 * tick, as every NT wait here does, so a post is seen within a tick. */

#include "nt_internal.h"

#define NT_IOCP_MAX 16u
#define NT_IOCP_PER_PROCESS 4u
#define NT_IOCP_DEPTH 256u   /* Windows' queue is unbounded; a post past this is refused */

typedef struct {
    uint64_t key;
    uint64_t ctx;
    uint64_t info;
    uint32_t status;
    uint32_t pad;
} nt_iocp_entry_t;

typedef struct {
    uint32_t used;
    uint32_t tgid;   /* the process it counts against */
    uint32_t head;
    uint32_t count;
    nt_iocp_entry_t e[NT_IOCP_DEPTH];
} nt_iocp_t;

/* quota: NT_IOCP_PER_PROCESS ports a process, enforced in iocp_claim; a port
 * is given back when its object's last reference goes (iocp_release). */
static nt_iocp_t g_iocp[NT_IOCP_MAX];   /* under the object lock */

/* Called with the object lock held. */
static int iocp_claim(uint32_t tgid) {
    uint32_t i, mine = 0;
    int slot = -1;

    for (i = 0; i < NT_IOCP_MAX; i++) {
        if (!g_iocp[i].used && slot < 0) {
            slot = (int)i;
        }
        mine += g_iocp[i].used && g_iocp[i].tgid == tgid;
    }
    if (slot < 0 || (tgid != 0u && mine >= NT_IOCP_PER_PROCESS)) {
        return -1;
    }
    g_iocp[slot].used = 1;
    g_iocp[slot].tgid = tgid;
    g_iocp[slot].head = 0;
    g_iocp[slot].count = 0;
    return slot;
}

static nt_iocp_t *port_of(nt_ob_t *o) {
    int p = o ? o->u.io.port : -1;

    return p >= 0 && p < (int)NT_IOCP_MAX && g_iocp[p].used ? &g_iocp[p] : 0;
}

/* Waitable, as on Windows: signalled while a completion is queued. A wait
 * takes nothing; only a remove does. */
static int iocp_signaled(nt_ob_t *o, uint32_t tid) {
    nt_iocp_t *q = port_of(o);

    (void)tid;
    return q && q->count != 0u;
}

static uint32_t iocp_acquire(nt_ob_t *o, uint32_t tid) {
    (void)o;
    (void)tid;
    return 0;
}

static const nt_type_t g_iocp_type = {
    "IoCompletion",
    VIBEOS_IO_COMPLETION_ALL_ACCESS,
    VIBEOS_READ_CONTROL | VIBEOS_IO_COMPLETION_QUERY_STATE,
    VIBEOS_READ_CONTROL | VIBEOS_IO_COMPLETION_MODIFY_STATE,
    VIBEOS_READ_CONTROL | VIBEOS_SYNCHRONIZE,
    iocp_signaled,
    iocp_acquire,
};

static void iocp_release(vibeos_file_t *f) {
    nt_ob_t *o;

    nt_ob_lock();
    o = nt_ob_of(f);
    if (o && o->u.io.port >= 0 && o->u.io.port < (int)NT_IOCP_MAX) {
        g_iocp[o->u.io.port].used = 0;
        o->u.io.port = -1;
    }
    nt_ob_unlock();
    nt_ob_release(f);
}

const vibeos_file_ops_t g_nt_iocp_ops = {
    .name = "nt-iocp",
    .release = iocp_release,
    .nt_type = &g_iocp_type,
};

static void iocp_init(nt_ob_t *o, const void *ctx) {
    o->u.io.port = *(const int *)ctx;
}

void nt_iocp_reset(void) {
    uint32_t i;

    for (i = 0; i < NT_IOCP_MAX; i++) {
        g_iocp[i].used = 0;
    }
}

/* NtCreateIoCompletion(IoCompletionHandle, DesiredAccess, ObjectAttributes,
 * NumberOfConcurrentThreads). The concurrency is Windows' scheduling hint -
 * how many removers it lets run at once - and is not one here: any number of
 * threads may remove. */
static long nt_create_io_completion(uint64_t out, uint64_t want, uint64_t oa, uint64_t threads) {
    int me = ks_current(), slot;
    long st;

    (void)threads;
    nt_ob_lock();
    slot = iocp_claim(me >= 0 ? ks_id(me)->tgid : 0u);
    nt_ob_unlock();
    if (slot < 0) {
        return NT_STATUS(VIBEOS_STATUS_INSUFFICIENT_RESOURCES);
    }
    st = nt_create_common(&g_nt_iocp_ops, out, (uint32_t)want, oa, 0, iocp_init, &slot);
    if (st != NT_STATUS(VIBEOS_STATUS_SUCCESS)) {
        /* Not attached to an object - or attached to one that existed by that
         * name and kept its own: the claimed queue is nobody's. */
        nt_ob_lock();
        g_iocp[slot].used = 0;
        nt_ob_unlock();
    }
    return st;
}

static long nt_open_io_completion(uint64_t out, uint64_t want, uint64_t oa) {
    return nt_create_common(&g_nt_iocp_ops, out, (uint32_t)want, oa, 1, 0, 0);
}

/* NtSetIoCompletion(IoCompletionHandle, KeyContext, ApcContext, IoStatus,
 * IoStatusInformation): PostQueuedCompletionStatus. A full queue is
 * INSUFFICIENT_RESOURCES and changes nothing. */
static long nt_set_io_completion(uint64_t h, uint64_t key, uint64_t ctx, uint64_t status,
                                 uint64_t info) {
    vibeos_file_t *f;
    nt_iocp_t *q;
    uint32_t st = nt_handle_ref(h, &g_nt_iocp_ops, VIBEOS_IO_COMPLETION_MODIFY_STATE, &f, 0);

    if (st != VIBEOS_STATUS_SUCCESS) {
        return NT_STATUS(st);
    }
    nt_ob_lock();
    q = port_of(nt_ob_of(f));
    if (!q) {
        st = VIBEOS_STATUS_INVALID_HANDLE;
    } else if (q->count >= NT_IOCP_DEPTH) {
        st = VIBEOS_STATUS_INSUFFICIENT_RESOURCES;
    } else {
        nt_iocp_entry_t *e = &q->e[(q->head + q->count) % NT_IOCP_DEPTH];

        e->key = key;
        e->ctx = ctx;
        e->status = (uint32_t)status;
        e->info = info;
        q->count++;
    }
    nt_ob_unlock();
    vibeos_file_put(f);
    return NT_STATUS(st);
}

/* The wait both removes share: up to `max` completions into `out`, the count
 * in *got, once at least one is queued - or TIMEOUT, USER_APC or ALERTED
 * with none taken. A zero timeout asks and does not wait. */
static uint32_t iocp_take(uint64_t h, nt_iocp_entry_t *out, uint32_t max, uint32_t *got,
                          uint64_t timeout_u, int alertable) {
    vibeos_file_t *f;
    int me = ks_current();
    int64_t t = 0;
    uint64_t deadline = 0;
    int forever = timeout_u == 0u;
    uint32_t st = nt_handle_ref(h, &g_nt_iocp_ops, VIBEOS_IO_COMPLETION_MODIFY_STATE, &f, 0);

    *got = 0;
    if (st != VIBEOS_STATUS_SUCCESS) {
        return st;
    }
    if (!forever) {
        if (nt_copy_in(&t, timeout_u, sizeof(t)) != 0) {
            vibeos_file_put(f);
            return VIBEOS_STATUS_ACCESS_VIOLATION;
        }
        deadline = nt_wait_deadline(t);
    }
    for (;;) {
        nt_iocp_t *q;

        nt_ob_lock();
        q = port_of(nt_ob_of(f));
        if (!q) {
            nt_ob_unlock();
            st = VIBEOS_STATUS_INVALID_HANDLE;
            break;
        }
        while (q->count != 0u && *got < max) {
            out[(*got)++] = q->e[q->head];
            q->head = (q->head + 1u) % NT_IOCP_DEPTH;
            q->count--;
        }
        nt_ob_unlock();
        if (*got != 0u) {
            st = VIBEOS_STATUS_SUCCESS;
            break;
        }
        if (alertable && (st = nt_alertable_end(me)) != 0u) {
            break;
        }
        if (!forever && ks_ticks() >= deadline) {
            st = VIBEOS_STATUS_TIMEOUT;
            break;
        }
        if (nt_signal_ends_wait(me)) {
            st = VIBEOS_STATUS_USER_APC;
            break;
        }
        ks_wait_tick();
    }
    vibeos_file_put(f);
    return st;
}

/* NtRemoveIoCompletion(IoCompletionHandle, KeyContext, ApcContext,
 * IoStatusBlock, Timeout): GetQueuedCompletionStatus. A row declares three
 * pointers at most, so the key and the context are judged by nt_copy_out
 * here rather than by the dispatcher. */
static long nt_remove_io_completion(uint64_t h, uint64_t key_u, uint64_t ctx_u, uint64_t iosb_u,
                                    uint64_t timeout_u) {
    nt_iocp_entry_t e;
    vibeos_nt_iosb_t iosb;
    uint32_t got, st = iocp_take(h, &e, 1u, &got, timeout_u, 0);

    if (st != VIBEOS_STATUS_SUCCESS) {
        return NT_STATUS(st);
    }
    iosb.status = e.status;
    iosb.information = e.info;
    if (nt_copy_out(key_u, &e.key, 8u) != 0 || nt_copy_out(ctx_u, &e.ctx, 8u) != 0 ||
        nt_copy_out(iosb_u, &iosb, sizeof(iosb)) != 0) {
        return NT_STATUS(VIBEOS_STATUS_ACCESS_VIOLATION);   /* taken, and nowhere to say so */
    }
    return NT_STATUS(VIBEOS_STATUS_SUCCESS);
}

/* NtRemoveIoCompletionEx(IoCompletionHandle, IoCompletionInformation, Count,
 * NumEntriesRemoved, Timeout, Alertable): GetQueuedCompletionStatusEx. Each
 * entry is FILE_IO_COMPLETION_INFORMATION - key, context, status block - the
 * layout kernel32's OVERLAPPED_ENTRY shares, which is why Windows' kernel32
 * passes its caller's array straight down. */
static long nt_remove_io_completion_ex(uint64_t h, uint64_t out_u, uint64_t count,
                                       uint64_t removed_u, uint64_t timeout_u,
                                       uint64_t alertable) {
    nt_iocp_entry_t e[16];
    uint32_t got = 0, i, st;
    uint32_t max = (uint32_t)count < 16u ? (uint32_t)count : 16u;

    if (max == 0u) {
        return NT_STATUS(VIBEOS_STATUS_INVALID_PARAMETER);
    }
    st = iocp_take(h, e, max, &got, timeout_u, (int)(alertable & 0xFFu));
    if (st != VIBEOS_STATUS_SUCCESS) {
        return NT_STATUS(st);
    }
    for (i = 0; i < got; i++) {
        uint64_t rec[4];

        rec[0] = e[i].key;
        rec[1] = e[i].ctx;
        rec[2] = e[i].status;
        rec[3] = e[i].info;
        if (nt_copy_out(out_u + 32u * i, rec, sizeof(rec)) != 0) {
            return NT_STATUS(VIBEOS_STATUS_ACCESS_VIOLATION);
        }
    }
    if (nt_copy_out(removed_u, &got, sizeof(got)) != 0) {
        return NT_STATUS(VIBEOS_STATUS_ACCESS_VIOLATION);
    }
    return NT_STATUS(VIBEOS_STATUS_SUCCESS);
}

#define NT_IOCP_SERVICES(X) \
    X(VIBEOS_NT_NR_NtCreateIoCompletion, NtCreateIoCompletion, EVENTFD, PTRS(OUT(0, 8)), \
      nt_create_io_completion(ARG(0), NT_ARG32(1), ARG(2), NT_ARG32(3))) \
    X(VIBEOS_NT_NR_NtOpenIoCompletion, NtOpenIoCompletion, EVENTFD, PTRS(OUT(0, 8)), \
      nt_open_io_completion(ARG(0), NT_ARG32(1), ARG(2))) \
    X(VIBEOS_NT_NR_NtSetIoCompletion, NtSetIoCompletion, EVENTFD, NOPTR, \
      nt_set_io_completion(ARG(0), ARG(1), ARG(2), NT_ARG32(3), ARG(4))) \
    X(VIBEOS_NT_NR_NtRemoveIoCompletion, NtRemoveIoCompletion, FUTEX, \
      PTRS(OUT(3, 16), IN_OPT(4, 8)), \
      nt_apc_after(FRAME, nt_remove_io_completion(ARG(0), ARG(1), ARG(2), ARG(3), ARG(4)))) \
    X(VIBEOS_NT_NR_NtRemoveIoCompletionEx, NtRemoveIoCompletionEx, FUTEX, \
      PTRS(OUT_VEC32(1, 2, 32, 16), OUT(3, 4), IN_OPT(4, 8)), \
      nt_apc_after(FRAME, nt_remove_io_completion_ex(ARG(0), ARG(1), NT_ARG32(2), ARG(3), ARG(4), \
                                                     ARG(5))))

NT_DEFINE_SERVICES(iocp, NT_IOCP_SERVICES)
