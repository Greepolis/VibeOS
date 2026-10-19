/* Host tests for the NT personality (docs/abi/windows.md W1).
 *
 * The registry against nt_services.def, the dispatcher's three kinds of no,
 * Windows' calling convention for the arguments past the fourth, the W1
 * services as a program sees them, and the blocks a process starts with - all
 * on tests/kernel/ksvc_fake.c, as the Linux handlers are tested. */

#include <stdio.h>
#include <string.h>

#include "ksvc_fake.h"
#include "vibeos/abi_nt.h"
#include "vibeos/abi_linux.h"
#include "vibeos/linux_exports.h"
#include "vibeos/nt_layout.h"
#include "vibeos/ksvc.h"

int test_nt_abi(void);

static int g_fail;

static void expect(int cond, const char *what) {
    if (!cond) {
        printf("FAIL:nt_abi %s\n", what);
        g_fail = 1;
    }
}

#define ST(s) ((long)(uint32_t)(s))

static long nt(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
               kf_outcome_t *out) {
    return kf_call(nt_syscall, nr, a0, a1, a2, a3, 0, 0, out);
}

static int fresh(uint32_t pid) {
    int slot;

    kf_reset();
    vibeos_linux_abi_init();
    vibeos_nt_abi_init();
    slot = kf_spawn(pid, pid);
    kf_set_current(slot);
    return slot;
}

/* The arguments past the fourth, where Windows' convention leaves them: a
 * stack whose pointer the call arrives with, the fifth at sp + 0x28. */
static uint64_t stack_args(const uint64_t *rest, uint32_t n) {
    uint64_t sp = kf_ualloc(0x28u + 8u * 16u);
    uint32_t i;

    for (i = 0; i < n; i++) {
        *(uint64_t *)kf_uptr(sp + 0x28u + 8u * i) = rest[i];
    }
    return sp;
}

static void t_registry(void) {
    static const vibeos_row_t past[] = {
        { VIBEOS_NT_SERVICES, "past", VIBEOS_OP_WRITE, (vibeos_handler_t)0 },
    };
    const vibeos_nt_service_t *w;

    fresh(30);
    expect(vibeos_abi_nt_disagreement() == (uint32_t)VIBEOS_NT_SERVICES,
           "every DONE and PARTIAL service has a row, and no other does");
    w = vibeos_nt_service(VIBEOS_NT_NR_NtWriteFile);
    expect(w && w->nr == (uint32_t)VIBEOS_NT_NR_NtWriteFile && w->args == 9u &&
           strcmp(w->name, "NtWriteFile") == 0, "NtWriteFile is its line: nine arguments");
    expect(VIBEOS_NT_SERVICES == 237, "237 services, as Wine 9.0 has them");
    expect(vibeos_nt_service(VIBEOS_NT_SERVICES) == 0, "past the table is no service");
    expect(vibeos_abi_nt_register(past, 1u) != 0, "a row past the table is refused");
    expect(vibeos_abi_nt()->lookup(VIBEOS_NT_NR_NtAddAtom) == 0, "a MISSING service has no row");
}

static void t_dispatch_refusals(void) {
    uint64_t un, ref;

    fresh(31);
    un = g_nt_unimplemented;
    ref = g_nt_refused;
    expect(nt(VIBEOS_NT_SERVICES + 5u, 0, 0, 0, 0, 0) == ST(VIBEOS_STATUS_INVALID_SYSTEM_SERVICE),
           "a number past the table names no service");
    expect(nt(VIBEOS_NT_NR_NtAddAtom, 4, 0, 0, 0, 0) == ST(VIBEOS_STATUS_NOT_IMPLEMENTED) &&
           g_nt_unimplemented == un + 1u && g_nt_last_nr == (uint64_t)VIBEOS_NT_NR_NtAddAtom &&
           strstr(kf_console(), "unimplemented NT service") &&
           strstr(kf_console(), "NtAddAtom"),
           "a service not written yet is NOT_IMPLEMENTED, counted and named");
    expect(nt(VIBEOS_NT_NR_NtLoadDriver, 0, 0, 0, 0, 0) == ST(VIBEOS_STATUS_NOT_IMPLEMENTED) &&
           g_nt_refused == ref + 1u && g_nt_unimplemented == un + 1u,
           "a refused service is counted apart, and not as news");
}

/* How many times `what` is on the console: the console's own framing (the
 * ring-3 write prefix) sits between writes, so a count is what can be compared. */
static int con_count(const char *what) {
    const char *p = kf_console();
    int n = 0;

    while ((p = strstr(p, what)) != 0) {
        n++;
        p++;
    }
    return n;
}

