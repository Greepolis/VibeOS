/* The exception runtime of our ntdll (docs/abi/windows.md W5 step 5).
 *
 * The kernel turns a fault, or an NtRaiseException with FirstChance set, into a
 * CONTEXT and an EXCEPTION_RECORD on the thread's own stack and sends the
 * thread into ntdll's entry with rcx 2 (start.c). From there dispatch is the
 * program's: the vectored handlers first, then the image's table-based SEH
 * walked over the stack with the x64 unwind information. A handler that resumes
 * execution returns here through NtContinue; one nobody takes goes back to the
 * kernel through NtRaiseException as a second chance, which ends the process.
 *
 * The dispatch and the unwind opcode interpreter are adapted from Wine 9.0,
 * dlls/ntdll/signal_x86_64.c and dlls/ntdll/exception.c. Wine is LGPL 2.1:
 *   Copyright (C) 1999 Alexandre Julliard
 * The structures that would otherwise come from winnt.h are declared here, at
 * Windows' x64 layout, because this file includes no Windows header: our own
 * programs are built freestanding against vibeos_ntdll.h, and Wine's tests and
 * anybody else's import these by the Windows names, which is the whole
 * contract. */

#define VIBEOS_BUILDING_NTDLL 1
#include "vibeos_ntdll.h"

/* ---- the fixed-width aliases Windows' own sources are written in ----------------- */
typedef uint8_t  BYTE;
typedef uint16_t WORD;
typedef uint16_t USHORT;
typedef uint32_t DWORD;
typedef uint32_t ULONG;
typedef int32_t  LONG;
typedef uint64_t ULONG64;
typedef int      BOOL;

#define NT_TRUE  1
#define NT_FALSE 0

/* ---- NTSTATUS values dispatch and unwind return, x64's -----------------------------
 * The exception codes a record carries (VIBEOS_STATUS_ACCESS_VIOLATION and the
 * rest) are in nt_layout.h; these are the outcomes of walking the stack, which
 * no handler ever sees. Windows' own numbers. */
#define EXC_STATUS_SUCCESS                 0x00000000u
#define EXC_STATUS_UNWIND                  0xC0000027u
#define EXC_STATUS_NONCONTINUABLE          0xC0000025u
#define EXC_STATUS_INVALID_DISPOSITION     0xC0000026u
#define EXC_STATUS_UNHANDLED_EXCEPTION     0xC0000144u
#define EXC_STATUS_INVALID_UNWIND_TARGET   0xC0000029u

/* EXCEPTION_RECORD.ExceptionFlags. */
#define EH_NONCONTINUABLE 0x01u
#define EH_UNWINDING      0x02u
#define EH_EXIT_UNWIND    0x04u
#define EH_STACK_INVALID  0x08u
#define EH_NESTED_CALL    0x10u
#define EH_TARGET_UNWIND  0x20u
#define EH_COLLIDED_UNWIND 0x40u

/* EXCEPTION_DISPOSITION, what a language handler returns. */
#define ExceptionContinueExecution 0
#define ExceptionContinueSearch    1
#define ExceptionNestedException   2
#define ExceptionCollidedUnwind    3

/* What a vectored handler returns. */
#define EXCEPTION_CONTINUE_EXECUTION (-1)
#define EXCEPTION_CONTINUE_SEARCH    0

/* The type of unwind information RtlVirtualUnwind is asked to honour. */
#define UNW_FLAG_NHANDLER 0u
#define UNW_FLAG_EHANDLER 1u
#define UNW_FLAG_UHANDLER 2u
#define UNW_FLAG_CHAININFO 4u

/* The PE data directory the x64 unwind tables live in (.pdata). */
#define IMAGE_DIRECTORY_ENTRY_EXCEPTION 3u

#define EXCEPTION_MAXIMUM_PARAMETERS 15

/* ---- the structures, at Windows' x64 offsets --------------------------------------- */

typedef struct { uint64_t Low; int64_t High; } M128A;   /* 16 bytes, aligned */

typedef struct _EXCEPTION_RECORD {
    DWORD ExceptionCode;                 /* 0x00 */
    DWORD ExceptionFlags;                /* 0x04 */
    struct _EXCEPTION_RECORD *ExceptionRecord; /* 0x08 */
    void *ExceptionAddress;              /* 0x10 */
    DWORD NumberParameters;              /* 0x18 */
    DWORD __align;                       /* 0x1c */
    ULONG64 ExceptionInformation[EXCEPTION_MAXIMUM_PARAMETERS]; /* 0x20 */
} EXCEPTION_RECORD;                      /* 0x98 */

/* XSAVE_FORMAT (FXSAVE area): the XMM registers sit at offset 0xa0 of it, so
 * context->FltSave.XmmRegisters is at 0x1a0 - where RtlCaptureContext's fxsave
 * and the kernel both place them. */
typedef struct {
    WORD ControlWord;
    WORD StatusWord;
    BYTE TagWord;
    BYTE Reserved1;
    WORD ErrorOpcode;
    DWORD ErrorOffset;
    WORD ErrorSelector;
    WORD Reserved2;
    DWORD DataOffset;
    WORD DataSelector;
    WORD Reserved3;
    DWORD MxCsr;
    DWORD MxCsr_Mask;
    M128A FloatRegisters[8];
    M128A XmmRegisters[16];
    BYTE Reserved4[96];
} XSAVE_FORMAT;                          /* 0x200 */

