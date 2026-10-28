/* NT personality: user APCs (docs/abi/windows.md W5 step 4).
 *
 * An APC is a call a thread makes on its own behalf when it next waits
 * alertably - or asks, with NtTestAlert. The queue is the kernel's; the call
 * is the thread's, in user mode. The kernel enters ntdll for it the way it
 * starts a thread there: at ntdll's entry, with rcx 1, the CONTEXT to go back
 * to in rdx and the APC's routine and arguments in r8. ntdll makes the call and
 * hands the context to NtContinue, which puts it back - and delivers the next
 * APC first, if there is one.
 *
 * The context is written on the thread's own stack, below the red zone the
 * Windows ABI does not have, and everything the thread reads back of it is the
 * program's: NtContinue forces what decides privilege (ks_regs_set), as Linux's
 * rt_sigreturn does. */

#include "nt_internal.h"

#define NT_APC_MAX           64u      /* queued across the machine          */
#define NT_APC_PER_THREAD    16u      /* queued for one thread              */
#define NT_APC_ENTRY_MARK    1u       /* rcx at ntdll's entry: an APC        */
#define NT_CONTEXT_FULL      0x0010000Bu   /* CONTEXT_AMD64 | CONTROL | INTEGER | FLOATING_POINT */
#define NT_CONTEXT_SIZE      0x4D0u
#define NT_THREAD_SET_CONTEXT_RIGHT 0x0010u

typedef struct {
    uint32_t used;
    int slot;
    uint32_t seq;
    uint64_t routine;
    uint64_t arg[3];
} nt_apc_t;

static nt_apc_t g_apc[NT_APC_MAX];   /* under the object lock */

/* Take the oldest APC queued for the calling thread into *out; 1 if there was
 * one. Oldest by position is good enough: the table is small and each thread's
 * entries are taken in the order they were put, as a slot is reused only when
 * free. */
static int apc_take(nt_apc_t *out) {
    int me = ks_current();
    uint32_t seq = ks_seq(me), i;
    int found = 0;

    nt_ob_lock();
    for (i = 0; i < NT_APC_MAX && !found; i++) {
        if (g_apc[i].used && g_apc[i].slot == me && g_apc[i].seq == seq) {
            *out = g_apc[i];
            g_apc[i].used = 0;
            found = 1;
        }
    }
    nt_ob_unlock();
    return found;
}

int nt_apc_pending(int slot) {
    uint32_t seq = ks_seq(slot), i;
    int any = 0;

    nt_ob_lock();
    for (i = 0; i < NT_APC_MAX && !any; i++) {
        any = g_apc[i].used && g_apc[i].slot == slot && g_apc[i].seq == seq;
    }
    nt_ob_unlock();
    return any;
}

/* Enter ntdll for one APC: the frame's current registers - as the call will
 * return them, `ret` in rax - written as a CONTEXT on the stack with the APC's
 * record below it, and the frame sent to ntdll's entry. 0, or -1 when the
 * stack could not take it (the APC is then lost, as a signal frame that does
 * not fit is). */
static int apc_enter(ks_regs_t *frame, const nt_apc_t *a, uint64_t ret) {
    uint8_t ctx[NT_CONTEXT_SIZE];
    uint64_t rec[4], sp, ctx_at, rec_at;
    vibeos_uregs_t r;
    const vibeos_image_t *img = ks_image(ks_current());
    uint32_t i;

    if (!img || img->entry == 0u) {
        return -1;
    }
    for (i = 0; i < sizeof(ctx); i++) {
        ctx[i] = 0;
    }
    ks_regs_get(frame, &r);
    r.rax = ret;
    nt_context_from_regs(&r, ctx, NT_CONTEXT_FULL);
    ctx[0x30] = (uint8_t)NT_CONTEXT_FULL;
    ctx[0x31] = (uint8_t)(NT_CONTEXT_FULL >> 8);
    ctx[0x32] = (uint8_t)(NT_CONTEXT_FULL >> 16);
    ctx[0x33] = (uint8_t)(NT_CONTEXT_FULL >> 24);
    sp = (r.rsp - NT_CONTEXT_SIZE) & ~0xFull;
    ctx_at = sp;
    rec_at = ctx_at - sizeof(rec);
    sp = (rec_at - 0x28u) & ~0xFull;   /* home space for the call, aligned as after a call */
    sp -= 8u;
    rec[0] = a->routine;
    rec[1] = a->arg[0];
    rec[2] = a->arg[1];
    rec[3] = a->arg[2];
    if (nt_copy_out(ctx_at, ctx, sizeof(ctx)) != 0 || nt_copy_out(rec_at, rec, sizeof(rec)) != 0) {
        return -1;
    }
    r.rip = img->entry;
    r.rsp = sp;
    r.rcx = NT_APC_ENTRY_MARK;
    r.rdx = ctx_at;
    r.r8 = rec_at;
    return ks_regs_set(frame, &r);
}