static void t_write_file(void) {
    uint64_t iosb = kf_ualloc(16), buf = kf_ualloc(16), off = kf_ualloc(8);
    uint64_t rest[5];
    const vibeos_nt_iosb_t *io = (const vibeos_nt_iosb_t *)kf_uptr(iosb);

    fresh(32);
    memcpy(kf_uptr(buf), "nt-hello", 8);
    /* The length's upper half is what a caller left in the register: a ULONG
     * is 32 bits, and reading 64 would be four billion bytes. */
    rest[0] = iosb;
    rest[1] = buf;
    rest[2] = 0xDEADBEEF00000005ull;
    rest[3] = 0;
    rest[4] = 0;
    kf_next_sp(stack_args(rest, 5));
    expect(nt(VIBEOS_NT_NR_NtWriteFile, 8, 0, 0, 0, 0) == ST(VIBEOS_STATUS_SUCCESS),
           "NtWriteFile to standard output succeeds");
    expect(con_count("nt-he") == 1 && con_count("nt-hel") == 0,
           "five bytes reach the console: the length is read as 32 bits");
    expect(io->status == 0u && io->information == 5u, "the status block says success, five bytes");

    kf_next_sp(stack_args(rest, 5));
    expect(nt(VIBEOS_NT_NR_NtWriteFile, 6, 0, 0, 0, 0) == ST(VIBEOS_STATUS_INVALID_HANDLE),
           "a handle that is not a multiple of four is no handle");
    kf_next_sp(stack_args(rest, 5));
    expect(nt(VIBEOS_NT_NR_NtWriteFile, 4u * 300u, 0, 0, 0, 0) == ST(VIBEOS_STATUS_INVALID_HANDLE),
           "a handle naming no descriptor is INVALID_HANDLE");
    kf_next_sp(stack_args(rest, 5));
    expect(nt(VIBEOS_NT_NR_NtWriteFile, 8, 0x40, 0, 0, 0) == ST(VIBEOS_STATUS_NOT_IMPLEMENTED),
           "an event asks for W4's asynchronous completion: refused, not ignored");
    rest[3] = off;
    kf_next_sp(stack_args(rest, 5));
    expect(nt(VIBEOS_NT_NR_NtWriteFile, 8, 0, 0, 0, 0) == ST(VIBEOS_STATUS_NOT_IMPLEMENTED) &&
           con_count("nt-he") == 1, "an offset is W4's: refused, and nothing written");
    rest[3] = 0;
    rest[0] = 0x10u;   /* the status block is nobody's memory */
    kf_next_sp(stack_args(rest, 5));
    expect(nt(VIBEOS_NT_NR_NtWriteFile, 8, 0, 0, 0, 0) == ST(VIBEOS_STATUS_ACCESS_VIOLATION) &&
           con_count("nt-he") == 1,
           "a bad status block is an access violation before anything is written");
    /* A stack pointer at the kernel's own memory, holding arguments that would
     * work: the copy here reads any address that does not fault, as the
     * machine's does, so only the judgement stops the kernel taking a
     * program's arguments out of itself (CLAUDE.md, M-082). */
    {
        static uint64_t kstack[16];

        rest[0] = iosb;
        memcpy(&kstack[5], rest, sizeof(rest));
        kf_next_sp((uint64_t)(uintptr_t)kstack);
        expect(nt(VIBEOS_NT_NR_NtWriteFile, 8, 0, 0, 0, 0) == ST(VIBEOS_STATUS_ACCESS_VIOLATION) &&
                   con_count("nt-he") == 1,
               "a stack pointer into the kernel is judged before anything is read through it");
    }
    {
        uint64_t sp = stack_args(rest, 5);
        kf_fault(sp + 0x28u, 40u);
        kf_next_sp(sp);
        expect(nt(VIBEOS_NT_NR_NtWriteFile, 8, 0, 0, 0, 0) == ST(VIBEOS_STATUS_ACCESS_VIOLATION),
               "arguments on a stack that faults are the caller's access violation");
    }
}

static void t_process(void) {
    uint64_t buf = kf_ualloc(64), got = kf_ualloc(4);
    const vibeos_nt_process_basic_t *pbi = (const vibeos_nt_process_basic_t *)kf_uptr(buf);
    int me = fresh(33);
    kf_outcome_t how;
    const uint64_t zero = 0;

    ks_image(me)->start_arg = 0x8003FF0000ull;
    kf_next_sp(stack_args(&zero, 1));
    expect(nt(VIBEOS_NT_NR_NtQueryInformationProcess, VIBEOS_NT_CURRENT_PROCESS,
              0xFFFFFFFF00000000ull | VIBEOS_NT_PROCESS_BASIC_INFORMATION, buf,
              0xABCD000000000030ull, 0) == ST(VIBEOS_STATUS_SUCCESS),
           "ProcessBasicInformation for oneself, with garbage in the 32-bit arguments' upper halves");
    expect(pbi->peb == 0x8003FF0000ull && pbi->unique_process_id == 33u,
           "the PEB the image was started with, and the process id");
    kf_next_sp(stack_args(&got, 1));
    expect(nt(VIBEOS_NT_NR_NtQueryInformationProcess, VIBEOS_NT_CURRENT_PROCESS, 0, buf, 48, 0) ==
               ST(VIBEOS_STATUS_SUCCESS) && *(uint32_t *)kf_uptr(got) == 48u,
           "the fifth argument, from the stack: the length written");
    kf_next_sp(stack_args(&zero, 1));
    expect(nt(VIBEOS_NT_NR_NtQueryInformationProcess, VIBEOS_NT_CURRENT_PROCESS, 0, buf, 8, 0) ==
               ST(VIBEOS_STATUS_INFO_LENGTH_MISMATCH), "a short buffer is a length mismatch");
    kf_next_sp(stack_args(&zero, 1));
    expect(nt(VIBEOS_NT_NR_NtQueryInformationProcess, VIBEOS_NT_CURRENT_PROCESS, 7, buf, 48, 0) ==
               ST(VIBEOS_STATUS_INVALID_INFO_CLASS), "a class W1 does not answer");
    kf_next_sp(stack_args(&zero, 1));
    expect(nt(VIBEOS_NT_NR_NtQueryInformationProcess, 4, 0, buf, 48, 0) ==
               ST(VIBEOS_STATUS_INVALID_HANDLE), "another process's handle does not exist yet");
    kf_next_sp(stack_args(&zero, 1));
    expect(nt(VIBEOS_NT_NR_NtQueryInformationProcess, VIBEOS_NT_CURRENT_PROCESS, 0, 0x10, 48, 0) ==
               ST(VIBEOS_STATUS_ACCESS_VIOLATION), "a buffer that is nobody's memory");
    expect(nt(VIBEOS_NT_NR_NtTerminateProcess, 4, 0, 0, 0, 0) == ST(VIBEOS_STATUS_INVALID_HANDLE),
           "terminating another process needs a handle W1 cannot give");
    (void)nt(VIBEOS_NT_NR_NtTerminateProcess, VIBEOS_NT_CURRENT_PROCESS,
             0xFFFFFFFF00000103ull, 0, 0, &how);
    expect(how == KF_EXITED && kf_exit_code() == 0x103u,
           "NtTerminateProcess of oneself ends the process with its status");
}