typedef struct {
    ULONG64 P1Home, P2Home, P3Home, P4Home, P5Home, P6Home; /* 0x00 */
    DWORD ContextFlags;                  /* 0x30 */
    DWORD MxCsr;                         /* 0x34 */
    WORD SegCs, SegDs, SegEs, SegFs, SegGs, SegSs; /* 0x38 */
    DWORD EFlags;                        /* 0x44 */
    ULONG64 Dr0, Dr1, Dr2, Dr3, Dr6, Dr7; /* 0x48 */
    /* The integer registers, contiguous so get_int_reg can index them:
     * reg 0 is Rax, 15 is R15. */
    ULONG64 Rax, Rcx, Rdx, Rbx, Rsp, Rbp, Rsi, Rdi; /* 0x78 */
    ULONG64 R8, R9, R10, R11, R12, R13, R14, R15;   /* 0xb8 */
    ULONG64 Rip;                         /* 0xf8 */
    XSAVE_FORMAT FltSave;                /* 0x100 */
    M128A VectorRegister[26];            /* 0x300 */
    ULONG64 VectorControl;               /* 0x4a0 */
    ULONG64 DebugControl;                /* 0x4a8 */
    ULONG64 LastBranchToRip;
    ULONG64 LastBranchFromRip;
    ULONG64 LastExceptionToRip;
    ULONG64 LastExceptionFromRip;
} CONTEXT;                               /* 0x4d0 */

typedef struct {
    DWORD BeginAddress;
    DWORD EndAddress;
    DWORD UnwindData;
} RUNTIME_FUNCTION;

union handler_data {
    RUNTIME_FUNCTION chain;
    ULONG handler;
};

struct opcode {
    BYTE offset;
    BYTE code : 4;
    BYTE info : 4;
};

struct UNWIND_INFO {
    BYTE version : 3;
    BYTE flags : 5;
    BYTE prolog;
    BYTE count;
    BYTE frame_reg : 4;
    BYTE frame_offset : 4;
    struct opcode opcodes[1];            /* info->count of them, then handler_data */
};

#define UWOP_PUSH_NONVOL     0
#define UWOP_ALLOC_LARGE     1
#define UWOP_ALLOC_SMALL     2
#define UWOP_SET_FPREG       3
#define UWOP_SAVE_NONVOL     4
#define UWOP_SAVE_NONVOL_FAR 5
#define UWOP_EPILOG          6
#define UWOP_SAVE_XMM128     8
#define UWOP_SAVE_XMM128_FAR 9
#define UWOP_PUSH_MACHFRAME  10

typedef struct {
    M128A *FloatingContext[16];
    ULONG64 *IntegerContext[16];
} KNONVOLATILE_CONTEXT_POINTERS;

typedef struct {
    ULONG64 ControlPc;                   /* 0x00 */
    ULONG64 ImageBase;                   /* 0x08 */
    RUNTIME_FUNCTION *FunctionEntry;     /* 0x10 */
    ULONG64 EstablisherFrame;            /* 0x18 */
    ULONG64 TargetIp;                    /* 0x20 */
    CONTEXT *ContextRecord;              /* 0x28 */
    void *LanguageHandler;               /* 0x30 */
    void *HandlerData;                   /* 0x38 */
    void *HistoryTable;                  /* 0x40 */
    DWORD ScopeIndex;                    /* 0x48 */
    DWORD Fill0;                         /* 0x4c */
} DISPATCHER_CONTEXT;

typedef struct {
    EXCEPTION_RECORD *ExceptionRecord;
    CONTEXT *ContextRecord;
} EXCEPTION_POINTERS;

/* A language handler and a vectored handler, by Windows' prototypes. */
typedef LONG (*PEXCEPTION_ROUTINE)(EXCEPTION_RECORD *, void *frame, CONTEXT *,
                                   DISPATCHER_CONTEXT *);
typedef LONG (*PVECTORED_EXCEPTION_HANDLER)(EXCEPTION_POINTERS *);

/* ---- what else this file reaches for ----------------------------------------------- */

/* start.c: the base of the loaded image that holds pc, or 0. */
uint64_t vibeos_ntdll_module_of(uint64_t pc);
/* Defined below in assembly; declared here for the C callers and the link. */
void RtlCaptureContext(CONTEXT *context);
void RtlRaiseException(EXCEPTION_RECORD *rec);

static uint32_t rd32(uint64_t a) { return *(const volatile uint32_t *)(uintptr_t)a; }

/* The process heap, as every program's GetProcessHeap reads it. */
static void *process_heap(void) {
    uint64_t peb = vibeos_nt_rd64(vibeos_nt_teb() + VIBEOS_TEB_PEB);
    return (void *)(uintptr_t)vibeos_nt_rd64(peb + VIBEOS_PEB_PROCESS_HEAP);
}

/* ---- a small spinlock -------------------------------------------------------------- *
 * The vectored list and the dynamic unwind tables have more than one possible
 * caller once there is more than one thread, so each locks itself. The lock is
 * never held across a handler call: the list is snapshotted under it and the
 * handlers run with it released. */
static volatile int g_veh_lock;

static void lock_take(volatile int *lock) {
    while (__atomic_exchange_n(lock, 1, __ATOMIC_ACQUIRE)) {
        /* another thread holds it; spin */
    }
}

static void lock_drop(volatile int *lock) {
    __atomic_store_n(lock, 0, __ATOMIC_RELEASE);
}

/* ================================================================================== *
 *  Vectored exception handlers
 * ================================================================================== */

typedef struct veh_node {
    struct veh_node *next;
    PVECTORED_EXCEPTION_HANDLER func;
} veh_node;

static veh_node *g_veh_head;

/* RtlAddVectoredExceptionHandler(First, Handler): First puts it at the head of
 * the chain, so it is called first. The return value is the handle
 * RtlRemoveVectoredExceptionHandler takes back. */
