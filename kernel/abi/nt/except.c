/* NT personality: exceptions (docs/abi/windows.md W5 step 5).
 *
 * A fault in a Windows program is the program's to handle, as Windows hands it
 * one: an EXCEPTION_RECORD saying what happened and a CONTEXT saying where,
 * both on the thread's own stack, and the thread sent into ntdll - at the entry
 * every thread and APC enters by, with rcx 2, the context in rdx and the record
 * in r8. ntdll dispatches it (vectored handlers, then the image's unwind
 * tables) and resumes with NtContinue; an exception nobody handles comes back
 * through NtRaiseException as a second chance, and that ends the process with
 * the exception's code, which is what Windows does without a debugger.
 *
 * Everything written there is the program's afterwards, and what decides
 * privilege on the way back is forced by ks_regs_set (NtContinue), never read.
 * A stack that cannot take the frames is the process's end, as a signal frame
 * that does not fit is for a Linux program. */

#include "nt_internal.h"

#define NT_EXCEPTION_ENTRY_MARK      2u
#define NT_RECORD_SIZE               0x98u
#define NT_CONTEXT_SIZE              0x4D0u
#define NT_CONTEXT_FULL              0x0010000Bu
#define NT_EXCEPTION_NONCONTINUABLE  0x1u

#define NT_EXC_ACCESS_VIOLATION      0xC0000005u
#define NT_EXC_ILLEGAL_INSTRUCTION   0xC000001Du
#define NT_EXC_INT_DIVIDE_BY_ZERO    0xC0000094u
#define NT_EXC_PRIVILEGED            0xC0000096u
#define NT_EXC_SINGLE_STEP           0x80000004u
#define NT_EXC_MISALIGNMENT          0x80000002u
#define NT_EXC_FLT_INVALID           0xC0000090u
#define NT_EXC_STACK_OVERFLOW        0xC00000FDu

static void put32(uint8_t *b, uint32_t at, uint32_t v) {
    uint32_t i;

    for (i = 0; i < 4u; i++) {
        b[at + i] = (uint8_t)(v >> (8u * i));
    }
}

static void put64(uint8_t *b, uint32_t at, uint64_t v) {
    put32(b, at, (uint32_t)v);
    put32(b, at + 4u, (uint32_t)(v >> 32));
}

static uint32_t get32(const uint8_t *b, uint32_t at) {
    return (uint32_t)b[at] | (uint32_t)b[at + 1u] << 8 | (uint32_t)b[at + 2u] << 16 |
           (uint32_t)b[at + 3u] << 24;
}

/* Send the thread into ntdll with `record` and `ctx`, both copied onto its
 * stack below `below`. 0, or -1 when the stack would not take them. */
static int exception_enter(ks_regs_t *frame, const uint8_t *record, const uint8_t *ctx, uint64_t below) {
    const vibeos_image_t *img = ks_image(ks_current());
    vibeos_uregs_t r;
    uint64_t ctx_at, rec_at, sp;

    if (!img || img->entry == 0u) {
        return -1;
    }
    ctx_at = (below - NT_CONTEXT_SIZE) & ~0xFull;
    rec_at = (ctx_at - NT_RECORD_SIZE) & ~0xFull;
    sp = ((rec_at - 0x28u) & ~0xFull) - 8u;   /* home space, aligned as after a call */
    if (!nt_user_ok(sp, below - sp, 1) || nt_copy_out(ctx_at, ctx, NT_CONTEXT_SIZE) != 0 ||
        nt_copy_out(rec_at, record, NT_RECORD_SIZE) != 0) {
        return -1;
    }
    ks_regs_get(frame, &r);
    r.rip = img->entry;
    r.rsp = sp;
    r.rcx = NT_EXCEPTION_ENTRY_MARK;
    r.rdx = ctx_at;
    r.r8 = rec_at;
    return ks_regs_set(frame, &r);
}

/* What a vector is, to Windows, and the parameters an access violation adds:
 * 0 read, 1 write, 8 an instruction fetch, then the address. */