static void t_system(void) {
    uint64_t t = kf_ualloc(8), c = kf_ualloc(8), f = kf_ualloc(8), buf = kf_ualloc(64);
    const vibeos_nt_system_basic_t *sbi = (const vibeos_nt_system_basic_t *)kf_uptr(buf);
    uint64_t first;

    fresh(34);
    expect(nt(VIBEOS_NT_NR_NtQuerySystemTime, t, 0, 0, 0, 0) == ST(VIBEOS_STATUS_SUCCESS) &&
           *(uint64_t *)kf_uptr(t) >= VIBEOS_NT_EPOCH_DELTA,
           "the system time is the kernel's clock moved to 1601");
    expect(nt(VIBEOS_NT_NR_NtQueryPerformanceCounter, c, f, 0, 0, 0) == ST(VIBEOS_STATUS_SUCCESS) &&
           *(uint64_t *)kf_uptr(f) == VIBEOS_NT_PERF_FREQUENCY, "the counter runs at 10 MHz");
    first = *(uint64_t *)kf_uptr(c);
    kf_cpu(3, 1);
    expect(nt(VIBEOS_NT_NR_NtQueryPerformanceCounter, c, 0, 0, 0, 0) == ST(VIBEOS_STATUS_SUCCESS) &&
           *(uint64_t *)kf_uptr(c) > first, "and moves with the clock; the frequency may be null");
    expect(nt(VIBEOS_NT_NR_NtQuerySystemTime, 0x10, 0, 0, 0, 0) == ST(VIBEOS_STATUS_ACCESS_VIOLATION),
           "a time to nobody's memory");
    expect(nt(VIBEOS_NT_NR_NtQuerySystemInformation, VIBEOS_NT_SYSTEM_BASIC_INFORMATION, buf, 64, 0, 0) ==
               ST(VIBEOS_STATUS_SUCCESS) && sbi->page_size == 4096u && sbi->processors == 4u &&
           sbi->active_processors_mask == 0xFu, "SystemBasicInformation: the page, the processors");
    expect(nt(VIBEOS_NT_NR_NtQuerySystemInformation, VIBEOS_NT_SYSTEM_BASIC_INFORMATION, buf, 60, 0, 0) ==
               ST(VIBEOS_STATUS_INFO_LENGTH_MISMATCH), "and a mismatched length");
    expect(nt(VIBEOS_NT_NR_NtQuerySystemInformation, 5, buf, 64, 0, 0) ==
               ST(VIBEOS_STATUS_INVALID_INFO_CLASS), "a class W1 does not answer");
}

/* ---- the object manager (W2) ---------------------------------------------------------- */

/* OBJECT_ATTRIBUTES in user memory naming `ascii`, or with no name for null. */
static uint64_t oattr(const char *ascii, uint32_t attributes) {
    uint64_t oa = kf_ualloc(VIBEOS_OA_SIZE);
    uint8_t *p = (uint8_t *)kf_uptr(oa);

    *(uint32_t *)(p + VIBEOS_OA_LENGTH) = VIBEOS_OA_SIZE;
    *(uint32_t *)(p + VIBEOS_OA_ATTRIBUTES) = attributes;
    if (ascii) {
        uint32_t n = (uint32_t)strlen(ascii), i;
        uint64_t us = kf_ualloc(VIBEOS_USTR_SIZE), buf = kf_ualloc(2u * n + 2u);
        uint8_t *u = (uint8_t *)kf_uptr(us);
        uint16_t *w = (uint16_t *)kf_uptr(buf);

        for (i = 0; i < n; i++) {
            w[i] = (uint8_t)ascii[i];
        }
        *(uint16_t *)(u + VIBEOS_USTR_LENGTH) = (uint16_t)(2u * n);
        *(uint16_t *)(u + VIBEOS_USTR_MAXIMUM) = (uint16_t)(2u * n + 2u);
        *(uint64_t *)(u + VIBEOS_USTR_BUFFER) = buf;
        *(uint64_t *)(p + VIBEOS_OA_OBJECT_NAME) = us;
    }
    return oa;
}

static const uint64_t g_zero5 = 0;

/* NtCreateEvent's fifth argument (InitialState) is on the stack. */
static long create_event(uint64_t *h, uint32_t access, uint64_t oa, uint32_t type, int initial) {
    uint64_t out = kf_ualloc(8), arg = (uint64_t)(initial ? 1u : 0u) | 0xAB00u;
    long r;

    kf_next_sp(stack_args(&arg, 1));
    r = nt(VIBEOS_NT_NR_NtCreateEvent, out, 0xFFFFFFFF00000000ull | access, oa,
           0xFFFFFFFF00000000ull | type, 0);
    *h = *(uint64_t *)kf_uptr(out);
    return r;
}

static int32_t event_state(uint64_t h) {
    uint64_t buf = kf_ualloc(8);
    kf_next_sp(stack_args(&g_zero5, 1));
    if (nt(VIBEOS_NT_NR_NtQueryEvent, h, 0, buf, 8, 0) != ST(VIBEOS_STATUS_SUCCESS)) {
        return -99;
    }
    return ((const vibeos_nt_event_basic_t *)kf_uptr(buf))->event_state;
}

static long wait1(uint64_t h, int64_t timeout) {
    uint64_t t = kf_ualloc(8);
    *(int64_t *)kf_uptr(t) = timeout;
    return nt(VIBEOS_NT_NR_NtWaitForSingleObject, h, 0, t, 0, 0);
}

static long waitn(const uint64_t *hs, uint32_t n, int all, int64_t timeout) {
    uint64_t arr = kf_ualloc(8u * n), t = kf_ualloc(8);
    uint64_t tptr = t;

    memcpy(kf_uptr(arr), hs, 8u * n);
    *(int64_t *)kf_uptr(t) = timeout;
    kf_next_sp(stack_args(&tptr, 1));
    return nt(VIBEOS_NT_NR_NtWaitForMultipleObjects, n, arr, all ? 0 : 1, 0, 0);
}