RTL_API void *RtlAddVectoredExceptionHandler(ULONG first, PVECTORED_EXCEPTION_HANDLER func) {
    veh_node *node = RtlAllocateHeap(process_heap(), 0, sizeof(*node));

    if (!node) {
        return 0;
    }
    node->func = func;
    lock_take(&g_veh_lock);
    if (first) {
        node->next = g_veh_head;
        g_veh_head = node;
    } else {
        node->next = 0;
        if (!g_veh_head) {
            g_veh_head = node;
        } else {
            veh_node *p = g_veh_head;

            while (p->next) {
                p = p->next;
            }
            p->next = node;
        }
    }
    lock_drop(&g_veh_lock);
    return node;
}

/* RtlRemoveVectoredExceptionHandler(Handle): non-zero if it was one of ours. */
RTL_API ULONG RtlRemoveVectoredExceptionHandler(void *handle) {
    veh_node **pp;
    ULONG ret = NT_FALSE;

    lock_take(&g_veh_lock);
    for (pp = &g_veh_head; *pp; pp = &(*pp)->next) {
        if (*pp == (veh_node *)handle) {
            veh_node *gone = *pp;

            *pp = gone->next;
            lock_drop(&g_veh_lock);
            RtlFreeHeap(process_heap(), 0, gone);
            return NT_TRUE;
        }
    }
    lock_drop(&g_veh_lock);
    return ret;
}

/* Call the chain. A handler can add or remove handlers, including itself, so the
 * list is copied under the lock and the copies are called with it released -
 * the func values stay valid even if a node is freed meanwhile. */
#define VEH_SNAPSHOT 64u

static LONG call_vectored_handlers(EXCEPTION_RECORD *rec, CONTEXT *context) {
    PVECTORED_EXCEPTION_HANDLER funcs[VEH_SNAPSHOT];
    EXCEPTION_POINTERS ptrs;
    LONG ret = EXCEPTION_CONTINUE_SEARCH;
    uint32_t n = 0, i;
    veh_node *p;

    lock_take(&g_veh_lock);
    for (p = g_veh_head; p && n < VEH_SNAPSHOT; p = p->next) {
        funcs[n++] = p->func;
    }
    lock_drop(&g_veh_lock);

    ptrs.ExceptionRecord = rec;
    ptrs.ContextRecord = context;
    for (i = 0; i < n; i++) {
        ret = funcs[i](&ptrs);
        if (ret == EXCEPTION_CONTINUE_EXECUTION) {
            break;
        }
    }
    return ret;
}

/* ================================================================================== *
 *  Finding a function's unwind information
 * ================================================================================== */

/* A PE data directory's RVA, with its size, or 0. */
static uint32_t pe_dir(uint64_t base, uint32_t which, uint32_t *size) {
    uint64_t opt = base + rd32(base + 0x3Cu) + 4u + 20u;
    uint32_t count = rd32(opt + 108u);

    if (which >= count) {
        return 0;
    }
    if (size) {
        *size = rd32(opt + 112u + which * 8u + 4u);
    }
    return rd32(opt + 112u + which * 8u);
}

/* Binary search of a sorted .pdata array; a chained entry (UnwindData odd)
 * is followed to the real one. */
static RUNTIME_FUNCTION *find_function_info(ULONG64 pc, ULONG64 base,
                                            RUNTIME_FUNCTION *func, ULONG size) {
    int min = 0;
    int max = (int)size - 1;

    while (min <= max) {
        int pos = (min + max) / 2;

        if (pc < base + func[pos].BeginAddress) {
            max = pos - 1;
        } else if (pc >= base + func[pos].EndAddress) {
            min = pos + 1;
        } else {
            func += pos;
            while (func->UnwindData & 1u) {
                func = (RUNTIME_FUNCTION *)(uintptr_t)(base + (func->UnwindData & ~1u));
            }
            return func;
        }
    }
    return 0;
}

/* ================================================================================== *
 *  Dynamic unwind tables (RtlAddFunctionTable / RtlDeleteFunctionTable)
 * ================================================================================== */

struct dyn_table {
    ULONG64 base;
    ULONG64 end;
    RUNTIME_FUNCTION *table;
    ULONG count;
    int used;
};

#define DYN_TABLES 32u
static struct dyn_table g_dyn[DYN_TABLES];

/* RtlAddFunctionTable(FunctionTable, EntryCount, BaseAddress): registers a
 * table for code that is not in a loaded module, so RtlLookupFunctionEntry can
 * find it. The span is from the first entry's start to the last entry's end. */
RTL_API BYTE RtlAddFunctionTable(RUNTIME_FUNCTION *table, DWORD count, ULONG64 base) {
    uint32_t i;

    if (!table || !count) {
        return NT_FALSE;
    }
    lock_take(&g_veh_lock);
    for (i = 0; i < DYN_TABLES; i++) {
        if (!g_dyn[i].used) {
            g_dyn[i].base = base;
            g_dyn[i].end = base + table[count - 1].EndAddress;
            g_dyn[i].table = table;
            g_dyn[i].count = count;
            g_dyn[i].used = 1;
            lock_drop(&g_veh_lock);
            return NT_TRUE;
        }
    }
    lock_drop(&g_veh_lock);
    return NT_FALSE;
}

/* RtlDeleteFunctionTable(FunctionTable): takes it back. */
RTL_API BYTE RtlDeleteFunctionTable(RUNTIME_FUNCTION *table) {
    uint32_t i;
    BYTE ret = NT_FALSE;

    lock_take(&g_veh_lock);
    for (i = 0; i < DYN_TABLES; i++) {
        if (g_dyn[i].used && g_dyn[i].table == table) {
            g_dyn[i].used = 0;
            ret = NT_TRUE;
            break;
        }
    }
    lock_drop(&g_veh_lock);
    return ret;
}