static void exception_record_of(const vibeos_siginfo_t *why, uint64_t rip, uint8_t *rec) {
    uint32_t code, i, nparams = 0;

    for (i = 0; i < NT_RECORD_SIZE; i++) {
        rec[i] = 0;
    }
    switch (why->trapno) {
        case 0u:  code = NT_EXC_INT_DIVIDE_BY_ZERO; break;
        case 1u:  code = NT_EXC_SINGLE_STEP; break;
        case 6u:  code = NT_EXC_ILLEGAL_INSTRUCTION; break;
        case 16u:
        case 19u: code = NT_EXC_FLT_INVALID; break;
        case 17u: code = NT_EXC_MISALIGNMENT; break;
        case 13u: code = NT_EXC_ACCESS_VIOLATION; break;   /* a general protection fault */
        default:  code = NT_EXC_ACCESS_VIOLATION; break;
    }
    put32(rec, 0x00u, code);
    put64(rec, 0x10u, rip);   /* ExceptionAddress */
    if (code == NT_EXC_ACCESS_VIOLATION) {
        uint64_t kind = 0, addr = why->trapno == 14u ? why->addr : ~0ull;

        if (why->trapno == 14u) {
            kind = (why->fault_err & 0x10u) ? 8u : (why->fault_err & 0x2u) ? 1u : 0u;
        }
        put64(rec, 0x20u, kind);
        put64(rec, 0x28u, addr);
        nparams = 2;
    }
    put32(rec, 0x18u, nparams);
}

int vibeos_nt_fault(struct ks_regs *frame, const struct vibeos_siginfo *why) {
    uint8_t rec[NT_RECORD_SIZE], ctx[NT_CONTEXT_SIZE];
    vibeos_uregs_t r;
    uint32_t i;

    ks_regs_get(frame, &r);
    exception_record_of(why, r.rip, rec);
    for (i = 0; i < NT_CONTEXT_SIZE; i++) {
        ctx[i] = 0;
    }
    nt_context_from_regs(&r, ctx, NT_CONTEXT_FULL);
    put32(ctx, 0x30u, NT_CONTEXT_FULL);
    return exception_enter(frame, rec, ctx, r.rsp) == 0;
}

/* NtRaiseException(ExceptionRecord, Context, FirstChance). A first chance
 * is dispatched as a fault is, from the context given; a second chance is
 * one nobody handled, and ends the process with its code. */
static long nt_raise_exception(ks_regs_t *frame, uint64_t rec_u, uint64_t ctx_u, uint64_t first) {
    uint8_t rec[NT_RECORD_SIZE], ctx[NT_CONTEXT_SIZE];
    uint64_t sp;

    if (nt_copy_in(rec, rec_u, sizeof(rec)) != 0 || nt_copy_in(ctx, ctx_u, sizeof(ctx)) != 0) {
        return NT_STATUS(VIBEOS_STATUS_ACCESS_VIOLATION);
    }
    if (get32(rec, 0x18u) > 15u) {
        return NT_STATUS(VIBEOS_STATUS_INVALID_PARAMETER);   /* EXCEPTION_MAXIMUM_PARAMETERS */
    }
    if (!first) {
        ks_task_exit_group((uint64_t)get32(rec, 0x00u));   /* nobody took it */
    }
    /* Below both the caller's stack and the context's own, so neither is
     * overwritten by the frames. */
    sp = ks_regs_sp(frame);
    {
        uint64_t csp = (uint64_t)get32(ctx, 0x98u) | (uint64_t)get32(ctx, 0x9Cu) << 32;

        if (csp != 0u && csp < sp) {
            sp = csp;
        }
    }
    if (exception_enter(frame, rec, ctx, sp) != 0) {
        ks_task_exit_group((uint64_t)get32(rec, 0x00u));
    }
    return NT_STATUS(VIBEOS_STATUS_SUCCESS);   /* the frame is ntdll's dispatcher now */
}

#define NT_EXCEPT_SERVICES(X) \
    X(VIBEOS_NT_NR_NtRaiseException, NtRaiseException, SIG_RETURN, \
      PTRS(IN(0, NT_RECORD_SIZE), IN(1, NT_CONTEXT_SIZE)), \
      nt_raise_exception(FRAME, ARG(0), ARG(1), ARG(2) & 0xFFu))

NT_DEFINE_SERVICES(except, NT_EXCEPT_SERVICES)