static void t_events(void) {
    uint64_t a, b, h[2];
    kf_outcome_t how;

    fresh(40);
    expect(create_event(&a, VIBEOS_EVENT_ALL_ACCESS, oattr(0, 0), VIBEOS_NT_SYNCHRONIZATION_EVENT,
                        0) == ST(VIBEOS_STATUS_SUCCESS) && a >= 16u && (a & 3u) == 0u,
           "an unnamed event: a handle past the standard ones, a multiple of four");
    expect(event_state(a) == 0, "it starts as asked: not signalled");
    expect(wait1(a, 0) == ST(VIBEOS_STATUS_TIMEOUT), "a zero timeout on an unsignalled event");
    expect(nt(VIBEOS_NT_NR_NtSetEvent, a, 0, 0, 0, 0) == ST(VIBEOS_STATUS_SUCCESS) &&
           event_state(a) == 1, "set");
    expect(wait1(a, 0) == ST(VIBEOS_STATUS_WAIT_0) && event_state(a) == 0,
           "a synchronization event is taken by the wait it satisfies");
    expect(wait1(a, -10000000) == ST(VIBEOS_STATUS_TIMEOUT),
           "a relative timeout ends the wait when it runs out");

    expect(create_event(&b, VIBEOS_EVENT_ALL_ACCESS, 0, VIBEOS_NT_NOTIFICATION_EVENT, 1) ==
               ST(VIBEOS_STATUS_SUCCESS), "a notification event, signalled, without attributes");
    expect(wait1(b, 0) == ST(VIBEOS_STATUS_WAIT_0) && wait1(b, 0) == ST(VIBEOS_STATUS_WAIT_0),
           "a notification event satisfies every wait until reset");
    h[0] = a;
    h[1] = b;
    expect(waitn(h, 2, 0, 0) == ST(VIBEOS_STATUS_WAIT_0 + 1u), "any: the index of the one signalled");
    expect(waitn(h, 2, 1, 0) == ST(VIBEOS_STATUS_TIMEOUT), "all: one unsignalled is a timeout");
    h[0] = b;
    h[1] = a;
    expect(waitn(h, 2, 1, 0) == ST(VIBEOS_STATUS_TIMEOUT) && event_state(b) == 1,
           "all: the first signalled is not enough, and nothing is acquired");
    h[0] = a;
    h[1] = b;
    (void)nt(VIBEOS_NT_NR_NtSetEvent, a, 0, 0, 0, 0);
    expect(waitn(h, 2, 1, 0) == ST(VIBEOS_STATUS_WAIT_0) && event_state(a) == 0 &&
               event_state(b) == 1, "all: both acquired at once, each as its type says");
    h[1] = a;
    expect(waitn(h, 2, 1, 0) == ST(0xC0000030u), "all, naming one object twice, is refused");
    expect(nt(VIBEOS_NT_NR_NtResetEvent, b, 0, 0, 0, 0) == ST(VIBEOS_STATUS_SUCCESS) &&
           nt(VIBEOS_NT_NR_NtPulseEvent, b, 0, 0, 0, 0) == ST(VIBEOS_STATUS_SUCCESS) &&
           event_state(b) == 0, "a pulse with nobody waiting leaves the event reset");
    (void)wait1(a, -1);   /* reset again; nothing */
    (void)nt(VIBEOS_NT_NR_NtWaitForSingleObject, a, 0, 0, 0, &how);
    expect(how == KF_BLOCKED, "no timeout on an event nobody will set waits for ever");
}

static long create_mutant(uint64_t *h, uint64_t oa, int owned) {
    uint64_t out = kf_ualloc(8);
    long r = nt(VIBEOS_NT_NR_NtCreateMutant, out, 0xFFFFFFFF00000000ull | VIBEOS_MUTANT_ALL_ACCESS,
                oa, (owned ? 1u : 0u) | 0xAB00u, 0);   /* a BOOLEAN: the low byte */
    *h = *(uint64_t *)kf_uptr(out);
    return r;
}

/* MutantBasicInformation, or -99 if the query failed; *owned set from it. */
static int32_t mutant_count(uint64_t h, int *owned) {
    uint64_t buf = kf_ualloc(8);
    const vibeos_nt_mutant_basic_t *m = (const vibeos_nt_mutant_basic_t *)kf_uptr(buf);

    kf_next_sp(stack_args(&g_zero5, 1));
    if (nt(VIBEOS_NT_NR_NtQueryMutant, h, 0, buf, 8, 0) != ST(VIBEOS_STATUS_SUCCESS)) {
        return -99;
    }
    *owned = m->owned_by_caller;
    return m->current_count;
}

static long release_mutant(uint64_t h, int32_t *prev) {
    uint64_t p = kf_ualloc(4);
    long r = nt(VIBEOS_NT_NR_NtReleaseMutant, h, p, 0, 0, 0);
    *prev = *(int32_t *)kf_uptr(p);
    return r;
}

static void t_mutants(void) {
    uint64_t m, other;
    int32_t prev = 7;
    int owned = 0;
    int a;

    a = fresh(50);
    expect(create_mutant(&m, oattr("\\BaseNamedObjects\\mu", 0), 1) == ST(VIBEOS_STATUS_SUCCESS),
           "a named mutant, owned by its creator");
    expect(mutant_count(m, &owned) == 0 && owned == 1, "held once: count 0, owned by the caller");
    expect(wait1(m, 0) == ST(VIBEOS_STATUS_WAIT_0) && mutant_count(m, &owned) == -1,
           "the owner takes it again without waiting: count -1");
    expect(release_mutant(m, &prev) == ST(VIBEOS_STATUS_SUCCESS) && prev == -1,
           "a release reports the count before it");
    expect(release_mutant(m, &prev) == ST(VIBEOS_STATUS_SUCCESS) && prev == 0 &&
               mutant_count(m, &owned) == 1 && owned == 0, "released as often as taken: free");
    expect(release_mutant(m, &prev) == ST(VIBEOS_STATUS_MUTANT_NOT_OWNED),
           "a free mutant cannot be released");
    expect(wait1(m, 0) == ST(VIBEOS_STATUS_WAIT_0) && mutant_count(m, &owned) == 0 && owned == 1,
           "a wait on a free mutant takes it");

    /* Another process, the same name: a different owner. */
    kf_set_current(kf_spawn(51, 51));
    expect(nt(VIBEOS_NT_NR_NtOpenMutant, kf_ualloc(8), VIBEOS_MUTANT_ALL_ACCESS,
              oattr("\\BaseNamedObjects\\mu", 0), 0, 0) == ST(VIBEOS_STATUS_SUCCESS),
           "opened by name from another process");
    expect(create_mutant(&other, oattr("\\BaseNamedObjects\\mu", 0), 1) ==
               ST(VIBEOS_STATUS_OBJECT_NAME_EXISTS) && mutant_count(other, &owned) == 0 &&
               owned == 0, "created again by name: the existing one, its owner unchanged");
    expect(wait1(other, 0) == ST(VIBEOS_STATUS_TIMEOUT), "held by somebody else: the wait times out");
    expect(release_mutant(other, &prev) == ST(VIBEOS_STATUS_MUTANT_NOT_OWNED),
           "only the owner releases");
    kf_set_current(a);
    expect(release_mutant(m, &prev) == ST(VIBEOS_STATUS_SUCCESS), "the owner releases it");
}

static long create_semaphore(uint64_t *h, int32_t initial, int32_t maximum) {
    uint64_t out = kf_ualloc(8), arg = 0xFFFFFFFF00000000ull | (uint32_t)maximum;
    long r;

    kf_next_sp(stack_args(&arg, 1));
    r = nt(VIBEOS_NT_NR_NtCreateSemaphore, out, VIBEOS_SEMAPHORE_ALL_ACCESS, 0,
           0xFFFFFFFF00000000ull | (uint32_t)initial, 0);
    *h = *(uint64_t *)kf_uptr(out);
    return r;
}