static RUNTIME_FUNCTION *lookup_dynamic(ULONG64 pc, ULONG64 *base) {
    RUNTIME_FUNCTION *func = 0;
    uint32_t i;

    lock_take(&g_veh_lock);
    for (i = 0; i < DYN_TABLES; i++) {
        if (g_dyn[i].used && pc >= g_dyn[i].base && pc < g_dyn[i].end) {
            *base = g_dyn[i].base;
            func = find_function_info(pc, g_dyn[i].base, g_dyn[i].table, g_dyn[i].count);
            break;
        }
    }
    lock_drop(&g_veh_lock);
    return func;
}

/* RtlLookupFunctionEntry(ControlPc, ImageBase, HistoryTable): the .pdata entry
 * covering pc, or NULL. A loaded module first, a dynamic table otherwise. */
RTL_API RUNTIME_FUNCTION *RtlLookupFunctionEntry(ULONG64 pc, ULONG64 *base, void *history) {
    uint64_t image = vibeos_ntdll_module_of(pc);
    uint32_t rva, size = 0;

    (void)history;
    if (image) {
        *base = image;
        rva = pe_dir(image, IMAGE_DIRECTORY_ENTRY_EXCEPTION, &size);
        if (rva) {
            return find_function_info(pc, image,
                                      (RUNTIME_FUNCTION *)(uintptr_t)(image + rva),
                                      size / (uint32_t)sizeof(RUNTIME_FUNCTION));
        }
        return 0;
    }
    return lookup_dynamic(pc, base);
}

/* ================================================================================== *
 *  RtlVirtualUnwind - interpret one function's unwind opcodes
 * ================================================================================== */

static ULONG64 get_int_reg(CONTEXT *context, int reg) {
    return *(&context->Rax + reg);
}

static void set_int_reg(CONTEXT *context, KNONVOLATILE_CONTEXT_POINTERS *ctx_ptr,
                        int reg, ULONG64 *val) {
    *(&context->Rax + reg) = *val;
    if (ctx_ptr) {
        ctx_ptr->IntegerContext[reg] = val;
    }
}

static void set_float_reg(CONTEXT *context, KNONVOLATILE_CONTEXT_POINTERS *ctx_ptr,
                          int reg, M128A *val) {
    /* A byte copy: val may be misaligned on the stack. */
    uint8_t *d = (uint8_t *)&context->FltSave.XmmRegisters[reg];
    const uint8_t *s = (const uint8_t *)val;
    uint32_t i;

    for (i = 0; i < sizeof(M128A); i++) {
        d[i] = s[i];
    }
    if (ctx_ptr) {
        ctx_ptr->FloatingContext[reg] = val;
    }
}

static int get_opcode_size(struct opcode op) {
    switch (op.code) {
    case UWOP_ALLOC_LARGE:
        return 2 + (op.info != 0);
    case UWOP_SAVE_NONVOL:
    case UWOP_SAVE_XMM128:
    case UWOP_EPILOG:
        return 2;
    case UWOP_SAVE_NONVOL_FAR:
    case UWOP_SAVE_XMM128_FAR:
        return 3;
    default:
        return 1;
    }
}

static BOOL is_inside_epilog(BYTE *pc, ULONG64 base, const RUNTIME_FUNCTION *function) {
    /* add or lea must be the first instruction, with a rex.W prefix. */
    if ((pc[0] & 0xf8) == 0x48) {
        switch (pc[1]) {
        case 0x81:   /* add $nnnn,%rsp */
            if (pc[0] == 0x48 && pc[2] == 0xc4) {
                pc += 7;
                break;
            }
            return NT_FALSE;
        case 0x83:   /* add $n,%rsp */
            if (pc[0] == 0x48 && pc[2] == 0xc4) {
                pc += 4;
                break;
            }
            return NT_FALSE;
        case 0x8d:   /* lea n(reg),%rsp */
            if (pc[0] & 0x06) {
                return NT_FALSE;   /* rex.RX must be clear */
            }
            if (((pc[2] >> 3) & 7) != 4) {
                return NT_FALSE;   /* destination must be %rsp */
            }
            if ((pc[2] & 7) == 4) {
                return NT_FALSE;   /* no SIB byte allowed */
            }
            if ((pc[2] >> 6) == 1) {
                pc += 4;
                break;
            }
            if ((pc[2] >> 6) == 2) {
                pc += 7;
                break;
            }
            return NT_FALSE;
        }
    }

    for (;;) {
        if ((*pc & 0xf0) == 0x40) {
            pc++;   /* rex prefix */
        }
        switch (*pc) {
        case 0x58: case 0x59: case 0x5a: case 0x5b:   /* pop reg */
        case 0x5c: case 0x5d: case 0x5e: case 0x5f:
            pc++;
            continue;
        case 0xc2:   /* ret $nn */
        case 0xc3:   /* ret */
            return NT_TRUE;
        case 0xe9:   /* jmp nnnn */
            pc += 5 + *(LONG *)(pc + 1);
            if ((ULONG64)(pc - (BYTE *)(uintptr_t)base) >= function->BeginAddress &&
                (ULONG64)(pc - (BYTE *)(uintptr_t)base) < function->EndAddress) {
                continue;
            }
            break;
        case 0xeb:   /* jmp n */
            pc += 2 + (signed char)pc[1];
            if ((ULONG64)(pc - (BYTE *)(uintptr_t)base) >= function->BeginAddress &&
                (ULONG64)(pc - (BYTE *)(uintptr_t)base) < function->EndAddress) {
                continue;
            }
            break;
        case 0xf3:   /* rep; ret */
            return pc[1] == 0xc3;
        }
        return NT_FALSE;
    }
}