long nt_apc_after(ks_regs_t *frame, long result) {
    nt_apc_t a;

    if (result != NT_STATUS(VIBEOS_STATUS_USER_APC) || !apc_take(&a)) {
        return result;
    }
    if (apc_enter(frame, &a, (uint64_t)result) != 0) {
        return result;
    }
    /* The dispatcher writes the result into rax after the handler; the entry
     * reads rcx, rdx and r8, and rax goes back with the context, so the value
     * here only has to be the one the context holds. */
    return result;
}

/* NtQueueApcThread(ThreadHandle, ApcRoutine, Arg1, Arg2, Arg3). */
static long nt_queue_apc_thread(uint64_t h, uint64_t routine, uint64_t a1, uint64_t a2, uint64_t a3) {
    int slot;
    uint32_t seq, i, mine = 0, st;
    int at = -1;

    if (routine == 0u || !nt_user_ok(routine, 1u, 0)) {
        return NT_STATUS(VIBEOS_STATUS_INVALID_PARAMETER);
    }
    st = nt_thread_target(h, NT_THREAD_SET_CONTEXT_RIGHT, &slot, &seq);
    if (st != VIBEOS_STATUS_SUCCESS) {
        return NT_STATUS(st);
    }
    nt_ob_lock();
    for (i = 0; i < NT_APC_MAX; i++) {
        if (g_apc[i].used && g_apc[i].slot == slot && g_apc[i].seq == seq) {
            mine++;
        } else if (!g_apc[i].used && at < 0) {
            at = (int)i;
        }
    }
    if (at < 0 || mine >= NT_APC_PER_THREAD) {
        nt_ob_unlock();
        return NT_STATUS(VIBEOS_STATUS_NO_MEMORY);
    }
    g_apc[at].used = 1;
    g_apc[at].slot = slot;
    g_apc[at].seq = seq;
    g_apc[at].routine = routine;
    g_apc[at].arg[0] = a1;
    g_apc[at].arg[1] = a2;
    g_apc[at].arg[2] = a3;
    nt_ob_unlock();
    return NT_STATUS(VIBEOS_STATUS_SUCCESS);
}

/* NtTestAlert(): deliver the caller's APCs now, as an alertable wait would -
 * the first through the dispatcher, the rest from its NtContinue - or clear
 * the alert. STATUS_SUCCESS is what the context returns once they have run. */
static long nt_test_alert(ks_regs_t *frame) {
    nt_apc_t a;
    int me = ks_current();

    if (apc_take(&a)) {
        if (apc_enter(frame, &a, VIBEOS_STATUS_SUCCESS) == 0) {
            return NT_STATUS(VIBEOS_STATUS_SUCCESS);
        }
        return NT_STATUS(VIBEOS_STATUS_SUCCESS);
    }
    if (__atomic_exchange_n(&ks_id(me)->nt_alerted, 0u, __ATOMIC_ACQ_REL)) {
        return NT_STATUS(VIBEOS_STATUS_ALERTED);
    }
    return NT_STATUS(VIBEOS_STATUS_SUCCESS);
}

/* NtContinue(Context, TestAlert): resume from the context, for the parts its
 * flags name. With TestAlert, an APC still queued is delivered first, on top
 * of that context. The value returned is the context's rax: the frame is the
 * context, and the dispatcher writes the result into rax. */
static long nt_continue(ks_regs_t *frame, uint64_t ctx_u, uint64_t test_alert) {
    uint8_t ctx[0x100];
    vibeos_uregs_t r;
    uint32_t flags;
    nt_apc_t a;

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
    if (test_alert && apc_take(&a)) {
        (void)apc_enter(frame, &a, r.rax);
    }
    return (long)r.rax;
}

/* An alertable wait's end, for the waits in sync.c: an APC queued, or the
 * alert, ends it - USER_APC or ALERTED. 0 when neither. */
uint32_t nt_alertable_end(int me) {
    if (nt_apc_pending(me)) {
        return VIBEOS_STATUS_USER_APC;
    }
    if (__atomic_exchange_n(&ks_id(me)->nt_alerted, 0u, __ATOMIC_ACQ_REL)) {
        return VIBEOS_STATUS_ALERTED;
    }
    return 0;
}

#define NT_APC_SERVICES(X) \
    X(VIBEOS_NT_NR_NtQueueApcThread, NtQueueApcThread, KILL, NOPTR, \
      nt_queue_apc_thread(ARG(0), ARG(1), ARG(2), ARG(3), ARG(4))) \
    X(VIBEOS_NT_NR_NtTestAlert, NtTestAlert, GETPID, NOPTR, nt_test_alert(FRAME)) \
    X(VIBEOS_NT_NR_NtContinue, NtContinue, SIG_RETURN, PTRS(IN(0, 0x100)), \
      nt_continue(FRAME, ARG(0), ARG(1) & 0xFFu))

NT_DEFINE_SERVICES(apc, NT_APC_SERVICES)