static int32_t semaphore_count(uint64_t h) {
    uint64_t buf = kf_ualloc(8);

    kf_next_sp(stack_args(&g_zero5, 1));
    if (nt(VIBEOS_NT_NR_NtQuerySemaphore, h, 0, buf, 8, 0) != ST(VIBEOS_STATUS_SUCCESS)) {
        return -99;
    }
    return ((const vibeos_nt_semaphore_basic_t *)kf_uptr(buf))->current_count;
}

static void t_semaphores(void) {
    uint64_t s, prev = kf_ualloc(4);

    fresh(52);
    expect(create_semaphore(&s, 3, 2) == ST(VIBEOS_STATUS_INVALID_PARAMETER),
           "an initial count above the maximum is refused");
    expect(create_semaphore(&s, 0, 0) == ST(VIBEOS_STATUS_INVALID_PARAMETER),
           "a maximum of zero is refused");
    expect(create_semaphore(&s, 1, 2) == ST(VIBEOS_STATUS_SUCCESS) && semaphore_count(s) == 1,
           "one of two");
    expect(wait1(s, 0) == ST(VIBEOS_STATUS_WAIT_0) && semaphore_count(s) == 0,
           "a wait takes one");
    expect(wait1(s, 0) == ST(VIBEOS_STATUS_TIMEOUT), "none left: the wait times out");
    expect(nt(VIBEOS_NT_NR_NtReleaseSemaphore, s, 0xFFFFFFFF00000003ull, prev, 0, 0) ==
               ST(VIBEOS_STATUS_SEMAPHORE_LIMIT_EXCEEDED) && semaphore_count(s) == 0,
           "a release past the maximum is refused and changes nothing");
    expect(nt(VIBEOS_NT_NR_NtReleaseSemaphore, s, 2, prev, 0, 0) == ST(VIBEOS_STATUS_SUCCESS) &&
               *(int32_t *)kf_uptr(prev) == 0 && semaphore_count(s) == 2,
           "a release up to the maximum, reporting the count before it");
    expect(nt(VIBEOS_NT_NR_NtReleaseSemaphore, s, 0, prev, 0, 0) ==
               ST(VIBEOS_STATUS_INVALID_PARAMETER), "a release of none is refused");
}

/* Sleep: a relative interval passes on the clock before the call returns. */
static void t_delay(void) {
    uint64_t t = kf_ualloc(8), before;

    fresh(53);
    *(int64_t *)kf_uptr(t) = -1500000;   /* 150 ms, 15 ticks at 100 Hz */
    before = ks_ticks();
    expect(nt(VIBEOS_NT_NR_NtDelayExecution, 0, t, 0, 0, 0) == ST(VIBEOS_STATUS_SUCCESS) &&
               ks_ticks() - before >= 15u, "a delay of 150 ms lets 15 ticks pass");
    *(int64_t *)kf_uptr(t) = 0;
    before = ks_ticks();
    expect(nt(VIBEOS_NT_NR_NtDelayExecution, 0, t, 0, 0, 0) == ST(VIBEOS_STATUS_SUCCESS) &&
               ks_ticks() - before <= 1u, "a delay of zero gives up a tick and no more");
    expect(nt(VIBEOS_NT_NR_NtDelayExecution, 0, 0, 0, 0, 0) == ST(VIBEOS_STATUS_ACCESS_VIOLATION),
           "no interval is a bad pointer");
}