static void interpret_epilog(BYTE *pc, CONTEXT *context, KNONVOLATILE_CONTEXT_POINTERS *ctx_ptr) {
    for (;;) {
        BYTE rex = 0;

        if ((*pc & 0xf0) == 0x40) {
            rex = *pc++ & 0x0f;   /* rex prefix */
        }
        switch (*pc) {
        case 0x58: case 0x59: case 0x5a: case 0x5b:   /* pop reg */
        case 0x5c: case 0x5d: case 0x5e: case 0x5f:
            set_int_reg(context, ctx_ptr, *pc - 0x58 + (rex & 1) * 8, (ULONG64 *)(uintptr_t)context->Rsp);
            context->Rsp += sizeof(ULONG64);
            pc++;
            continue;
        case 0x81:   /* add $nnnn,%rsp */
            context->Rsp += *(LONG *)(pc + 2);
            pc += 2 + sizeof(LONG);
            continue;
        case 0x83:   /* add $n,%rsp */
            context->Rsp += (signed char)pc[2];
            pc += 3;
            continue;
        case 0x8d:
            if ((pc[1] >> 6) == 1) {   /* lea n(reg),%rsp */
                context->Rsp = get_int_reg(context, (pc[1] & 7) + (rex & 1) * 8) + (signed char)pc[2];
                pc += 3;
            } else {                   /* lea nnnn(reg),%rsp */
                context->Rsp = get_int_reg(context, (pc[1] & 7) + (rex & 1) * 8) + *(LONG *)(pc + 2);
                pc += 2 + sizeof(LONG);
            }
            continue;
        case 0xc2:   /* ret $nn */
            context->Rip = *(ULONG64 *)(uintptr_t)context->Rsp;
            context->Rsp += sizeof(ULONG64) + *(WORD *)(pc + 1);
            return;
        case 0xc3:   /* ret */
        case 0xf3:   /* rep; ret */
            context->Rip = *(ULONG64 *)(uintptr_t)context->Rsp;
            context->Rsp += sizeof(ULONG64);
            return;
        case 0xe9:   /* jmp nnnn */
            pc += 5 + *(LONG *)(pc + 1);
            continue;
        case 0xeb:   /* jmp n */
            pc += 2 + (signed char)pc[1];
            continue;
        }
        return;
    }
}

/* RtlVirtualUnwind(Type, ImageBase, ControlPc, FunctionEntry, Context,
 *                  HandlerData, EstablisherFrame, ContextPointers): steps
 * Context back by one frame and, if the frame has a handler of the asked type,
 * returns it. */
RTL_API void *RtlVirtualUnwind(ULONG type, ULONG64 base, ULONG64 pc,
                               RUNTIME_FUNCTION *function, CONTEXT *context,
                               void **data, ULONG64 *frame_ret,
                               KNONVOLATILE_CONTEXT_POINTERS *ctx_ptr) {
    union handler_data *handler_data;
    ULONG64 frame, off;
    struct UNWIND_INFO *info;
    unsigned int i, prolog_offset = ~0u;
    BOOL mach_frame = NT_FALSE;

    frame = *frame_ret = context->Rsp;
    for (;;) {
        info = (struct UNWIND_INFO *)(uintptr_t)(base + function->UnwindData);
        handler_data = (union handler_data *)&info->opcodes[(info->count + 1) & ~1];

        if (info->version != 1 && info->version != 2) {
            return 0;
        }

        if (info->frame_reg) {
            frame = get_int_reg(context, info->frame_reg) - info->frame_offset * 16;
        }

        if (pc >= base + function->BeginAddress &&
            pc < base + function->BeginAddress + info->prolog) {
            prolog_offset = (unsigned int)(pc - base - function->BeginAddress);
        } else {
            prolog_offset = ~0u;
            /* Since Win10 1809 a zero opcode count is no longer special-cased. */
            if (info->count && is_inside_epilog((BYTE *)(uintptr_t)pc, base, function)) {
                interpret_epilog((BYTE *)(uintptr_t)pc, context, ctx_ptr);
                *frame_ret = frame;
                return 0;
            }
        }

        for (i = 0; i < info->count; i += get_opcode_size(info->opcodes[i])) {
            if (prolog_offset < info->opcodes[i].offset) {
                continue;   /* not reached yet in the prolog */
            }
            switch (info->opcodes[i].code) {
            case UWOP_PUSH_NONVOL:   /* pushq %reg */
                set_int_reg(context, ctx_ptr, info->opcodes[i].info, (ULONG64 *)(uintptr_t)context->Rsp);
                context->Rsp += sizeof(ULONG64);
                break;
            case UWOP_ALLOC_LARGE:   /* subq $nn,%rsp */
                if (info->opcodes[i].info) {
                    context->Rsp += *(DWORD *)&info->opcodes[i + 1];
                } else {
                    context->Rsp += *(USHORT *)&info->opcodes[i + 1] * 8;
                }
                break;
            case UWOP_ALLOC_SMALL:   /* subq $n,%rsp */
                context->Rsp += (info->opcodes[i].info + 1) * 8;
                break;
            case UWOP_SET_FPREG:     /* leaq nn(%rsp),%framereg */
                context->Rsp = *frame_ret = frame;
                break;
            case UWOP_SAVE_NONVOL:   /* movq %reg,n(%rsp) */
                off = frame + *(USHORT *)&info->opcodes[i + 1] * 8;
                set_int_reg(context, ctx_ptr, info->opcodes[i].info, (ULONG64 *)(uintptr_t)off);
                break;
            case UWOP_SAVE_NONVOL_FAR:   /* movq %reg,nn(%rsp) */
                off = frame + *(DWORD *)&info->opcodes[i + 1];
                set_int_reg(context, ctx_ptr, info->opcodes[i].info, (ULONG64 *)(uintptr_t)off);
                break;
            case UWOP_SAVE_XMM128:   /* movaps %xmmreg,n(%rsp) */
                off = frame + *(USHORT *)&info->opcodes[i + 1] * 16;
                set_float_reg(context, ctx_ptr, info->opcodes[i].info, (M128A *)(uintptr_t)off);
                break;
            case UWOP_SAVE_XMM128_FAR:   /* movaps %xmmreg,nn(%rsp) */
                off = frame + *(DWORD *)&info->opcodes[i + 1];
                set_float_reg(context, ctx_ptr, info->opcodes[i].info, (M128A *)(uintptr_t)off);
                break;
            case UWOP_PUSH_MACHFRAME:
                if (info->flags & UNW_FLAG_CHAININFO) {
                    break;
                }
                if (i + (unsigned int)get_opcode_size(info->opcodes[i]) < info->count) {
                    break;
                }
                if (info->opcodes[i].info) {
                    context->Rsp += 0x8;
                }
                context->Rip = *(ULONG64 *)(uintptr_t)context->Rsp;
                context->Rsp = *(ULONG64 *)(uintptr_t)(context->Rsp + 24);
                mach_frame = NT_TRUE;
                break;
            case UWOP_EPILOG:
                if (info->version == 2) {
                    break;   /* nothing to do */
                }
                /* fall through */
            default:
                break;       /* an opcode we do not interpret */
            }
        }

        if (!(info->flags & UNW_FLAG_CHAININFO)) {
            break;
        }
        function = &handler_data->chain;   /* restart with the chained info */
    }

    if (!mach_frame) {
        context->Rip = *(ULONG64 *)(uintptr_t)context->Rsp;
        context->Rsp += sizeof(ULONG64);
    }

    if (!(info->flags & type)) {
        return 0;   /* no handler of this type */
    }
    if (prolog_offset != ~0u) {
        return 0;   /* inside the prolog */
    }

    *data = &handler_data->handler + 1;
    return (void *)(uintptr_t)(base + handler_data->handler);
}

/* ================================================================================== *
 *  Walking the stack: virtual_unwind over one frame
 * ================================================================================== */

static DWORD virtual_unwind(ULONG type, DISPATCHER_CONTEXT *dispatch, CONTEXT *context) {
    dispatch->ImageBase = 0;
    dispatch->ScopeIndex = 0;
    dispatch->ControlPc = context->Rip;

    dispatch->FunctionEntry = RtlLookupFunctionEntry(context->Rip, &dispatch->ImageBase, dispatch->HistoryTable);
    if (dispatch->FunctionEntry) {
        dispatch->LanguageHandler = RtlVirtualUnwind(type, dispatch->ImageBase, context->Rip,
                                                     dispatch->FunctionEntry, context,
                                                     &dispatch->HandlerData, &dispatch->EstablisherFrame,
                                                     0);
        return EXC_STATUS_SUCCESS;
    }

    /* No unwind information: a leaf function. Its return address is on top of
     * the stack. */
    dispatch->EstablisherFrame = context->Rsp;
    dispatch->LanguageHandler = 0;
    context->Rip = *(ULONG64 *)(uintptr_t)context->Rsp;
    context->Rsp += sizeof(ULONG64);
    return EXC_STATUS_SUCCESS;
}

static ULONG64 stack_base(void) {
    return vibeos_nt_rd64(vibeos_nt_teb() + VIBEOS_TEB_STACK_BASE);
}
static ULONG64 stack_limit(void) {
    return vibeos_nt_rd64(vibeos_nt_teb() + VIBEOS_TEB_STACK_LIMIT);
}

/* ================================================================================== *
 *  Dispatch to the frame-based handlers
 * ================================================================================== */

static LONG call_handler(EXCEPTION_RECORD *rec, CONTEXT *context, DISPATCHER_CONTEXT *dispatch) {
    PEXCEPTION_ROUTINE handler = (PEXCEPTION_ROUTINE)dispatch->LanguageHandler;
    LONG res = handler(rec, (void *)(uintptr_t)dispatch->EstablisherFrame, context, dispatch);

    rec->ExceptionFlags &= EH_NONCONTINUABLE;
    return res;
}

static DWORD call_stack_handlers(EXCEPTION_RECORD *rec, CONTEXT *orig_context) {
    DISPATCHER_CONTEXT dispatch;
    CONTEXT context;
    DWORD status;

    context = *orig_context;
    dispatch.TargetIp = 0;
    dispatch.ContextRecord = &context;
    dispatch.HistoryTable = 0;
    for (;;) {
        status = virtual_unwind(UNW_FLAG_EHANDLER, &dispatch, &context);
        if (status != EXC_STATUS_SUCCESS) {
            return status;
        }

    unwind_done:
        if (!dispatch.EstablisherFrame) {
            break;
        }
        if ((dispatch.EstablisherFrame & 7) ||
            dispatch.EstablisherFrame < stack_limit() ||
            dispatch.EstablisherFrame > stack_base()) {
            rec->ExceptionFlags |= EH_STACK_INVALID;
            break;
        }

        if (dispatch.LanguageHandler) {
            switch (call_handler(rec, orig_context, &dispatch)) {
            case ExceptionContinueExecution:
                if (rec->ExceptionFlags & EH_NONCONTINUABLE) {
                    return EXC_STATUS_NONCONTINUABLE;
                }
                return EXC_STATUS_SUCCESS;
            case ExceptionContinueSearch:
                break;
            case ExceptionNestedException:
                rec->ExceptionFlags |= EH_NESTED_CALL;
                break;
            case ExceptionCollidedUnwind: {
                ULONG64 frame;

                context = *dispatch.ContextRecord;
                dispatch.ContextRecord = &context;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, dispatch.ImageBase, dispatch.ControlPc,
                                 dispatch.FunctionEntry, &context, &dispatch.HandlerData,
                                 &frame, 0);
                goto unwind_done;
            }
            default:
                return EXC_STATUS_INVALID_DISPOSITION;
            }
        }

        if (context.Rsp == stack_base()) {
            break;
        }
    }
    return EXC_STATUS_UNHANDLED_EXCEPTION;
}