static void t_handles_and_names(void) {
    uint64_t a, b, c, out = kf_ualloc(8), oa;
    long r;

    fresh(41);
    expect(create_event(&a, VIBEOS_EVENT_QUERY_STATE, 0, 0, 0) == ST(VIBEOS_STATUS_SUCCESS) &&
           nt(VIBEOS_NT_NR_NtSetEvent, a, 0, 0, 0, 0) == ST(VIBEOS_STATUS_ACCESS_DENIED),
           "a handle without EVENT_MODIFY_STATE cannot set");
    expect(wait1(a, 0) == ST(VIBEOS_STATUS_ACCESS_DENIED), "nor wait, without SYNCHRONIZE");
    expect(event_state(a) == 0, "but can query: it was granted that");
    expect(nt(VIBEOS_NT_NR_NtSetEvent, 8, 0, 0, 0, 0) == ST(VIBEOS_STATUS_OBJECT_TYPE_MISMATCH),
           "the console's handle is not an event");

    /* Duplicating: the same object, the rights asked for or the same ones. */
    kf_next_sp(stack_args((const uint64_t[3]){ VIBEOS_EVENT_ALL_ACCESS, 0, VIBEOS_DUPLICATE_SAME_ACCESS }, 3));
    expect(nt(VIBEOS_NT_NR_NtDuplicateObject, VIBEOS_NT_CURRENT_PROCESS, a,
              VIBEOS_NT_CURRENT_PROCESS, out, 0) == ST(VIBEOS_STATUS_SUCCESS) &&
           *(uint64_t *)kf_uptr(out) != a &&
           nt(VIBEOS_NT_NR_NtSetEvent, *(uint64_t *)kf_uptr(out), 0, 0, 0, 0) ==
               ST(VIBEOS_STATUS_ACCESS_DENIED), "SAME_ACCESS copies the rights, not more");
    kf_next_sp(stack_args((const uint64_t[3]){ VIBEOS_GENERIC_ALL, 0, VIBEOS_DUPLICATE_CLOSE_SOURCE }, 3));
    expect(nt(VIBEOS_NT_NR_NtDuplicateObject, VIBEOS_NT_CURRENT_PROCESS, a,
              VIBEOS_NT_CURRENT_PROCESS, out, 0) == ST(VIBEOS_STATUS_SUCCESS) &&
           nt(VIBEOS_NT_NR_NtSetEvent, *(uint64_t *)kf_uptr(out), 0, 0, 0, 0) ==
               ST(VIBEOS_STATUS_SUCCESS) &&
           nt(VIBEOS_NT_NR_NtClose, a, 0, 0, 0, 0) == ST(VIBEOS_STATUS_INVALID_HANDLE),
           "GENERIC_ALL maps to all of an event's rights, and CLOSE_SOURCE closed the source");
    expect(nt(VIBEOS_NT_NR_NtClose, VIBEOS_NT_CURRENT_PROCESS, 0, 0, 0, 0) ==
               ST(VIBEOS_STATUS_SUCCESS), "closing the process's pseudo-handle closes nothing");
    expect(nt(VIBEOS_NT_NR_NtClose, 6, 0, 0, 0, 0) == ST(VIBEOS_STATUS_INVALID_HANDLE),
           "a handle that is not a multiple of four");

    /* Names. */
    oa = oattr("\\BaseNamedObjects\\vibe", VIBEOS_OBJ_CASE_INSENSITIVE);
    expect(create_event(&b, VIBEOS_EVENT_ALL_ACCESS, oa, VIBEOS_NT_NOTIFICATION_EVENT, 0) ==
               ST(VIBEOS_STATUS_SUCCESS), "a named event");
    r = create_event(&c, VIBEOS_EVENT_ALL_ACCESS, oattr("\\BaseNamedObjects\\VIBE",
                                                        VIBEOS_OBJ_CASE_INSENSITIVE),
                     VIBEOS_NT_SYNCHRONIZATION_EVENT, 1);
    expect(r == ST(VIBEOS_STATUS_OBJECT_NAME_EXISTS) && c != b,
           "creating it again opens it, says it existed, and is another handle");
    (void)nt(VIBEOS_NT_NR_NtSetEvent, c, 0, 0, 0, 0);
    expect(event_state(b) == 1 && wait1(c, 0) == ST(VIBEOS_STATUS_WAIT_0) && event_state(b) == 1,
           "one object: set through one handle, seen through the other - still a notification event");
    expect(nt(VIBEOS_NT_NR_NtOpenEvent, out, VIBEOS_EVENT_ALL_ACCESS,
              oattr("\\BaseNamedObjects\\vibe", 0), 0, 0) == ST(VIBEOS_STATUS_SUCCESS),
           "opened by its name");
    expect(nt(VIBEOS_NT_NR_NtOpenEvent, out, VIBEOS_EVENT_ALL_ACCESS,
              oattr("\\BaseNamedObjects\\nobody", 0), 0, 0) ==
               ST(VIBEOS_STATUS_OBJECT_NAME_NOT_FOUND), "a name nothing has");
    expect(nt(VIBEOS_NT_NR_NtOpenEvent, out, VIBEOS_EVENT_ALL_ACCESS, oattr("relative", 0), 0, 0) ==
               ST(VIBEOS_STATUS_OBJECT_PATH_SYNTAX_BAD), "a name that is not a path");
    (void)nt(VIBEOS_NT_NR_NtClose, *(uint64_t *)kf_uptr(out), 0, 0, 0, 0);
    (void)nt(VIBEOS_NT_NR_NtClose, b, 0, 0, 0, 0);
    expect(nt(VIBEOS_NT_NR_NtOpenEvent, out, VIBEOS_EVENT_ALL_ACCESS,
              oattr("\\BaseNamedObjects\\vibe", 0), 0, 0) == ST(VIBEOS_STATUS_SUCCESS),
           "a name lives while any handle does");
    (void)nt(VIBEOS_NT_NR_NtClose, *(uint64_t *)kf_uptr(out), 0, 0, 0, 0);
    (void)nt(VIBEOS_NT_NR_NtClose, c, 0, 0, 0, 0);
    expect(nt(VIBEOS_NT_NR_NtOpenEvent, out, VIBEOS_EVENT_ALL_ACCESS,
              oattr("\\BaseNamedObjects\\vibe", 0), 0, 0) ==
               ST(VIBEOS_STATUS_OBJECT_NAME_NOT_FOUND), "and goes with the last");

    /* Case: a name made without OBJ_CASE_INSENSITIVE, asked for in another
     * case by a caller that did not ask for it either, is another name. */
    expect(create_event(&b, VIBEOS_EVENT_ALL_ACCESS, oattr("\\BaseNamedObjects\\Exact", 0),
                        0, 0) == ST(VIBEOS_STATUS_SUCCESS) &&
           nt(VIBEOS_NT_NR_NtOpenEvent, out, VIBEOS_EVENT_ALL_ACCESS,
              oattr("\\BaseNamedObjects\\EXACT", 0), 0, 0) ==
               ST(VIBEOS_STATUS_OBJECT_NAME_NOT_FOUND) &&
           nt(VIBEOS_NT_NR_NtOpenEvent, out, VIBEOS_EVENT_ALL_ACCESS,
              oattr("\\BaseNamedObjects\\EXACT", VIBEOS_OBJ_CASE_INSENSITIVE), 0, 0) ==
               ST(VIBEOS_STATUS_SUCCESS),
           "case matters unless somebody asked for it not to");
}

/* ---- files and image sections (W2) ----------------------------------------------- */

static long open_file(uint64_t *h, uint32_t access, const char *nt_path) {
    uint64_t out = kf_ualloc(8), iosb = kf_ualloc(16);
    uint64_t rest[2] = { 7u, 0x20u };   /* share access, open options */
    long r;

    kf_next_sp(stack_args(rest, 2));
    r = nt(VIBEOS_NT_NR_NtOpenFile, out, access, oattr(nt_path, VIBEOS_OBJ_CASE_INSENSITIVE),
           iosb, 0);
    *h = *(uint64_t *)kf_uptr(out);
    return r;
}

/* A Linux call, from the NT tests: to make files with owners and modes, which
 * NtCreateFile (W4) cannot yet. */
static long lsys(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2) {
    return kf_call(linux_syscall, nr, a0, a1, a2, 0, 0, 0, 0);
}

static uint64_t lstr(const char *s) {
    uint64_t u = kf_ualloc((uint32_t)strlen(s) + 1u);
    memcpy(kf_uptr(u), s, strlen(s) + 1u);
    return u;
}

/* NtOpenFile asks who is asking, as openat does (M-105). */
static void t_open_file_permissions(void) {
    uint64_t fh = 0;
    int me = fresh(44);

    expect(lsys(83 /* mkdir */, lstr("/tmp/sealed"), 0700, 0) == 0 &&
               lsys(85 /* creat */, lstr("/tmp/mine"), 0600, 0) >= 0 &&
               lsys(85, lstr("/tmp/open"), 0644, 0) >= 0,
           "root makes a closed directory, a private file and a readable one");
    ks_ps(me)->cred.euid = ks_ps(me)->cred.fsuid = 1000u;
    ks_ps(me)->cred.egid = ks_ps(me)->cred.fsgid = 1000u;
    expect(open_file(&fh, 0x00120089u, "\\??\\C:\\tmp\\open") == ST(VIBEOS_STATUS_SUCCESS),
           "another user opens what everybody may read");
    expect(open_file(&fh, 0x00120089u, "\\??\\C:\\tmp\\mine") == ST(VIBEOS_STATUS_ACCESS_DENIED),
           "but not root's 0600 file");
    expect(open_file(&fh, 0x00120089u, "\\??\\C:\\tmp\\sealed\\anything") ==
               ST(VIBEOS_STATUS_ACCESS_DENIED),
           "nor a path through a directory it may not search: not even whether it exists");
    expect(open_file(&fh, 0x001200A9u /* + FILE_EXECUTE */, "\\??\\C:\\tmp\\open") ==
               ST(VIBEOS_STATUS_ACCESS_DENIED),
           "and FILE_EXECUTE asks for execute permission, which 0644 does not give");
}