/* The entry start.c calls for a first-chance exception (rcx == 2): 1 if a
 * handler took it and the CONTEXT, possibly moved, is to be resumed; 0 if
 * nobody did, and the caller raises a second chance. */
int vibeos_rtl_dispatch_exception(void *record, void *context) {
    EXCEPTION_RECORD *rec = record;
    CONTEXT *ctx = context;

    if (call_vectored_handlers(rec, ctx) == EXCEPTION_CONTINUE_EXECUTION) {
        return 1;
    }
    if (call_stack_handlers(rec, ctx) == EXC_STATUS_SUCCESS) {
        return 1;
    }
    return 0;
}

/* ================================================================================== *
 *  RtlUnwindEx - run the unwind handlers down to a target frame
 * ================================================================================== */

/* Raise a non-continuable exception with a given status; used when an unwind
 * cannot proceed. Does not return. */
static __attribute__((noreturn)) void raise_status(DWORD status, EXCEPTION_RECORD *cause) {
    EXCEPTION_RECORD rec;

    rec.ExceptionCode = status;
    rec.ExceptionFlags = EH_NONCONTINUABLE;
    rec.ExceptionRecord = cause;
    rec.ExceptionAddress = 0;
    rec.NumberParameters = 0;
    for (;;) {
        RtlRaiseException(&rec);   /* never returns */
    }
}

static LONG call_unwind_handler(EXCEPTION_RECORD *rec, DISPATCHER_CONTEXT *dispatch) {
    PEXCEPTION_ROUTINE handler = (PEXCEPTION_ROUTINE)dispatch->LanguageHandler;

    return handler(rec, (void *)(uintptr_t)dispatch->EstablisherFrame, dispatch->ContextRecord, dispatch);
}

/* RtlUnwindEx(TargetFrame, TargetIp, ExceptionRecord, ReturnValue, Context,
 *             HistoryTable): unwinds the stack to TargetFrame, calling each
 * frame's unwind handler, then resumes at TargetIp with ReturnValue in rax. */
RTL_API void RtlUnwindEx(void *end_frame, void *target_ip, EXCEPTION_RECORD *rec,
                         void *retval, CONTEXT *context, void *table) {
    EXCEPTION_RECORD record;
    DISPATCHER_CONTEXT dispatch;
    CONTEXT new_context;
    DWORD status;

    RtlCaptureContext(context);
    new_context = *context;

    if (!rec) {
        record.ExceptionCode = EXC_STATUS_UNWIND;
        record.ExceptionFlags = 0;
        record.ExceptionRecord = 0;
        record.ExceptionAddress = (void *)(uintptr_t)context->Rip;
        record.NumberParameters = 0;
        rec = &record;
    }
    rec->ExceptionFlags |= EH_UNWINDING | (end_frame ? 0 : EH_EXIT_UNWIND);

    dispatch.EstablisherFrame = context->Rsp;
    dispatch.TargetIp = (ULONG64)(uintptr_t)target_ip;
    dispatch.ContextRecord = context;
    dispatch.HistoryTable = table;

    for (;;) {
        status = virtual_unwind(UNW_FLAG_UHANDLER, &dispatch, &new_context);
        if (status != EXC_STATUS_SUCCESS) {
            raise_status(status, rec);
        }

    unwind_done:
        if (!dispatch.EstablisherFrame) {
            break;
        }
        if ((dispatch.EstablisherFrame & 7) ||
            dispatch.EstablisherFrame < stack_limit() ||
            dispatch.EstablisherFrame > stack_base()) {
            rec->ExceptionFlags |= EH_STACK_INVALID;
            break;
        }

        if (dispatch.LanguageHandler) {
            if (end_frame && dispatch.EstablisherFrame > (ULONG64)(uintptr_t)end_frame) {
                raise_status(EXC_STATUS_INVALID_UNWIND_TARGET, rec);
            }
            if (dispatch.EstablisherFrame == (ULONG64)(uintptr_t)end_frame) {
                rec->ExceptionFlags |= EH_TARGET_UNWIND;
            }
            if (call_unwind_handler(rec, &dispatch) == ExceptionCollidedUnwind) {
                ULONG64 frame;

                new_context = *dispatch.ContextRecord;
                *context = new_context;
                dispatch.ContextRecord = context;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, dispatch.ImageBase, dispatch.ControlPc,
                                 dispatch.FunctionEntry, &new_context, &dispatch.HandlerData,
                                 &frame, 0);
                rec->ExceptionFlags |= EH_COLLIDED_UNWIND;
                goto unwind_done;
            }
            rec->ExceptionFlags &= ~EH_COLLIDED_UNWIND;
        }

        if (dispatch.EstablisherFrame == (ULONG64)(uintptr_t)end_frame) {
            break;
        }
        *context = new_context;
    }

    context->Rax = (ULONG64)(uintptr_t)retval;
    context->Rip = (ULONG64)(uintptr_t)target_ip;
    (void)NtContinue(context, 0);
}

/* RtlUnwind(TargetFrame, TargetIp, ExceptionRecord, ReturnValue): the shorter
 * form, over its own captured context. */
RTL_API void RtlUnwind(void *frame, void *target_ip, EXCEPTION_RECORD *rec, void *retval) {
    CONTEXT context;

    RtlUnwindEx(frame, target_ip, rec, retval, &context, 0);
}

/* RtlRestoreContext(Context, ExceptionRecord): resume a context. We do not
 * carry the longjmp or consolidate cases, so this is NtContinue. */
RTL_API void RtlRestoreContext(CONTEXT *context, EXCEPTION_RECORD *rec) {
    (void)rec;
    (void)NtContinue(context, 0);
}

/* ================================================================================== *
 *  The two routines that must be assembly
 * ================================================================================== *
 *
 * RtlCaptureContext stores the caller's register state - the return address as
 * Rip and the stack as it was before the call as Rsp - so an unwind can start
 * from the point of call. RtlRaiseException raises a software exception: it
 * captures a context pointing at its own return, fixes Rsp, Rcx, Rip and the
 * record's ExceptionAddress to the caller, and dispatches it; what nobody takes
 * goes to NtRaiseException as a second chance, which does not return. Both are
 * exported through a .drectve, as the system-call stubs are (stubs.S). */

__asm__(
    ".text\n\t"
    ".globl RtlCaptureContext\n\t"
    ".p2align 4\n"
    "RtlCaptureContext:\n\t"
    "pushfq\n\t"
    "movl $0x10000f,0x30(%rcx)\n\t"      /* context->ContextFlags */
    "stmxcsr 0x34(%rcx)\n\t"             /* context->MxCsr */
    "movw %cs,0x38(%rcx)\n\t"
    "movw %ds,0x3a(%rcx)\n\t"
    "movw %es,0x3c(%rcx)\n\t"
    "movw %fs,0x3e(%rcx)\n\t"
    "movw %gs,0x40(%rcx)\n\t"
    "movw %ss,0x42(%rcx)\n\t"
    "popq 0x44(%rcx)\n\t"                /* context->EFlags */
    "movq %rax,0x78(%rcx)\n\t"
    "movq %rcx,0x80(%rcx)\n\t"
    "movq %rdx,0x88(%rcx)\n\t"
    "movq %rbx,0x90(%rcx)\n\t"
    "leaq 8(%rsp),%rax\n\t"
    "movq %rax,0x98(%rcx)\n\t"           /* context->Rsp (before the call) */
    "movq %rbp,0xa0(%rcx)\n\t"
    "movq %rsi,0xa8(%rcx)\n\t"
    "movq %rdi,0xb0(%rcx)\n\t"
    "movq %r8,0xb8(%rcx)\n\t"
    "movq %r9,0xc0(%rcx)\n\t"
    "movq %r10,0xc8(%rcx)\n\t"
    "movq %r11,0xd0(%rcx)\n\t"
    "movq %r12,0xd8(%rcx)\n\t"
    "movq %r13,0xe0(%rcx)\n\t"
    "movq %r14,0xe8(%rcx)\n\t"
    "movq %r15,0xf0(%rcx)\n\t"
    "movq (%rsp),%rax\n\t"
    "movq %rax,0xf8(%rcx)\n\t"           /* context->Rip (the return address) */
    "fxsave 0x100(%rcx)\n\t"             /* context->FltSave */
    "ret\n\t"
    ".section .drectve\n\t"
    ".ascii \" -export:RtlCaptureContext\"\n\t"
    ".text\n\t"

    ".globl RtlRaiseException\n\t"
    ".p2align 4\n"
    "RtlRaiseException:\n\t"
    "sub $0x4f8,%rsp\n\t"
    "movq %rcx,0x500(%rsp)\n\t"          /* save the record pointer */
    "leaq 0x20(%rsp),%rcx\n\t"           /* context */
    "call RtlCaptureContext\n\t"
    "leaq 0x20(%rsp),%rdx\n\t"           /* context */
    "leaq 0x500(%rsp),%rax\n\t"          /* the stack as the caller had it */
    "movq %rax,0x98(%rdx)\n\t"           /* context->Rsp */
    "movq 0x500(%rsp),%rcx\n\t"          /* the record */
    "movq %rcx,0x80(%rdx)\n\t"           /* context->Rcx = record (as Windows does) */
    "movq 0x4f8(%rsp),%rax\n\t"          /* the return address */
    "movq %rax,0xf8(%rdx)\n\t"           /* context->Rip */
    "movq %rax,0x10(%rcx)\n\t"           /* rec->ExceptionAddress */
    /* vibeos_rtl_dispatch_exception(record, context): rcx=record, rdx=context. */
    "call vibeos_rtl_dispatch_exception\n\t"
    "testl %eax,%eax\n\t"
    "jz 1f\n\t"
    "leaq 0x20(%rsp),%rcx\n\t"           /* handled: NtContinue(context, FALSE) */
    "xorl %edx,%edx\n\t"
    "call NtContinue\n"
    "1:\n\t"
    "movq 0x500(%rsp),%rcx\n\t"          /* record */
    "leaq 0x20(%rsp),%rdx\n\t"           /* context */
    "xorl %r8d,%r8d\n\t"                 /* FirstChance = FALSE: second chance */
    "call NtRaiseException\n"
    "2:\n\t"
    "jmp 2b\n\t"                         /* neither returns */
    ".section .drectve\n\t"
    ".ascii \" -export:RtlRaiseException\"\n\t"
    ".text\n\t"
);