static long create_section(uint64_t *h, uint32_t attributes, uint64_t file) {
    uint64_t out = kf_ualloc(8);
    uint64_t rest[3] = { 0x02u /* PAGE_READONLY */, attributes, file };
    long r;

    kf_next_sp(stack_args(rest, 3));
    r = nt(VIBEOS_NT_NR_NtCreateSection, out, 0x000F001Fu, 0, 0, 0);
    *h = *(uint64_t *)kf_uptr(out);
    return r;
}

#ifdef VIBEOS_PE_FIXTURE_DIR
/* Only the fixture's part of t_sections maps a view: without mingw-w64 there is
 * no image to map, and the helper would be defined for nothing. */
static long map_view(uint64_t sec, uint64_t process, uint64_t *base, uint64_t *size) {
    uint64_t b = kf_ualloc(8), s = kf_ualloc(8);
    uint64_t rest[6] = { 0, 0, s, 1, 0, 0x02u };

    *(uint64_t *)kf_uptr(b) = 0;
    kf_next_sp(stack_args(rest, 6));
    {
        long r = nt(VIBEOS_NT_NR_NtMapViewOfSection, sec, process, b, 0, 0);
        *base = *(uint64_t *)kf_uptr(b);
        *size = *(uint64_t *)kf_uptr(s);
        return r;
    }
}
#endif

static void t_sections(void) {
    uint64_t fh, sh;

    fresh(42);
    kf_fs_add("/win", 0, 0, 1);
    kf_fs_add("/win/text.txt", "not an image", 12, 0);
    expect(open_file(&fh, 0x00120089u, "\\??\\C:\\win\\nothing.dll") ==
               ST(VIBEOS_STATUS_OBJECT_NAME_NOT_FOUND), "a file that is not there");
    expect(open_file(&fh, 0x00120089u, "\\??\\D:\\win\\text.txt") ==
               ST(VIBEOS_STATUS_OBJECT_PATH_NOT_FOUND), "a drive that is not there");
    expect(open_file(&fh, 0x00120089u, "C:\\win\\text.txt") ==
               ST(VIBEOS_STATUS_OBJECT_PATH_SYNTAX_BAD), "a DOS path is not an NT path");
    expect(open_file(&fh, 0x00120089u, "\\BaseNamedObjects\\text.txt") ==
               ST(VIBEOS_STATUS_OBJECT_PATH_SYNTAX_BAD),
           "an NT path outside \\??\\ names no file W2 can open");
    expect(open_file(&fh, VIBEOS_GENERIC_WRITE, "\\??\\C:\\win\\text.txt") ==
               ST(VIBEOS_STATUS_NOT_IMPLEMENTED), "writing is W4's: refused, not opened read-only");
    expect(open_file(&fh, 0x00120089u, "\\??\\C:\\win\\text.txt") == ST(VIBEOS_STATUS_SUCCESS) &&
           fh >= 16u, "a file opened to read, by its NT path");
    expect(create_section(&sh, 0x08000000u /* SEC_COMMIT */, fh) ==
               ST(VIBEOS_STATUS_NOT_IMPLEMENTED), "a data section is W3's");
    expect(create_section(&sh, 0x01000000u, fh) == ST(VIBEOS_STATUS_INVALID_IMAGE_FORMAT),
           "an image section of a file that is no image is refused at creation");
    expect(create_section(&sh, 0x01000000u, 8) == ST(VIBEOS_STATUS_OBJECT_TYPE_MISMATCH),
           "the console is not a file to map");
#ifdef VIBEOS_PE_FIXTURE_DIR
    {
        uint64_t base = 0, size = 0;
        char path[512];
        FILE *f;
        static uint8_t img[1u << 17];
        size_t n;

        snprintf(path, sizeof(path), "%s/pe_fixture.exe", VIBEOS_PE_FIXTURE_DIR);
        f = fopen(path, "rb");
        n = f ? fread(img, 1, sizeof(img), f) : 0;
        if (f) {
            fclose(f);
        }
        expect(n > 0u, "the mingw fixture is there to map");
        kf_fs_add("/win/fx.dll", img, (uint32_t)n, 0);
        {
            long ro = open_file(&fh, 0x00120089u, "\\??\\C:\\win\\fx.dll");
            long rc = create_section(&sh, 0x01000000u, fh);
            if (ro != ST(VIBEOS_STATUS_SUCCESS) || rc != ST(VIBEOS_STATUS_SUCCESS)) {
                printf("nt_abi: open %lx section %lx (fixture %zu bytes)\n", ro, rc, n);
            }
            expect(ro == ST(VIBEOS_STATUS_SUCCESS) && rc == ST(VIBEOS_STATUS_SUCCESS),
                   "an image section over mingw's image");
        }
        expect(nt(VIBEOS_NT_NR_NtClose, fh, 0, 0, 0, 0) == ST(VIBEOS_STATUS_SUCCESS),
               "the file's handle can go: the section holds the file");
        expect(map_view(sh, VIBEOS_NT_CURRENT_PROCESS, &base, &size) == ST(0x40000003u) &&
                   base >= 0x8003000000ull && size >= 0x6000u && (base & 0xFFFFu) == 0u,
               "mapped where it can be, said moved, at a 64 KiB boundary");
        expect(map_view(sh, 4, &base, &size) == ST(VIBEOS_STATUS_INVALID_HANDLE),
               "into another process: W6's");
        expect(nt(VIBEOS_NT_NR_NtClose, sh, 0, 0, 0, 0) == ST(VIBEOS_STATUS_SUCCESS),
               "and the section closes");

        /* Another file at the name between the section and the map (M-106):
         * the kernel reads images by path, and must not read that one. The old
         * file goes, something else takes its slot, and a new file is put at
         * the name - a different node behind the same path. */
        {
            uint64_t fh2 = 0, sh2 = 0, fh3 = 0, sh3 = 0;

            expect(open_file(&fh2, 0x00120089u, "\\??\\C:\\win\\fx.dll") == ST(VIBEOS_STATUS_SUCCESS) &&
                       create_section(&sh2, 0x01000000u, fh2) == ST(VIBEOS_STATUS_SUCCESS) &&
                       open_file(&fh3, 0x00120089u, "\\??\\C:\\win\\fx.dll") == ST(VIBEOS_STATUS_SUCCESS),
                   "a section of the image, and a second handle to it");
            expect(lsys(87 /* unlink */, lstr("/win/fx.dll"), 0, 0) == 0, "the image is unlinked");
            kf_fs_add("/win/filler", "x", 1, 0);
            kf_fs_add("/win/fx.dll", img, (uint32_t)n, 0);
            expect(map_view(sh2, VIBEOS_NT_CURRENT_PROCESS, &base, &size) ==
                       ST(VIBEOS_STATUS_FILE_RENAMED),
                   "a section whose name now names another file is not mapped from it");
            expect(create_section(&sh3, 0x01000000u, fh3) == ST(VIBEOS_STATUS_FILE_RENAMED),
                   "nor is a section made of it through a handle to the old one");
        }
    }
#endif
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint64_t rd64(const uint8_t *p) {
    uint64_t v = 0;
    int i;
    for (i = 7; i >= 0; i--) {
        v = (v << 8) | p[i];
    }
    return v;
}

/* The UNICODE_STRING at block offset `field`, compared with `ascii`. */
static int ustr_is(uint8_t *const *pages, uint64_t base, uint32_t field, const char *ascii) {
    uint8_t *blk = pages[field / 4096u] + field % 4096u;
    uint16_t len = rd16(blk + VIBEOS_USTR_LENGTH);
    uint64_t at = rd64(blk + VIBEOS_USTR_BUFFER) - base;
    uint32_t i;

    if (len != 2u * strlen(ascii) || rd16(blk + VIBEOS_USTR_MAXIMUM) != len + 2u) {
        return 0;
    }
    for (i = 0; i < len / 2u; i++) {
        uint32_t o = (uint32_t)at + 2u * i;
        if (rd16(pages[o / 4096u] + o % 4096u) != (uint8_t)ascii[i]) {
            return 0;
        }
    }
    return 1;
}

static void t_blocks(void) {
    static uint8_t mem[VIBEOS_NT_BLOCK_PAGES][4096];
    uint8_t *pages[VIBEOS_NT_BLOCK_PAGES];
    const char *argv[] = { "C:\\a b.exe", "x", 0 };
    /* Larger than the room the parameters' two pages leave, by less than a
     * page: a bound that is off by a page must not accept it. */
    static char longarg[4001];
    const char *big[] = { longarg, 0 };
    const uint64_t base = 0x8003FF0000ull;
    vibeos_nt_start_t s;
    uint8_t *teb, *peb, *rupp;
    uint32_t i;

    for (i = 0; i < VIBEOS_NT_BLOCK_PAGES; i++) {
        pages[i] = mem[i];
    }
    memset(mem, 0, sizeof(mem));
    memset(&s, 0, sizeof(s));
    s.image_base = 0x8000000000ull;
    s.stack_base = 0x8000400000ull;
    s.stack_limit = 0x8000201000ull;
    s.pid = 7;
    s.tid = 8;
    s.processors = 4;
    s.std_handle[0] = 4;
    s.std_handle[1] = 8;
    s.std_handle[2] = 12;
    s.image_path = "/win/hello.exe";
    s.argv = argv;
    expect(vibeos_nt_build_blocks(pages, base, &s) == 0, "the blocks are built");
    teb = mem[VIBEOS_NT_BLOCK_TEB / 4096u];
    peb = mem[VIBEOS_NT_BLOCK_PEB / 4096u];
    rupp = mem[VIBEOS_NT_BLOCK_PARAMS / 4096u];
    expect(rd64(teb + VIBEOS_TEB_SELF) == base + VIBEOS_NT_BLOCK_TEB &&
           rd64(teb + VIBEOS_TEB_PEB) == base + VIBEOS_NT_BLOCK_PEB &&
           rd64(teb + VIBEOS_TEB_STACK_BASE) == s.stack_base &&
           rd64(teb + VIBEOS_TEB_CLIENT_PROCESS) == 7u && rd64(teb + VIBEOS_TEB_CLIENT_THREAD) == 8u,
           "the TEB: itself, the PEB, the stack, who it is");
    expect(rd64(peb + VIBEOS_PEB_IMAGE_BASE) == s.image_base &&
           rd64(peb + VIBEOS_PEB_PROCESS_PARAMS) == base + VIBEOS_NT_BLOCK_PARAMS &&
           rd16(peb + VIBEOS_PEB_OS_BUILD) == 19045u &&
           rd16(peb + VIBEOS_PEB_SESSION_ID) == 1u && rd16(peb + VIBEOS_PEB_SESSION_ID + 2u) == 0u,
           "the PEB: the program, the parameters, Windows 10, session 1");
    expect(rd64(rupp + VIBEOS_RUPP_STD_INPUT) == 4u && rd64(rupp + VIBEOS_RUPP_STD_OUTPUT) == 8u &&
           rd64(rupp + VIBEOS_RUPP_STD_ERROR) == 12u, "the standard handles");
    expect(ustr_is(pages, base, VIBEOS_NT_BLOCK_PARAMS + VIBEOS_RUPP_IMAGE_PATH, "/win/hello.exe"),
           "the image path, in UTF-16");
    expect(ustr_is(pages, base, VIBEOS_NT_BLOCK_PARAMS + VIBEOS_RUPP_COMMAND_LINE,
                   "\"C:\\a b.exe\" x"),
           "the command line: arguments joined, the one with a space quoted");
    memset(longarg, 'a', sizeof(longarg) - 1u);
    s.argv = big;
    expect(vibeos_nt_build_blocks(pages, base, &s) != 0,
           "arguments larger than the parameters' pages are refused");
}

int test_nt_abi(void) {
    g_fail = 0;
    t_registry();
    t_dispatch_refusals();
    t_write_file();
    t_process();
    t_system();
    t_blocks();
    t_events();
    t_mutants();
    t_semaphores();
    t_delay();
    t_handles_and_names();
    t_sections();
    t_open_file_permissions();
    return g_fail ? -1 : 0;
}
