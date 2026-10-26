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
    expect(nt(VIBEOS_NT_NR_NtWriteFile, 8, 0x40, 0, 0, 0) == ST(VIBEOS_STATUS_INVALID_HANDLE),
           "an event that is no event's handle is refused before anything is written (W4)");
    rest[3] = off;
    kf_next_sp(stack_args(rest, 5));
    expect(nt(VIBEOS_NT_NR_NtWriteFile, 8, 0, 0, 0, 0) == ST(VIBEOS_STATUS_INVALID_PARAMETER) &&
           con_count("nt-he") == 1, "an offset on the console, which has none: refused, nothing written (W4)");
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
/* Set, oattr spells three characters beyond ASCII (W4 step 6): '#' is U+00E9,
 * '%' is U+1F600 as its surrogate pair, '!' is half a pair alone. */
static int g_oattr_wide;

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
        if (g_oattr_wide) {
            uint32_t o = 0;

            buf = kf_ualloc(4u * n + 2u);
            w = (uint16_t *)kf_uptr(buf);
            for (i = 0; i < n; i++) {
                if (ascii[i] == '#') {
                    w[o++] = 0x00E9u;
                } else if (ascii[i] == '%') {
                    w[o++] = 0xD83Du;
                    w[o++] = 0xDE00u;
                } else if (ascii[i] == '!') {
                    w[o++] = 0xD800u;
                } else {
                    w[o++] = (uint8_t)ascii[i];
                }
            }
            n = o;
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

/* NtCreateFile: the handle, and what the status block's Information said. */
static long create_file(uint64_t *h, uint64_t *info, uint32_t access, const char *nt_path,
                        uint32_t disposition, uint32_t options) {
    uint64_t out = kf_ualloc(8), iosb = kf_ualloc(16);
    uint64_t rest[7] = { 0, 0, 7u, disposition, options, 0, 0 };
    long r;

    kf_next_sp(stack_args(rest, 7));
    r = nt(VIBEOS_NT_NR_NtCreateFile, out, access, oattr(nt_path, VIBEOS_OBJ_CASE_INSENSITIVE), iosb, 0);
    *h = *(uint64_t *)kf_uptr(out);
    *info = ((uint64_t *)kf_uptr(iosb))[1];
    return r;
}

/* W4 step 1: the dispositions, the options, and what each says it did. */
static void t_create_file(void) {
    const uint32_t rw = VIBEOS_GENERIC_READ | VIBEOS_GENERIC_WRITE;
    uint64_t h = 0, info = 9;

    fresh(43);
    kf_fs_add("/w4", 0, 0, 1);
    expect(create_file(&h, &info, rw, "\\??\\C:\\w4\\a.txt", 2, 0x40) == ST(VIBEOS_STATUS_SUCCESS) &&
               info == 2u && nt(VIBEOS_NT_NR_NtClose, h, 0, 0, 0, 0) == ST(VIBEOS_STATUS_SUCCESS),
           "FILE_CREATE makes a file, and says FILE_CREATED");
    expect(create_file(&h, &info, rw, "\\??\\C:\\w4\\a.txt", 2, 0) == ST(VIBEOS_STATUS_OBJECT_NAME_COLLISION),
           "FILE_CREATE of a name that exists is a collision");
    expect(create_file(&h, &info, rw, "\\??\\C:\\w4\\none.txt", 1, 0) ==
               ST(VIBEOS_STATUS_OBJECT_NAME_NOT_FOUND) &&
           create_file(&h, &info, rw, "\\??\\C:\\w4\\none.txt", 4, 0) ==
               ST(VIBEOS_STATUS_OBJECT_NAME_NOT_FOUND),
           "FILE_OPEN and FILE_OVERWRITE want the file to exist");
    expect(create_file(&h, &info, rw, "\\??\\C:\\w4\\a.txt", 3, 0) == ST(VIBEOS_STATUS_SUCCESS) && info == 1u &&
               nt(VIBEOS_NT_NR_NtClose, h, 0, 0, 0, 0) == ST(VIBEOS_STATUS_SUCCESS) &&
           create_file(&h, &info, rw, "\\??\\C:\\w4\\a.txt", 5, 0) == ST(VIBEOS_STATUS_SUCCESS) && info == 3u &&
               nt(VIBEOS_NT_NR_NtClose, h, 0, 0, 0, 0) == ST(VIBEOS_STATUS_SUCCESS) &&
           create_file(&h, &info, rw, "\\??\\C:\\w4\\a.txt", 0, 0) == ST(VIBEOS_STATUS_SUCCESS) && info == 0u &&
               nt(VIBEOS_NT_NR_NtClose, h, 0, 0, 0, 0) == ST(VIBEOS_STATUS_SUCCESS),
           "FILE_OPEN_IF opens, FILE_OVERWRITE_IF overwrites, FILE_SUPERSEDE supersedes");
    expect(create_file(&h, &info, VIBEOS_GENERIC_READ, "\\??\\C:\\w4\\d", 2, 0x1) == ST(VIBEOS_STATUS_SUCCESS) &&
               info == 2u && nt(VIBEOS_NT_NR_NtClose, h, 0, 0, 0, 0) == ST(VIBEOS_STATUS_SUCCESS),
           "FILE_DIRECTORY_FILE with FILE_CREATE makes a directory");
    expect(create_file(&h, &info, VIBEOS_GENERIC_READ, "\\??\\C:\\w4\\d", 1, 0x40) ==
               ST(VIBEOS_STATUS_FILE_IS_A_DIRECTORY) &&
           create_file(&h, &info, VIBEOS_GENERIC_READ, "\\??\\C:\\w4\\a.txt", 1, 0x1) ==
               ST(VIBEOS_STATUS_NOT_A_DIRECTORY),
           "a directory is not a non-directory file, and a file is not a directory");
    expect(create_file(&h, &info, rw, "\\??\\C:\\w4\\b.txt", 6, 0) == ST(VIBEOS_STATUS_INVALID_PARAMETER) &&
           create_file(&h, &info, rw, "\\??\\C:\\w4\\b.txt", 2, 0x41) == ST(VIBEOS_STATUS_INVALID_PARAMETER) &&
           create_file(&h, &info, rw, "\\??\\C:\\w4\\b.txt", 2, 0x1000) == ST(VIBEOS_STATUS_INVALID_PARAMETER),
           "an unknown disposition, both kinds asked, DELETE_ON_CLOSE without DELETE");
}

/* NtReadFile or NtWriteFile of `len` bytes at `buf`, at *offset when `offset`
 * is not null, with `event` for completion. The status, and the count. */
static long rw_file(int write, uint64_t h, uint64_t event, uint64_t buf, uint32_t len,
                    const uint64_t *offset, uint64_t *count) {
    uint64_t iosb = kf_ualloc(16), off = 0;
    uint64_t rest[5];
    long r;

    if (offset) {
        off = kf_ualloc(8);
        *(uint64_t *)kf_uptr(off) = *offset;
    }
    rest[0] = iosb;
    rest[1] = buf;
    rest[2] = len;
    rest[3] = off;
    rest[4] = 0;
    kf_next_sp(stack_args(rest, 5));
    r = nt(write ? VIBEOS_NT_NR_NtWriteFile : VIBEOS_NT_NR_NtReadFile, h, event, 0, 0, 0);
    *count = ((uint64_t *)kf_uptr(iosb))[1];
    return r;
}

/* W4 step 2: offsets, the two that are not, the end of the file, events. */
static void t_read_write_file(void) {
    const uint32_t rw = VIBEOS_GENERIC_READ | VIBEOS_GENERIC_WRITE;
    uint64_t h = 0, info = 0, n = 0, ev = 0, buf = kf_ualloc(32);
    const uint64_t at4 = 4, end = 0xFFFFFFFFFFFFFFFFull, cur = 0xFFFFFFFFFFFFFFFEull, past = 100;
    char *b = (char *)kf_uptr(buf);

    fresh(44);
    kf_fs_add("/w4", 0, 0, 1);
    expect(create_file(&h, &info, rw, "\\??\\C:\\w4\\rw.txt", 2, 0x40) == ST(VIBEOS_STATUS_SUCCESS),
           "a file to read and write");
    memcpy(b, "abcdefgh", 8);
    expect(rw_file(1, h, 0, buf, 8, 0, &n) == ST(VIBEOS_STATUS_SUCCESS) && n == 8u, "eight bytes at the position");
    memcpy(b, "XY", 2);
    expect(rw_file(1, h, 0, buf, 2, &at4, &n) == ST(VIBEOS_STATUS_SUCCESS) && n == 2u &&
               rw_file(1, h, 0, buf, 2, &end, &n) == ST(VIBEOS_STATUS_SUCCESS),
           "two at offset four, two at the end");
    memset(b, 0, 32);
    expect(rw_file(0, h, 0, buf, 32, &(uint64_t){0}, &n) == ST(VIBEOS_STATUS_SUCCESS) && n == 10u &&
               memcmp(b, "abcdXYghXY", 10) == 0,
           "read back from offset zero: the file is what the writes made it");
    expect(rw_file(0, h, 0, buf, 4, &cur, &n) == ST(VIBEOS_STATUS_END_OF_FILE) && n == 0u &&
               rw_file(0, h, 0, buf, 4, &past, &n) == ST(VIBEOS_STATUS_END_OF_FILE),
           "at the position, now the end, and past the end: STATUS_END_OF_FILE");
    expect(create_event(&ev, 0x1F0003u, 0, 0, 0) == ST(VIBEOS_STATUS_SUCCESS) &&
               rw_file(0, h, ev, buf, 2, &at4, &n) == ST(VIBEOS_STATUS_SUCCESS) && event_state(ev) == 1 &&
               memcmp(b, "XY", 2) == 0,
           "the event is set when the read completes");
}

/* NtQueryInformationFile or NtSetInformationFile with class `cls` on `len`
 * bytes at `buf`. The status, and the status block's count. */
static long file_info(int set, uint64_t h, uint64_t buf, uint32_t len, uint32_t cls, uint64_t *count) {
    uint64_t iosb = kf_ualloc(16), rest[1] = { cls };
    long r;

    kf_next_sp(stack_args(rest, 1));
    r = nt(set ? VIBEOS_NT_NR_NtSetInformationFile : VIBEOS_NT_NR_NtQueryInformationFile, h, iosb, buf, len, 0);
    *count = ((uint64_t *)kf_uptr(iosb))[1];
    return r;
}

/* W4 step 3: what a handle says of its file, and what may be set through it. */
static void t_file_information(void) {
    const uint32_t rw = VIBEOS_GENERIC_READ | VIBEOS_GENERIC_WRITE | VIBEOS_DELETE;
    uint64_t h = 0, h2 = 0, info = 0, n = 0, buf = kf_ualloc(256);
    uint8_t *b = (uint8_t *)kf_uptr(buf);
    static const char to[] = "\\??\\C:\\tmp\\moved.txt";
    uint32_t i;

    fresh(45);
    expect(create_file(&h, &info, rw, "\\??\\C:\\tmp\\info.txt", 2, 0x40) == ST(VIBEOS_STATUS_SUCCESS),
           "a file to ask about");
    memcpy(b, "0123456789", 10);
    (void)rw_file(1, h, 0, buf, 10, 0, &n);
    memset(b, 0xEE, 256);
    expect(file_info(0, h, buf, 24, 5, &n) == ST(VIBEOS_STATUS_SUCCESS) && n == 24u &&
               *(uint64_t *)(b + 8) == 10u && b[21] == 0u,
           "FileStandardInformation: ten bytes at the end of the file, not a directory");
    expect(file_info(0, h, buf, 8, 14, &n) == ST(VIBEOS_STATUS_SUCCESS) && *(uint64_t *)b == 10u,
           "FilePositionInformation: after the ten written");
    expect(file_info(0, h, buf, 23, 5, &n) == ST(VIBEOS_STATUS_INFO_LENGTH_MISMATCH) &&
               file_info(0, h, buf, 64, 99, &n) == ST(VIBEOS_STATUS_INVALID_INFO_CLASS),
           "a buffer short of the class is a length mismatch, an unknown class invalid");
    /* The fake's clock may stand at zero, which is no time: 0, as Windows has
     * it, or a time since 1601 - never a raw count of nanoseconds. */
    expect(file_info(0, h, buf, 40, 4, &n) == ST(VIBEOS_STATUS_SUCCESS) &&
               (*(uint32_t *)(b + 32) & ~1u) == 0x20u &&
               (*(uint64_t *)(b + 16) == 0u || *(uint64_t *)(b + 16) >= 116444736000000000ull),
           "FileBasicInformation: an archive file, its write time since 1601");
    expect(file_info(0, h, buf, 8, 9, &n) == ST(VIBEOS_STATUS_BUFFER_OVERFLOW) && n == 8u &&
               *(uint32_t *)b == 2u * 13u,
           "FileNameInformation too small: the length, the start, and BUFFER_OVERFLOW");
    expect(file_info(0, h, buf, 64, 9, &n) == ST(VIBEOS_STATUS_SUCCESS) && n == 4u + 26u &&
               b[4] == '\\' && b[6] == 't' && b[12] == '\\',
           "and whole: \\tmp\\info.txt from the volume's root");
    *(uint64_t *)b = 4;
    expect(file_info(1, h, buf, 8, 20, &n) == ST(VIBEOS_STATUS_SUCCESS) &&
               file_info(0, h, buf, 24, 5, &n) == ST(VIBEOS_STATUS_SUCCESS) && *(uint64_t *)(b + 8) == 4u,
           "FileEndOfFileInformation cuts the file to four bytes");
    *(uint64_t *)b = 1;
    expect(file_info(1, h, buf, 8, 14, &n) == ST(VIBEOS_STATUS_SUCCESS) &&
               rw_file(0, h, 0, buf, 3, 0, &n) == ST(VIBEOS_STATUS_SUCCESS) && n == 3u && memcmp(b, "123", 3) == 0,
           "FilePositionInformation moves where the next read starts");
    memset(b, 0, 256);
    b[0] = 0;   /* ReplaceIfExists: no */
    *(uint32_t *)(b + 16) = (uint32_t)(2u * strlen(to));
    for (i = 0; to[i]; i++) {
        b[20u + 2u * i] = (uint8_t)to[i];
    }
    expect(file_info(1, h, buf, 20u + 2u * (uint32_t)strlen(to), 10, &n) == ST(VIBEOS_STATUS_SUCCESS) &&
               create_file(&h2, &info, rw, "\\??\\C:\\tmp\\moved.txt", 1, 0) == ST(VIBEOS_STATUS_SUCCESS) &&
               nt(VIBEOS_NT_NR_NtClose, h2, 0, 0, 0, 0) == ST(VIBEOS_STATUS_SUCCESS) &&
               create_file(&h2, &info, rw, "\\??\\C:\\tmp\\info.txt", 1, 0) ==
                   ST(VIBEOS_STATUS_OBJECT_NAME_NOT_FOUND),
           "FileRenameInformation moves the file to its new name");
    b[0] = 1;
    expect(file_info(1, h, buf, 1, 13, &n) == ST(VIBEOS_STATUS_SUCCESS) &&
               nt(VIBEOS_NT_NR_NtClose, h, 0, 0, 0, 0) == ST(VIBEOS_STATUS_SUCCESS) &&
               create_file(&h2, &info, rw, "\\??\\C:\\tmp\\moved.txt", 1, 0) ==
                   ST(VIBEOS_STATUS_OBJECT_NAME_NOT_FOUND),
           "FileDispositionInformation: the file goes with its last handle");
}

/* W4 step 4: attributes by name, deletion by name, the volume, the cancels. */
static void t_small_file_services(void) {
    const uint32_t rw = VIBEOS_GENERIC_READ | VIBEOS_GENERIC_WRITE;
    uint64_t h = 0, info = 0, n = 0, buf = kf_ualloc(64), iosb = kf_ualloc(16), rest[1];
    uint8_t *b = (uint8_t *)kf_uptr(buf);

    fresh(46);
    expect(create_file(&h, &info, rw, "\\??\\C:\\tmp\\sv.txt", 2, 0x40) == ST(VIBEOS_STATUS_SUCCESS) &&
               rw_file(1, h, 0, buf, 5, 0, &n) == ST(VIBEOS_STATUS_SUCCESS),
           "a five-byte file");
    expect(nt(VIBEOS_NT_NR_NtQueryFullAttributesFile, oattr("\\??\\C:\\tmp\\sv.txt", VIBEOS_OBJ_CASE_INSENSITIVE),
              buf, 0, 0, 0) == ST(VIBEOS_STATUS_SUCCESS) &&
               *(uint64_t *)(b + 40) == 5u && (*(uint32_t *)(b + 48) & 0x10u) == 0u,
           "NtQueryFullAttributesFile: its size, and not a directory");
    expect(nt(VIBEOS_NT_NR_NtQueryAttributesFile, oattr("\\??\\C:\\tmp", VIBEOS_OBJ_CASE_INSENSITIVE),
              buf, 0, 0, 0) == ST(VIBEOS_STATUS_SUCCESS) && (*(uint32_t *)(b + 32) & 0x10u) != 0u,
           "NtQueryAttributesFile: a directory says so");
    expect(nt(VIBEOS_NT_NR_NtFlushBuffersFile, h, iosb, 0, 0, 0) == ST(VIBEOS_STATUS_SUCCESS) &&
               nt(VIBEOS_NT_NR_NtCancelIoFile, h, iosb, 0, 0, 0) == ST(VIBEOS_STATUS_SUCCESS) &&
               nt(VIBEOS_NT_NR_NtCancelIoFileEx, h, 0, iosb, 0, 0) == ST(VIBEOS_STATUS_NOT_FOUND),
           "a flush; a cancel finds nothing pending, one asked by request finds nothing");
    rest[0] = 4;   /* FileFsDeviceInformation */
    kf_next_sp(stack_args(rest, 1));
    expect(nt(VIBEOS_NT_NR_NtQueryVolumeInformationFile, h, iosb, buf, 8, 0) == ST(VIBEOS_STATUS_SUCCESS) &&
               *(uint32_t *)b == 7u,
           "the volume is a disk");
    expect(nt(VIBEOS_NT_NR_NtClose, h, 0, 0, 0, 0) == ST(VIBEOS_STATUS_SUCCESS) &&
               nt(VIBEOS_NT_NR_NtDeleteFile, oattr("\\??\\C:\\tmp\\sv.txt", VIBEOS_OBJ_CASE_INSENSITIVE),
                  0, 0, 0, 0) == ST(VIBEOS_STATUS_SUCCESS) &&
               nt(VIBEOS_NT_NR_NtQueryAttributesFile, oattr("\\??\\C:\\tmp\\sv.txt", VIBEOS_OBJ_CASE_INSENSITIVE),
                  buf, 0, 0, 0) == ST(VIBEOS_STATUS_OBJECT_NAME_NOT_FOUND),
           "NtDeleteFile takes the name at once");
    expect(create_file(&h, &info, VIBEOS_GENERIC_READ, "\\??\\C:\\tmp\\dd", 2, 0x1) == ST(VIBEOS_STATUS_SUCCESS) &&
               nt(VIBEOS_NT_NR_NtClose, h, 0, 0, 0, 0) == ST(VIBEOS_STATUS_SUCCESS) &&
               create_file(&h, &info, rw, "\\??\\C:\\tmp\\dd\\in", 2, 0x40) == ST(VIBEOS_STATUS_SUCCESS) &&
               nt(VIBEOS_NT_NR_NtClose, h, 0, 0, 0, 0) == ST(VIBEOS_STATUS_SUCCESS) &&
               nt(VIBEOS_NT_NR_NtDeleteFile, oattr("\\??\\C:\\tmp\\dd", VIBEOS_OBJ_CASE_INSENSITIVE),
                  0, 0, 0, 0) == ST(VIBEOS_STATUS_DIRECTORY_NOT_EMPTY),
           "a directory with something in it is not deleted");
}

/* NtQueryDirectoryFile on `h` into `len` bytes at `buf`, class `cls`, with
 * `mask` (0: none). The status, and the status block's count. */
static long query_dir(uint64_t h, uint64_t buf, uint32_t len, uint32_t cls, int single, const char *mask,
                      int restart, uint64_t *count) {
    uint64_t iosb = kf_ualloc(16), us = 0, rest[7];
    long r;

    if (mask) {
        uint64_t chars = kf_ualloc(128);
        uint32_t i;

        us = kf_ualloc(16);
        for (i = 0; mask[i]; i++) {
            ((uint16_t *)kf_uptr(chars))[i] = (uint8_t)mask[i];
        }
        ((uint16_t *)kf_uptr(us))[0] = (uint16_t)(2u * i);
        ((uint16_t *)kf_uptr(us))[1] = (uint16_t)(2u * i);
        ((uint64_t *)kf_uptr(us))[1] = chars;
    }
    rest[0] = iosb;
    rest[1] = buf;
    rest[2] = len;
    rest[3] = cls;
    rest[4] = (uint64_t)single;
    rest[5] = us;
    rest[6] = (uint64_t)restart;
    kf_next_sp(stack_args(rest, 7));
    r = nt(VIBEOS_NT_NR_NtQueryDirectoryFile, h, 0, 0, 0, 0);
    *count = ((uint64_t *)kf_uptr(iosb))[1];
    return r;
}

/* The names in a buffer of FILE_NAMES_INFORMATION, joined by ','. */
static void dir_names(const uint8_t *b, char *out) {
    uint32_t at = 0, n = 0, k;

    for (;;) {
        uint32_t len = *(const uint32_t *)(b + at + 8);

        for (k = 0; k < len / 2u; k++) {
            out[n++] = (char)b[at + 12u + 2u * k];
        }
        out[n++] = ',';
        if (*(const uint32_t *)(b + at) == 0u) {
            break;
        }
        at += *(const uint32_t *)(b + at);
    }
    out[n] = 0;
}

/* W4 step 5: listing a directory - masks, classes, restart, one at a time. */
static void t_query_directory(void) {
    const uint32_t rw = VIBEOS_GENERIC_READ | VIBEOS_GENERIC_WRITE;
    uint64_t h = 0, f = 0, info = 0, n = 0, buf = kf_ualloc(512);
    uint8_t *b = (uint8_t *)kf_uptr(buf);
    char names[256];

    fresh(47);
    expect(create_file(&h, &info, VIBEOS_GENERIC_READ, "\\??\\C:\\tmp\\ls", 2, 0x1) == ST(VIBEOS_STATUS_SUCCESS),
           "a directory to list");
    (void)create_file(&f, &info, rw, "\\??\\C:\\tmp\\ls\\a.txt", 2, 0x40);
    (void)rw_file(1, f, 0, buf, 7, 0, &n);
    (void)nt(VIBEOS_NT_NR_NtClose, f, 0, 0, 0, 0);
    (void)create_file(&f, &info, rw, "\\??\\C:\\tmp\\ls\\b.log", 2, 0x40);
    (void)nt(VIBEOS_NT_NR_NtClose, f, 0, 0, 0, 0);
    (void)create_file(&f, &info, rw, "\\??\\C:\\tmp\\ls\\c.TXT", 2, 0x40);
    (void)nt(VIBEOS_NT_NR_NtClose, f, 0, 0, 0, 0);
    memset(b, 0xEE, 512);
    expect(query_dir(h, buf, 512, 12, 0, "*.txt", 0, &n) == ST(VIBEOS_STATUS_SUCCESS) &&
               (dir_names(b, names), strstr(names, "a.txt,") && strstr(names, "c.TXT,") && !strstr(names, "b.log")),
           "a mask matches without case, and only what it matches");
    expect(query_dir(h, buf, 512, 12, 0, "*.log", 0, &n) == ST(VIBEOS_STATUS_NO_MORE_FILES),
           "the mask is the first call's; after it, nothing more");
    expect(query_dir(h, buf, 512, 12, 1, "?.log", 1, &n) == ST(VIBEOS_STATUS_SUCCESS) &&
               *(uint32_t *)b == 0u && *(uint32_t *)(b + 8) == 10u && b[12] == 'b',
           "a restart takes the new mask; a single entry ends the chain");
    expect(query_dir(h, buf, 512, 1, 0, "a.txt", 1, &n) == ST(VIBEOS_STATUS_SUCCESS) &&
               *(uint64_t *)(b + 40) == 7u && *(uint32_t *)(b + 60) == 10u && b[64] == 'a' &&
               (*(uint32_t *)(b + 56) & 0x10u) == 0u,
           "FileDirectoryInformation: its size, its name at 64, a file");
    expect(query_dir(h, buf, 512, 12, 0, "zz*", 1, &n) == ST(VIBEOS_STATUS_NO_SUCH_FILE),
           "a mask nothing matches, at the start: no such file");
    expect(query_dir(h, buf, 512, 12, 1, "*.TXT", 1, &n) == ST(VIBEOS_STATUS_SUCCESS) && *(uint32_t *)b == 0u &&
               (names[0] = (char)b[12], query_dir(h, buf, 512, 12, 0, 0, 0, &n) == ST(VIBEOS_STATUS_SUCCESS)) &&
               *(uint32_t *)b == 0u && (b[12] == 'a' || b[12] == 'c') && b[12] != names[0],
           "one at a time: each call one entry, and the mask holds across them");
    expect(query_dir(h, buf, 512, 99, 0, 0, 1, &n) == ST(VIBEOS_STATUS_INVALID_INFO_CLASS) &&
               query_dir(h, buf, 20, 1, 0, 0, 1, &n) == ST(VIBEOS_STATUS_INFO_LENGTH_MISMATCH),
           "an unknown class, a buffer short of the header");
    expect(query_dir(h, buf, 70, 1, 0, "a.txt", 1, &n) == ST(VIBEOS_STATUS_BUFFER_OVERFLOW) &&
               query_dir(h, buf, 512, 1, 0, 0, 0, &n) == ST(VIBEOS_STATUS_SUCCESS) && b[64] == 'a',
           "an entry that does not fit: the overflow, and the next call has it whole");
}

/* W4 step 6: a name found without case on a filesystem that keeps it, and
 * names beyond ASCII there and back. */
static void t_names(void) {
    const uint32_t rw = VIBEOS_GENERIC_READ | VIBEOS_GENERIC_WRITE;
    uint64_t h = 0, h2 = 0, info = 0, n = 0, buf = kf_ualloc(512);
    uint8_t *b = (uint8_t *)kf_uptr(buf);
    const uint16_t *w = (const uint16_t *)(b + 4);

    fresh(48);
    expect(create_file(&h, &info, rw, "\\??\\C:\\tmp\\Mixed.TXT", 2, 0x40) == ST(VIBEOS_STATUS_SUCCESS) &&
               nt(VIBEOS_NT_NR_NtClose, h, 0, 0, 0, 0) == ST(VIBEOS_STATUS_SUCCESS),
           "a file named with both cases, on tmpfs");
    expect(create_file(&h, &info, rw, "\\??\\C:\\TMP\\mixed.txt", 1, 0x40) == ST(VIBEOS_STATUS_SUCCESS) &&
               file_info(0, h, buf, 64, 9, &n) == ST(VIBEOS_STATUS_SUCCESS) &&
               w[1] == 't' && w[5] == 'M' && w[11] == 'T',
           "opened by another case, every component: it is the same file, named as it was made");
    expect(create_file(&h2, &info, rw, "\\??\\C:\\tmp\\MIXED.txt", 2, 0x40) ==
               ST(VIBEOS_STATUS_OBJECT_NAME_COLLISION),
           "and FILE_CREATE under a third case collides with it");
    (void)nt(VIBEOS_NT_NR_NtClose, h, 0, 0, 0, 0);
    g_oattr_wide = 1;
    expect(create_file(&h, &info, rw, "\\??\\C:\\tmp\\#%.txt", 2, 0x40) == ST(VIBEOS_STATUS_SUCCESS) &&
               file_info(0, h, buf, 64, 9, &n) == ST(VIBEOS_STATUS_SUCCESS) &&
               *(uint32_t *)b == 2u * 12u && w[5] == 0x00E9u && w[6] == 0xD83Du && w[7] == 0xDE00u && w[8] == '.',
           "a name beyond ASCII comes back as it was given, a surrogate pair included");
    expect(create_file(&h2, &info, rw, "\\??\\C:\\tmp\\!.txt", 2, 0x40) == ST(VIBEOS_STATUS_OBJECT_NAME_INVALID),
           "half a surrogate pair is not a name");
    g_oattr_wide = 0;
    expect(create_file(&h, &info, VIBEOS_GENERIC_READ, "\\??\\C:\\tmp", 1, 0x1) == ST(VIBEOS_STATUS_SUCCESS) &&
               query_dir(h, buf, 512, 12, 1, "*.txt", 1, &n) == ST(VIBEOS_STATUS_SUCCESS),
           "the directory lists");
    {
        int seen = 0;

        (void)query_dir(h, buf, 512, 12, 0, "*", 1, &n);
        for (;;) {
            uint32_t at = 0;

            for (;;) {
                const uint16_t *nm = (const uint16_t *)(b + at + 12);
                if (*(uint32_t *)(b + at + 8) == 14u && nm[0] == 0x00E9u && nm[1] == 0xD83Du && nm[2] == 0xDE00u) {
                    seen = 1;
                }
                if (*(uint32_t *)(b + at) == 0u) {
                    break;
                }
                at += *(uint32_t *)(b + at);
            }
            if (query_dir(h, buf, 512, 12, 0, 0, 0, &n) != ST(VIBEOS_STATUS_SUCCESS)) {
                break;
            }
        }
        expect(seen, "and its listing spells the name in UTF-16 too");
    }
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

/* No process makes more than a quarter of the object table (M-110). */
static void t_object_share(void) {
    uint64_t h = 0;
    uint32_t i;
    long r = ST(VIBEOS_STATUS_SUCCESS);
    int me = fresh(45), other;

    for (i = 0; i < 64u && r == ST(VIBEOS_STATUS_SUCCESS); i++) {
        r = create_event(&h, VIBEOS_EVENT_ALL_ACCESS, 0, VIBEOS_NT_NOTIFICATION_EVENT, 0);
    }
    expect(r == ST(VIBEOS_STATUS_SUCCESS), "a process makes 64 events");
    expect(create_event(&h, VIBEOS_EVENT_ALL_ACCESS, 0, VIBEOS_NT_NOTIFICATION_EVENT, 0) ==
               ST(VIBEOS_STATUS_QUOTA_EXCEEDED),
           "a 65th is past its share: STATUS_QUOTA_EXCEEDED");
    other = kf_spawn(46, 46);
    kf_set_current(other);
    expect(create_event(&h, VIBEOS_EVENT_ALL_ACCESS, 0, VIBEOS_NT_NOTIFICATION_EVENT, 0) ==
               ST(VIBEOS_STATUS_SUCCESS), "and another process is not refused for it");
    kf_set_current(me);
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
    kf_next_sp(stack_args(rest, 7));
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
    expect(open_file(&fh, VIBEOS_GENERIC_WRITE, "\\??\\C:\\win\\text.txt") == ST(VIBEOS_STATUS_SUCCESS) &&
               nt(VIBEOS_NT_NR_NtClose, fh, 0, 0, 0, 0) == ST(VIBEOS_STATUS_SUCCESS),
           "a file opened to write (W4)");
    expect(open_file(&fh, 0x00120089u, "\\??\\C:\\win\\text.txt") == ST(VIBEOS_STATUS_SUCCESS) &&
           fh >= 16u, "a file opened to read, by its NT path");
    expect(create_section(&sh, 0x08000000u /* SEC_COMMIT */, fh) == ST(VIBEOS_STATUS_SUCCESS) &&
               nt(VIBEOS_NT_NR_NtClose, sh, 0, 0, 0, 0) == ST(VIBEOS_STATUS_SUCCESS),
           "a data section of a file, read-only (W3; t_data_sections has the rest)");
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

/* ---- virtual memory (W3 step 2) ------------------------------------------------ */

static long valloc(uint64_t *base, uint64_t *size, uint64_t zero_bits, uint32_t type,
                   uint32_t prot) {
    uint64_t b = kf_ualloc(8), s = kf_ualloc(8);
    uint64_t rest[2] = { type, prot };
    long r;

    *(uint64_t *)kf_uptr(b) = *base;
    *(uint64_t *)kf_uptr(s) = *size;
    kf_next_sp(stack_args(rest, 2));
    r = nt(VIBEOS_NT_NR_NtAllocateVirtualMemory, VIBEOS_NT_CURRENT_PROCESS, b, zero_bits, s, 0);
    *base = *(uint64_t *)kf_uptr(b);
    *size = *(uint64_t *)kf_uptr(s);
    return r;
}

static long vfree(uint64_t *base, uint64_t *size, uint32_t type) {
    uint64_t b = kf_ualloc(8), s = kf_ualloc(8);
    long r;

    *(uint64_t *)kf_uptr(b) = *base;
    *(uint64_t *)kf_uptr(s) = *size;
    r = nt(VIBEOS_NT_NR_NtFreeVirtualMemory, VIBEOS_NT_CURRENT_PROCESS, b, s, type, 0);
    *base = *(uint64_t *)kf_uptr(b);
    *size = *(uint64_t *)kf_uptr(s);
    return r;
}

static long vprotect(uint64_t base, uint64_t size, uint32_t prot, uint32_t *old) {
    uint64_t b = kf_ualloc(8), s = kf_ualloc(8), o = kf_ualloc(8);
    uint64_t rest[1] = { o };
    long r;

    *(uint64_t *)kf_uptr(b) = base;
    *(uint64_t *)kf_uptr(s) = size;
    *(uint32_t *)kf_uptr(o) = 0xEEEEu;
    kf_next_sp(stack_args(rest, 1));
    r = nt(VIBEOS_NT_NR_NtProtectVirtualMemory, VIBEOS_NT_CURRENT_PROCESS, b, s, prot, 0);
    *old = *(uint32_t *)kf_uptr(o);
    return r;
}

static long vquery(uint64_t va, uint32_t cls, uint64_t len, vibeos_nt_memory_basic_t *mbi) {
    uint64_t buf = kf_ualloc(64), got = kf_ualloc(8);
    uint64_t rest[2] = { len, got };
    long r;

    memset(kf_uptr(buf), 0xA5, 64);
    kf_next_sp(stack_args(rest, 2));
    r = nt(VIBEOS_NT_NR_NtQueryVirtualMemory, VIBEOS_NT_CURRENT_PROCESS, va, cls, buf, 0);
    memcpy(mbi, kf_uptr(buf), sizeof(*mbi));
    if (r == ST(VIBEOS_STATUS_SUCCESS) && *(uint64_t *)kf_uptr(got) != sizeof(*mbi)) {
        return -1;   /* the length written is the structure's */
    }
    return r;
}

static void t_virtual_memory(void) {
    vibeos_nt_memory_basic_t m;
    uint64_t a, as, b, bs, c, cs, x, xs;
    uint32_t old;
    uint8_t byte = 0x5A, y = 0;

    fresh(43);
    /* Reserved and committed at once, wherever there is room: at 64 KiB, the
     * size by the page, and nothing made until it is touched. */
    a = 0;
    as = 0x2800;
    expect(valloc(&a, &as, 0, VIBEOS_NT_MEM_RESERVE | VIBEOS_NT_MEM_COMMIT,
                  VIBEOS_NT_PAGE_READWRITE) == ST(VIBEOS_STATUS_SUCCESS) &&
               a != 0u && (a & 0xFFFFu) == 0u && as == 0x3000u,
           "reserve and commit: at 64 KiB, by the page");
    expect(kf_peek(a + 0x1000u, &y, 1) == 0 && y == 0 && kf_poke(a + 0x2FFFu, &byte, 1) == 0 &&
               kf_peek(a + 0x2FFFu, &y, 1) == 0 && y == byte,
           "committed memory reads as zeros and keeps what is written, made on first touch");
    expect(kf_poke(a + 0x3000u, &byte, 1) != 0, "and not a byte past it");
    expect(vquery(a + 0x123u, 0, sizeof(m), &m) == ST(VIBEOS_STATUS_SUCCESS) &&
               m.base_address == a && m.allocation_base == a &&
               m.allocation_protect == VIBEOS_NT_PAGE_READWRITE && m.region_size == 0x3000u &&
               m.state == VIBEOS_NT_MEM_COMMIT && m.protect == VIBEOS_NT_PAGE_READWRITE &&
               m.type == VIBEOS_NT_MEM_PRIVATE,
           "MEMORY_BASIC_INFORMATION of a committed allocation");
    x = a + 0x2000u;
    xs = 0x1000u;
    expect(valloc(&x, &xs, 0, VIBEOS_NT_MEM_COMMIT, VIBEOS_NT_PAGE_READONLY) ==
               ST(VIBEOS_STATUS_SUCCESS) && kf_poke(a + 0x2FFFu, &byte, 1) != 0 &&
               kf_peek(a + 0x2FFFu, &y, 1) == 0 && y == byte,
           "a commit over a page already made gives it the new protection, contents kept");

    /* The next one at the next 64 KiB, though the last ended three pages in. */
    b = 0;
    bs = 16u * 4096u;
    expect(valloc(&b, &bs, 0, VIBEOS_NT_MEM_RESERVE, VIBEOS_NT_PAGE_READWRITE) ==
               ST(VIBEOS_STATUS_SUCCESS) && b == a + 0x10000u && bs == 0x10000u,
           "a reservation starts at the next 64 KiB");
    expect(kf_peek(b, &y, 1) != 0, "reserved memory has no access");
    expect(vquery(b + 0x5000u, 0, sizeof(m), &m) == ST(VIBEOS_STATUS_SUCCESS) &&
               m.base_address == b + 0x5000u && m.allocation_base == b &&
               m.allocation_protect == VIBEOS_NT_PAGE_READWRITE && m.region_size == 0xB000u &&
               m.state == VIBEOS_NT_MEM_RESERVE && m.protect == 0u,
           "a reservation reports no protection, and its allocation's");

    /* Committed a piece at a time, inside the reservation. */
    c = b + 0x2100u;
    cs = 0x1000u;
    expect(valloc(&c, &cs, 0, VIBEOS_NT_MEM_COMMIT, VIBEOS_NT_PAGE_READONLY) ==
               ST(VIBEOS_STATUS_SUCCESS) && c == b + 0x2000u && cs == 0x2000u,
           "a commit inside a reservation, by the page: the range the bytes touch");
    expect(kf_peek(b + 0x2000u, &y, 1) == 0 && y == 0 && kf_poke(b + 0x2000u, &byte, 1) != 0 &&
               kf_peek(b + 0x1000u, &y, 1) != 0,
           "a read-only commit reads zeros and refuses a store; the rest stays reserved");
    expect(vquery(b, 0, sizeof(m), &m) == ST(VIBEOS_STATUS_SUCCESS) &&
               m.state == VIBEOS_NT_MEM_RESERVE && m.region_size == 0x2000u &&
               vquery(b + 0x3000u, 0, sizeof(m), &m) == ST(VIBEOS_STATUS_SUCCESS) &&
               m.base_address == b + 0x3000u && m.state == VIBEOS_NT_MEM_COMMIT &&
               m.protect == VIBEOS_NT_PAGE_READONLY && m.region_size == 0x1000u &&
               m.allocation_base == b,
           "a query stops where the state stops, and starts at the page asked about");
    x = b + 0x20000u;
    xs = 0x1000u;
    expect(valloc(&x, &xs, 0, VIBEOS_NT_MEM_COMMIT, VIBEOS_NT_PAGE_READWRITE) ==
               ST(VIBEOS_STATUS_CONFLICTING_ADDRESSES),
           "a commit where nothing is reserved is refused");
    x = b + 0xF000u;
    xs = 0x2000u;
    expect(valloc(&x, &xs, 0, VIBEOS_NT_MEM_COMMIT, VIBEOS_NT_PAGE_READWRITE) ==
               ST(VIBEOS_STATUS_CONFLICTING_ADDRESSES),
           "a commit running past its reservation is refused");
    x = b + 0x4321u;
    xs = 0x100u;
    expect(valloc(&x, &xs, 0, VIBEOS_NT_MEM_RESERVE, VIBEOS_NT_PAGE_READWRITE) ==
               ST(VIBEOS_STATUS_CONFLICTING_ADDRESSES),
           "a reservation over another is refused");
    x = b + 0x34567u;
    xs = 0x100u;
    expect(valloc(&x, &xs, 0, VIBEOS_NT_MEM_RESERVE, VIBEOS_NT_PAGE_READWRITE) ==
               ST(VIBEOS_STATUS_SUCCESS) && x == b + 0x30000u && xs == 0x5000u,
           "a reservation at an address: down to 64 KiB, and up to the page past the end");

    /* The requests Windows refuses before anything is looked at. */
    x = 0;
    xs = 0x1000u;
    expect(valloc(&x, &xs, 0, VIBEOS_NT_MEM_COMMIT, VIBEOS_NT_PAGE_WRITECOPY) ==
               ST(VIBEOS_STATUS_INVALID_PAGE_PROTECTION) &&
               valloc(&x, &xs, 0, VIBEOS_NT_MEM_COMMIT, 0x3u) ==
               ST(VIBEOS_STATUS_INVALID_PAGE_PROTECTION) &&
               valloc(&x, &xs, 0, VIBEOS_NT_MEM_COMMIT,
                      VIBEOS_NT_PAGE_NOACCESS | VIBEOS_NT_PAGE_GUARD) ==
               ST(VIBEOS_STATUS_INVALID_PAGE_PROTECTION),
           "copy-on-write private memory, two protections at once, a guarded no-access page");
    /* ZeroBits with an address the caller names: the address decides, and a
     * conflict there is a conflict (Wine's ntdll:virtual). */
    x = a;
    xs = 0x1000u;
    expect(valloc(&x, &xs, 12, VIBEOS_NT_MEM_RESERVE | VIBEOS_NT_MEM_COMMIT,
                  VIBEOS_NT_PAGE_READWRITE) == ST(VIBEOS_STATUS_CONFLICTING_ADDRESSES),
           "ZeroBits does not hide a conflict at a named address");
    x = 0;
    expect(valloc(&x, &xs, 0, 0u, VIBEOS_NT_PAGE_READWRITE) == ST(VIBEOS_STATUS_INVALID_PARAMETER) &&
               valloc(&x, &xs, 0, VIBEOS_NT_MEM_RESERVE | VIBEOS_NT_MEM_RESET,
                      VIBEOS_NT_PAGE_READWRITE) == ST(VIBEOS_STATUS_INVALID_PARAMETER) &&
               valloc(&x, &xs, 25, VIBEOS_NT_MEM_COMMIT, VIBEOS_NT_PAGE_READWRITE) ==
               ST(VIBEOS_STATUS_INVALID_PARAMETER_3) &&
               valloc(&x, &xs, 0, VIBEOS_NT_MEM_RESERVE | VIBEOS_NT_MEM_WRITE_WATCH,
                      VIBEOS_NT_PAGE_READWRITE) == ST(VIBEOS_STATUS_NOT_SUPPORTED),
           "no type, a reset in an allocation, ZeroBits past 21, a write watch");
    xs = 0;
    expect(valloc(&x, &xs, 0, VIBEOS_NT_MEM_COMMIT, VIBEOS_NT_PAGE_READWRITE) ==
               ST(VIBEOS_STATUS_INVALID_PARAMETER), "a size of zero");
    x = 0;
    xs = 0x1000u;
    expect(nt(VIBEOS_NT_NR_NtFreeVirtualMemory, 4, kf_ualloc(8), kf_ualloc(8),
              VIBEOS_NT_MEM_RELEASE, 0) == ST(VIBEOS_STATUS_INVALID_HANDLE),
           "another process is W6's");

    /* Protection: of committed pages, the old one the first page's. */
    expect(vprotect(b + 0x2800u, 0x100u, VIBEOS_NT_PAGE_READWRITE, &old) ==
               ST(VIBEOS_STATUS_SUCCESS) && old == VIBEOS_NT_PAGE_READONLY &&
               kf_poke(b + 0x2000u, &byte, 1) == 0,
           "a protection change: the old one back, and a page touched before takes the new");
    expect(vprotect(b + 0x3000u, 0x1000u, VIBEOS_NT_PAGE_READWRITE | VIBEOS_NT_PAGE_GUARD, &old) ==
               ST(VIBEOS_STATUS_SUCCESS) && old == VIBEOS_NT_PAGE_READONLY &&
               vquery(b + 0x3000u, 0, sizeof(m), &m) == ST(VIBEOS_STATUS_SUCCESS) &&
               m.protect == (VIBEOS_NT_PAGE_READWRITE | VIBEOS_NT_PAGE_GUARD) &&
               m.region_size == 0x1000u,
           "PAGE_GUARD is recorded and reported as it was given");
    expect(vprotect(b + 0x1000u, 0x2000u, VIBEOS_NT_PAGE_READWRITE, &old) ==
               ST(VIBEOS_STATUS_NOT_COMMITTED) && old == 0xEEEEu,
           "a range with a reserved page in it is not protected, and nothing is written");
    expect(vprotect(a + 0x20000000u, 0x1000u, VIBEOS_NT_PAGE_READWRITE, &old) ==
               ST(VIBEOS_STATUS_CONFLICTING_ADDRESSES) &&
               vprotect(a, 0x1000u, 0x6u, &old) == ST(VIBEOS_STATUS_INVALID_PAGE_PROTECTION),
           "free memory, and a protection that is two");
    expect(vprotect(a + 0x2000u, 0x10000u, VIBEOS_NT_PAGE_READONLY, &old) ==
               ST(VIBEOS_STATUS_CONFLICTING_ADDRESSES),
           "a range running out of its allocation is refused");

    /* A decommit to the end, said from inside the allocation, is refused;
     * one of two bytes takes the pages they fall in (Wine's ntdll:virtual). */
    x = b + 0x1001u;
    xs = 0;
    expect(vfree(&x, &xs, VIBEOS_NT_MEM_DECOMMIT) == ST(VIBEOS_STATUS_FREE_VM_NOT_AT_BASE) &&
               x == b + 0x1001u && xs == 0u,
           "a decommit of size zero not at the base's page, and nothing written back");
    x = b + 0xffeu;
    xs = 0;
    expect(vfree(&x, &xs, VIBEOS_NT_MEM_DECOMMIT) == ST(VIBEOS_STATUS_SUCCESS) && x == b &&
               xs == 0u && kf_peek(b + 0x3000u, &y, 1) != 0 &&
               vquery(b, 0, sizeof(m), &m) == ST(VIBEOS_STATUS_SUCCESS) &&
               m.state == VIBEOS_NT_MEM_RESERVE && m.region_size == 0x10000u,
           "one from the base's page decommits the whole allocation and says size zero");
    /* Decommit: the pages go, the range stays reserved, and a commit again is
     * fresh zeros. */
    x = b + 0x2000u;
    xs = 0x1000u;
    expect(vfree(&x, &xs, VIBEOS_NT_MEM_DECOMMIT) == ST(VIBEOS_STATUS_SUCCESS) &&
               x == b + 0x2000u && xs == 0x1000u && kf_peek(b + 0x2000u, &y, 1) != 0 &&
               vquery(b + 0x2000u, 0, sizeof(m), &m) == ST(VIBEOS_STATUS_SUCCESS) &&
               m.state == VIBEOS_NT_MEM_RESERVE,
           "a decommit leaves the range reserved, with no access");
    x = b + 0x2000u;
    xs = 0x1000u;
    expect(valloc(&x, &xs, 0, VIBEOS_NT_MEM_COMMIT, VIBEOS_NT_PAGE_READWRITE) ==
               ST(VIBEOS_STATUS_SUCCESS) && kf_peek(b + 0x2000u, &y, 1) == 0 && y == 0,
           "committed again, the page is zeros - not what was written before");

    /* Release: the whole allocation, by its base. */
    x = b + 0x1000u;
    xs = 0;
    expect(vfree(&x, &xs, VIBEOS_NT_MEM_RELEASE) == ST(VIBEOS_STATUS_FREE_VM_NOT_AT_BASE),
           "a release not at the allocation's base");
    x = b + 0xF000u;
    xs = 0x2000u;
    expect(vfree(&x, &xs, VIBEOS_NT_MEM_RELEASE) == ST(VIBEOS_STATUS_UNABLE_TO_FREE_VM) &&
               x == b + 0xF000u && xs == 0x2000u,
           "a release running past the allocation, and nothing written back");
    x = b;
    xs = 0;
    expect(vfree(&x, &xs, 0u) == ST(VIBEOS_STATUS_INVALID_PARAMETER_4) &&
               vfree(&x, &xs, VIBEOS_NT_MEM_RELEASE | VIBEOS_NT_MEM_DECOMMIT) ==
               ST(VIBEOS_STATUS_INVALID_PARAMETER_4),
           "a free type that is neither, or both");
    expect(vfree(&x, &xs, VIBEOS_NT_MEM_RELEASE) == ST(VIBEOS_STATUS_SUCCESS) && x == b &&
               xs == 0x10000u && kf_peek(b + 0x2000u, &y, 1) != 0,
           "released whole, by its base: the size is the allocation's");
    expect(vquery(b + 0x4000u, 0, sizeof(m), &m) == ST(VIBEOS_STATUS_SUCCESS) &&
               m.state == VIBEOS_NT_MEM_FREE && m.protect == VIBEOS_NT_PAGE_NOACCESS &&
               m.allocation_base == 0u && m.base_address == b + 0x4000u &&
               m.region_size == 0x2C000u,
           "free memory runs to the next region");
    x = b;
    xs = 0;
    expect(vfree(&x, &xs, VIBEOS_NT_MEM_RELEASE) == ST(VIBEOS_STATUS_MEMORY_NOT_ALLOCATED),
           "released twice: nothing is allocated there");

    expect(vquery(a, 1u, sizeof(m), &m) == ST(VIBEOS_STATUS_INVALID_INFO_CLASS) &&
               vquery(a, 0, sizeof(m) - 1u, &m) == ST(VIBEOS_STATUS_INFO_LENGTH_MISMATCH) &&
               vquery(ks_user_top(), 0, sizeof(m), &m) == ST(VIBEOS_STATUS_INVALID_PARAMETER),
           "a class not written, a short buffer, an address past user memory");
    expect(kf_uaccess_under_mm() == 0u,
           "no store into the program's memory under its own mm lock");
}

/* ---- NtAllocateVirtualMemoryEx and placeholders (W3 step 10) ------------------- */

/* The upper halves of the ULONGs on the stack are garbage, as a caller may
 * leave them: the service reads 32 bits of each. */
static long valloc_ex(uint64_t *base, uint64_t *size, uint32_t type, uint32_t prot,
                      uint64_t params, uint32_t count) {
    uint64_t b = kf_ualloc(8), s = kf_ualloc(8);
    uint64_t rest[3] = { 0xDEAD000000000000ull | prot, params,
                         0xBEEF000000000000ull | count };
    long r;

    *(uint64_t *)kf_uptr(b) = *base;
    *(uint64_t *)kf_uptr(s) = *size;
    kf_next_sp(stack_args(rest, 3));
    r = nt(VIBEOS_NT_NR_NtAllocateVirtualMemoryEx, VIBEOS_NT_CURRENT_PROCESS, b, s,
           0xFEED000000000000ull | type, 0);
    *base = *(uint64_t *)kf_uptr(b);
    *size = *(uint64_t *)kf_uptr(s);
    return r;
}

/* The size of the run NtQueryVirtualMemory reports from `va`, or 0. */
static uint64_t region_of(uint64_t va) {
    vibeos_nt_memory_basic_t m;

    return vquery(va, 0, sizeof(m), &m) == ST(VIBEOS_STATUS_SUCCESS) ? m.region_size : 0u;
}

/* One extended parameter of address requirements, in the program's memory. */
static uint64_t requirements(uint64_t lo, uint64_t hi, uint64_t align) {
    uint64_t p = kf_ualloc(sizeof(vibeos_nt_mem_ext_param_t));
    uint64_t a = kf_ualloc(sizeof(vibeos_nt_mem_address_requirements_t));
    vibeos_nt_mem_address_requirements_t *ar = kf_uptr(a);
    vibeos_nt_mem_ext_param_t *ep = kf_uptr(p);

    ar->lowest_starting_address = lo;
    ar->highest_ending_address = hi;
    ar->alignment = align;
    ep->type = VIBEOS_NT_MEM_EXT_ADDRESS_REQUIREMENTS | 0xAB00ull;   /* reserved bits ignored */
    ep->value = a;
    return p;
}

/* Wine 9.0's test_NtAllocateVirtualMemoryEx, the placeholder half, in its
 * order: what Windows 10 answers is what the test reads. */
static void t_placeholders(void) {
    const uint32_t rsv_ph = VIBEOS_NT_MEM_RESERVE | VIBEOS_NT_MEM_RESERVE_PLACEHOLDER;
    const uint32_t replace = VIBEOS_NT_MEM_RESERVE | VIBEOS_NT_MEM_REPLACE_PLACEHOLDER;
    const uint32_t preserve = VIBEOS_NT_MEM_RELEASE | VIBEOS_NT_MEM_PRESERVE_PLACEHOLDER;
    const uint32_t coalesce = VIBEOS_NT_MEM_RELEASE | VIBEOS_NT_MEM_COALESCE_PLACEHOLDERS;
    vibeos_nt_memory_basic_t m;
    uint64_t a, as, p1, p2, p3, s, x, xs, ep;
    uint8_t byte = 0xCC, y = 0;

    fresh(44);
    a = 0;
    as = 0x1000u;
    expect(valloc_ex(&a, &as, VIBEOS_NT_MEM_RESERVE | VIBEOS_NT_MEM_COMMIT,
                     VIBEOS_NT_PAGE_EXECUTE_READWRITE, 0, 0) == ST(VIBEOS_STATUS_SUCCESS) &&
               a != 0u && as == 0x1000u,
           "Ex with no parameters is NtAllocateVirtualMemory");
    xs = 0;
    expect(vfree(&a, &xs, VIBEOS_NT_MEM_RELEASE) == ST(VIBEOS_STATUS_SUCCESS),
           "and gives back what that gives back");
    expect(valloc_ex(&a, &as, VIBEOS_NT_MEM_RESERVE | VIBEOS_NT_MEM_COMMIT,
                     VIBEOS_NT_PAGE_READWRITE, 0, 1) == ST(VIBEOS_STATUS_INVALID_PARAMETER),
           "a count with no parameters");
    {
        uint64_t bb = kf_ualloc(8), ss = kf_ualloc(8);
        uint64_t rest[3] = { VIBEOS_NT_PAGE_READWRITE, 0, 0 };

        *(uint64_t *)kf_uptr(ss) = 0x1000u;
        kf_next_sp(stack_args(rest, 3));
        expect(nt(VIBEOS_NT_NR_NtAllocateVirtualMemoryEx, 0, bb, ss,
                  VIBEOS_NT_MEM_RESERVE | VIBEOS_NT_MEM_COMMIT, 0) ==
                   ST(VIBEOS_STATUS_INVALID_HANDLE),
               "a null process handle");
    }

    /* A placeholder: only from Ex, only with no access. */
    a = 0;
    as = 0x10000u;
    expect(valloc(&a, &as, 0, rsv_ph, VIBEOS_NT_PAGE_NOACCESS) ==
               ST(VIBEOS_STATUS_INVALID_PARAMETER),
           "NtAllocateVirtualMemory knows no placeholders");
    expect(valloc_ex(&a, &as, rsv_ph, VIBEOS_NT_PAGE_READWRITE, 0, 0) ==
               ST(VIBEOS_STATUS_INVALID_PARAMETER),
           "a placeholder anybody may touch");
    expect(valloc_ex(&a, &as, rsv_ph, VIBEOS_NT_PAGE_NOACCESS, 0, 0) ==
               ST(VIBEOS_STATUS_SUCCESS) && a != 0u && (a & 0xFFFFu) == 0u && as == 0x10000u,
           "a placeholder reserved");
    expect(vquery(a, 0, sizeof(m), &m) == ST(VIBEOS_STATUS_SUCCESS) &&
               m.allocation_base == a && m.allocation_protect == VIBEOS_NT_PAGE_NOACCESS &&
               m.state == VIBEOS_NT_MEM_RESERVE && m.type == VIBEOS_NT_MEM_PRIVATE &&
               m.region_size == 0x10000u,
           "a placeholder is reserved private memory with no access, marks unreported");
    x = a;
    xs = 0x1000u;
    expect(valloc(&x, &xs, 0, VIBEOS_NT_MEM_COMMIT, VIBEOS_NT_PAGE_READWRITE) ==
               ST(VIBEOS_STATUS_CONFLICTING_ADDRESSES),
           "a placeholder is not committed by asking");

    /* Replaced, written, preserved, replaced again: zeros. */
    s = 0x10000u;
    expect(valloc_ex(&a, &s, replace | VIBEOS_NT_MEM_COMMIT, VIBEOS_NT_PAGE_READWRITE, 0, 0) ==
               ST(VIBEOS_STATUS_SUCCESS) && s == 0x10000u &&
               kf_poke(a + 0x5000u, &byte, 1) == 0,
           "a placeholder replaced by committed memory");
    expect(vfree(&a, &s, preserve) == ST(VIBEOS_STATUS_SUCCESS) && s == 0x10000u &&
               kf_peek(a + 0x5000u, &y, 1) != 0,
           "released into a placeholder: no access");
    s = 0x10000u;
    expect(valloc_ex(&a, &s, replace | VIBEOS_NT_MEM_COMMIT, VIBEOS_NT_PAGE_READONLY, 0, 0) ==
               ST(VIBEOS_STATUS_SUCCESS) && kf_peek(a + 0x5000u, &y, 1) == 0 && y == 0,
           "replaced again, the pages are fresh zeros");
    expect(vquery(a, 0, sizeof(m), &m) == ST(VIBEOS_STATUS_SUCCESS) &&
               m.allocation_protect == VIBEOS_NT_PAGE_READONLY &&
               m.state == VIBEOS_NT_MEM_COMMIT && m.type == VIBEOS_NT_MEM_PRIVATE &&
               m.region_size == 0x10000u,
           "a replaced placeholder is the allocation it was replaced by");
    expect(vfree(&a, &s, preserve) == ST(VIBEOS_STATUS_SUCCESS) &&
               vquery(a, 0, sizeof(m), &m) == ST(VIBEOS_STATUS_SUCCESS) &&
               m.allocation_protect == VIBEOS_NT_PAGE_NOACCESS &&
               m.state == VIBEOS_NT_MEM_RESERVE && m.region_size == 0x10000u,
           "and a placeholder again");
    expect(valloc_ex(&a, &s, rsv_ph, VIBEOS_NT_PAGE_NOACCESS, 0, 0) ==
               ST(VIBEOS_STATUS_CONFLICTING_ADDRESSES) &&
               valloc_ex(&a, &s, VIBEOS_NT_MEM_RESERVE, VIBEOS_NT_PAGE_NOACCESS, 0, 0) ==
               ST(VIBEOS_STATUS_CONFLICTING_ADDRESSES),
           "a reservation over a placeholder");
    xs = 0x1000u;
    expect(valloc_ex(&a, &xs, replace, VIBEOS_NT_PAGE_NOACCESS, 0, 0) ==
               ST(VIBEOS_STATUS_CONFLICTING_ADDRESSES),
           "a placeholder is replaced whole or not at all");
    xs = 0x10000u;
    expect(valloc_ex(&a, &xs, VIBEOS_NT_MEM_REPLACE_PLACEHOLDER, VIBEOS_NT_PAGE_READWRITE, 0, 0) ==
               ST(VIBEOS_STATUS_INVALID_PARAMETER) &&
               valloc_ex(&a, &xs, VIBEOS_NT_MEM_COMMIT | VIBEOS_NT_MEM_REPLACE_PLACEHOLDER,
                         VIBEOS_NT_PAGE_READWRITE, 0, 0) == ST(VIBEOS_STATUS_INVALID_PARAMETER),
           "a replacement that does not reserve");
    x = 0;
    xs = 0x10000u;
    expect(valloc_ex(&x, &xs, VIBEOS_NT_MEM_RESERVE, VIBEOS_NT_PAGE_NOACCESS, 0, 0) ==
               ST(VIBEOS_STATUS_SUCCESS) &&
               valloc_ex(&x, &xs, replace, VIBEOS_NT_PAGE_NOACCESS, 0, 0) ==
               ST(VIBEOS_STATUS_INVALID_PARAMETER),
           "a reservation is not a placeholder to replace");
    p1 = x;
    expect(vfree(&p1, &xs, preserve) == ST(VIBEOS_STATUS_CONFLICTING_ADDRESSES) &&
               xs == 0x10000u && vfree(&p1, &xs, VIBEOS_NT_MEM_RELEASE) == ST(VIBEOS_STATUS_SUCCESS),
           "nor released into one");

    /* Split in three, from the middle. */
    xs = 0x10000u;
    expect(vfree(&a, &xs, preserve) == ST(VIBEOS_STATUS_CONFLICTING_ADDRESSES),
           "a placeholder whole is not preserved into itself");
    p1 = a + 0x8000u;
    p2 = a + 0xC000u;
    s = 0x4000u;
    expect(vfree(&p1, &s, preserve) == ST(VIBEOS_STATUS_SUCCESS) && p1 == a + 0x8000u &&
               s == 0x4000u,
           "a placeholder split from the middle");
    expect(region_of(a) == 0x8000u && region_of(p1) == 0x4000u && region_of(p2) == 0x4000u &&
               vquery(p2, 0, sizeof(m), &m) == ST(VIBEOS_STATUS_SUCCESS) &&
               m.allocation_base == p2,
           "three placeholders, the one above based at itself");
    x = a;
    expect(vfree(&x, &s, VIBEOS_NT_MEM_RELEASE) == ST(VIBEOS_STATUS_SUCCESS) && x == a &&
               s == 0x4000u && vquery(a + 0x4000u, 0, sizeof(m), &m) == ST(VIBEOS_STATUS_SUCCESS) &&
               m.allocation_base == a + 0x4000u && m.state == VIBEOS_NT_MEM_RESERVE,
           "a release of the front of an allocation: the rest is based where it starts");
    expect(vfree(&p1, &s, VIBEOS_NT_MEM_RELEASE) == ST(VIBEOS_STATUS_SUCCESS) &&
               vfree(&p2, &s, VIBEOS_NT_MEM_RELEASE) == ST(VIBEOS_STATUS_SUCCESS) &&
               vquery(p2, 0, sizeof(m), &m) == ST(VIBEOS_STATUS_SUCCESS) &&
               m.state == VIBEOS_NT_MEM_FREE,
           "each released by its own base");
    x = a + 0x4000u;
    s = 0;
    expect(vfree(&x, &s, VIBEOS_NT_MEM_RELEASE) == ST(VIBEOS_STATUS_SUCCESS), "tidied");

    /* Split from the bottom, joined again. */
    a = 0;
    as = 0x10000u;
    expect(valloc_ex(&a, &as, rsv_ph, VIBEOS_NT_PAGE_NOACCESS, 0, 0) == ST(VIBEOS_STATUS_SUCCESS),
           "a second placeholder");
    s = 0;
    expect(vfree(&a, &s, preserve) == ST(VIBEOS_STATUS_INVALID_PARAMETER_3) && s == 0u,
           "preserved with no size");
    p1 = a;
    p2 = a + 0x4000u;
    p3 = a + 0x8000u;
    s = 0x4000u;
    expect(vfree(&p1, &s, preserve) == ST(VIBEOS_STATUS_SUCCESS) && p1 == a && s == 0x4000u &&
               vfree(&p2, &s, preserve) == ST(VIBEOS_STATUS_SUCCESS) &&
               region_of(p1) == 0x4000u && region_of(p2) == 0x4000u && region_of(p3) == 0x8000u,
           "split from the bottom, twice");
    expect(vfree(&p1, &s, coalesce) == ST(VIBEOS_STATUS_CONFLICTING_ADDRESSES),
           "one placeholder is not joined");
    xs = 0x10000u;
    expect(vfree(&p1, &xs, VIBEOS_NT_MEM_COALESCE_PLACEHOLDERS) ==
               ST(VIBEOS_STATUS_INVALID_PARAMETER_4),
           "joining without releasing");
    xs = 0x11000u;
    expect(vfree(&p1, &xs, coalesce) == ST(VIBEOS_STATUS_CONFLICTING_ADDRESSES), "joined past them");
    xs = 0xF000u;
    expect(vfree(&p1, &xs, coalesce) == ST(VIBEOS_STATUS_CONFLICTING_ADDRESSES), "joined short");
    x = a + 0x1000u;
    expect(vfree(&x, &xs, coalesce) == ST(VIBEOS_STATUS_CONFLICTING_ADDRESSES),
           "joined from inside one");
    xs = 0;
    expect(vfree(&p1, &xs, coalesce) == ST(VIBEOS_STATUS_INVALID_PARAMETER_3), "joined with no size");
    xs = 0x10000u;
    expect(vfree(&p1, &xs, VIBEOS_NT_MEM_RELEASE) == ST(VIBEOS_STATUS_UNABLE_TO_FREE_VM),
           "a release does not run on into the next placeholder");
    expect(vfree(&p1, &xs, coalesce) == ST(VIBEOS_STATUS_SUCCESS) && p1 == a && xs == 0x10000u &&
               region_of(a) == 0x10000u && vquery(p3, 0, sizeof(m), &m) == ST(VIBEOS_STATUS_SUCCESS) &&
               m.allocation_base == a,
           "three placeholders joined into one");
    s = 0x4000u;
    xs = 0xC000u;
    expect(vfree(&p1, &s, preserve) == ST(VIBEOS_STATUS_SUCCESS) &&
               valloc_ex(&p2, &xs, replace, VIBEOS_NT_PAGE_READWRITE, 0, 0) ==
               ST(VIBEOS_STATUS_SUCCESS),
           "split, and the upper part replaced by a reservation");
    xs = 0x10000u;
    expect(vfree(&p1, &xs, coalesce) == ST(VIBEOS_STATUS_CONFLICTING_ADDRESSES),
           "a placeholder is not joined to an allocation");
    x = p2 + 0x1000u;
    xs = 0x2000u;
    expect(valloc(&x, &xs, 0, VIBEOS_NT_MEM_COMMIT, VIBEOS_NT_PAGE_READWRITE) ==
               ST(VIBEOS_STATUS_SUCCESS) && kf_poke(p2 + 0x2000u, &byte, 1) == 0,
           "the replacement commits as any reservation does");
    xs = 0xC000u;
    expect(vfree(&p2, &xs, VIBEOS_NT_MEM_RELEASE) == ST(VIBEOS_STATUS_SUCCESS) && xs == 0xC000u &&
               vfree(&p1, &s, VIBEOS_NT_MEM_RELEASE) == ST(VIBEOS_STATUS_SUCCESS),
           "released, each");
    s = 0;
    expect(vfree(&p3, &s, VIBEOS_NT_MEM_RELEASE) == ST(VIBEOS_STATUS_MEMORY_NOT_ALLOCATED),
           "and nothing left");

    /* A release from the middle of an ordinary allocation, as Wine's
     * test_NtFreeVirtualMemory asks: the part above is an allocation of its
     * own, the part below keeps its base, both keep their pages. */
    a = 0;
    as = 0x10000u;
    expect(valloc(&a, &as, 0, VIBEOS_NT_MEM_RESERVE | VIBEOS_NT_MEM_COMMIT,
                  VIBEOS_NT_PAGE_READWRITE) == ST(VIBEOS_STATUS_SUCCESS) &&
               kf_poke(a + 0x2000u, &byte, 1) == 0,
           "an allocation to release a piece of");
    x = a + 0x1001u;
    xs = 0xFFFu;
    expect(vfree(&x, &xs, VIBEOS_NT_MEM_RELEASE) == ST(VIBEOS_STATUS_SUCCESS) &&
               x == a + 0x1000u && xs == 0x1000u && kf_peek(a + 0x1000u, &y, 1) != 0 &&
               kf_poke(a, &byte, 1) == 0 && kf_peek(a + 0x2000u, &y, 1) == 0 && y == byte,
           "a page released from the middle; the pages beside it stay");
    expect(vquery(a + 0x2000u, 0, sizeof(m), &m) == ST(VIBEOS_STATUS_SUCCESS) &&
               m.allocation_base == a + 0x2000u && m.region_size == 0xE000u &&
               region_of(a) == 0x1000u,
           "the part above is based at itself");
    x = a;
    xs = 0x10000u;
    expect(vfree(&x, &xs, VIBEOS_NT_MEM_RELEASE) == ST(VIBEOS_STATUS_UNABLE_TO_FREE_VM),
           "the part below ends where it does");
    x = a + 0x2000u;
    xs = 0;
    s = 0;
    expect(vfree(&x, &xs, VIBEOS_NT_MEM_RELEASE) == ST(VIBEOS_STATUS_SUCCESS) && xs == 0xE000u &&
               vfree(&a, &s, VIBEOS_NT_MEM_RELEASE) == ST(VIBEOS_STATUS_SUCCESS) && s == 0x1000u,
           "each released whole by its base");

    /* Extended parameters. */
    ep = requirements(0, 0, 0);
    a = 0;
    as = 0x1000u;
    expect(valloc_ex(&a, &as, VIBEOS_NT_MEM_RESERVE | VIBEOS_NT_MEM_COMMIT,
                     VIBEOS_NT_PAGE_READWRITE, ep, 1) == ST(VIBEOS_STATUS_SUCCESS),
           "address requirements of nothing");
    xs = 0;
    expect(vfree(&a, &xs, VIBEOS_NT_MEM_RELEASE) == ST(VIBEOS_STATUS_SUCCESS), "released");
    {
        uint64_t two = kf_ualloc(2u * sizeof(vibeos_nt_mem_ext_param_t));
        vibeos_nt_mem_ext_param_t *e = kf_uptr(two);

        memcpy(&e[0], kf_uptr(ep), sizeof(e[0]));
        memcpy(&e[1], kf_uptr(ep), sizeof(e[1]));
        a = 0;
        expect(valloc_ex(&a, &as, VIBEOS_NT_MEM_RESERVE, VIBEOS_NT_PAGE_READWRITE, two, 2) ==
                   ST(VIBEOS_STATUS_INVALID_PARAMETER),
               "a parameter given twice");
        e[0].type = 0;
        expect(valloc_ex(&a, &as, VIBEOS_NT_MEM_RESERVE, VIBEOS_NT_PAGE_READWRITE, two, 1) ==
                   ST(VIBEOS_STATUS_INVALID_PARAMETER),
               "a parameter of no type");
        e[0].type = VIBEOS_NT_MEM_EXT_MAX;
        expect(valloc_ex(&a, &as, VIBEOS_NT_MEM_RESERVE, VIBEOS_NT_PAGE_READWRITE, two, 1) ==
                   ST(VIBEOS_STATUS_INVALID_PARAMETER),
               "a parameter past the last type");
        e[0].type = VIBEOS_NT_MEM_EXT_ATTRIBUTE_FLAGS;
        e[0].value = 0;
        expect(valloc_ex(&a, &as, VIBEOS_NT_MEM_RESERVE, VIBEOS_NT_PAGE_READWRITE, two, 1) ==
                   ST(VIBEOS_STATUS_SUCCESS),
               "attribute flags of nothing");
        xs = 0;
        expect(vfree(&a, &xs, VIBEOS_NT_MEM_RELEASE) == ST(VIBEOS_STATUS_SUCCESS), "released");
        e[0].value = VIBEOS_NT_MEM_EXT_EC_CODE;
        a = 0;
        expect(valloc_ex(&a, &as, VIBEOS_NT_MEM_RESERVE, VIBEOS_NT_PAGE_READWRITE, two, 1) ==
                   ST(VIBEOS_STATUS_INVALID_PARAMETER),
               "ARM64EC code on a machine that is not ARM64");
    }
    a = 0;
    as = 0x10000u;
    expect(valloc_ex(&a, &as, VIBEOS_NT_MEM_RESERVE, VIBEOS_NT_PAGE_READWRITE,
                     requirements(0, 0x20001000u + 1u, 0), 1) == ST(VIBEOS_STATUS_INVALID_PARAMETER) &&
               valloc_ex(&a, &as, VIBEOS_NT_MEM_RESERVE, VIBEOS_NT_PAGE_READWRITE,
                         requirements(0, 0x20000800u - 1u, 0), 1) ==
               ST(VIBEOS_STATUS_INVALID_PARAMETER) &&
               valloc_ex(&a, &as, VIBEOS_NT_MEM_RESERVE, VIBEOS_NT_PAGE_READWRITE,
                         requirements(0, VIBEOS_NT_MAX_USER_ADDRESS + 0x1000u, 0), 1) ==
               ST(VIBEOS_STATUS_INVALID_PARAMETER),
           "a ceiling not a page's end, or past the top");
    expect(valloc_ex(&a, &as, VIBEOS_NT_MEM_RESERVE, VIBEOS_NT_PAGE_READWRITE,
                     requirements(0, 0, 0x8000u), 1) == ST(VIBEOS_STATUS_INVALID_PARAMETER) &&
               valloc_ex(&a, &as, VIBEOS_NT_MEM_RESERVE, VIBEOS_NT_PAGE_READWRITE,
                         requirements(0, 0, 0x30000u), 1) == ST(VIBEOS_STATUS_INVALID_PARAMETER) &&
               valloc_ex(&a, &as, VIBEOS_NT_MEM_RESERVE, VIBEOS_NT_PAGE_READWRITE,
                         requirements(0x20001000u, 0, 0), 1) ==
               ST(VIBEOS_STATUS_INVALID_PARAMETER) &&
               valloc_ex(&a, &as, VIBEOS_NT_MEM_RESERVE, VIBEOS_NT_PAGE_READWRITE,
                         requirements(0x30000000u, 0x20000000u - 1u, 0), 1) ==
               ST(VIBEOS_STATUS_INVALID_PARAMETER),
           "an alignment under 64 KiB or not a power of two, a floor not at 64 KiB, "
           "a ceiling under the floor");
    expect(valloc_ex(&a, &as, VIBEOS_NT_MEM_RESERVE, VIBEOS_NT_PAGE_READWRITE,
                     requirements(0, VIBEOS_NT_MAX_USER_ADDRESS, 0x40000u), 1) ==
               ST(VIBEOS_STATUS_SUCCESS) && (a & 0x3FFFFu) == 0u,
           "placed at the alignment asked for, the top of user memory a valid ceiling");
    expect(valloc_ex(&a, &as, VIBEOS_NT_MEM_COMMIT, VIBEOS_NT_PAGE_READWRITE,
                     requirements(0, 0, 0x40000u), 1) == ST(VIBEOS_STATUS_INVALID_PARAMETER),
           "requirements with an address the caller names");
    x = a + 0x400000u;
    xs = 0x1000u;
    expect(valloc_ex(&x, &xs, VIBEOS_NT_MEM_RESERVE, VIBEOS_NT_PAGE_READWRITE, 0, 0) ==
               ST(VIBEOS_STATUS_SUCCESS),
           "something further up");
    p1 = 0;
    s = 0x10000u;
    expect(valloc_ex(&p1, &s, VIBEOS_NT_MEM_RESERVE, VIBEOS_NT_PAGE_READWRITE,
                     requirements(x, 0, 0), 1) == ST(VIBEOS_STATUS_SUCCESS) && p1 >= x + 0x10000u,
           "a floor: placed above it, past what is there");
    p2 = 0;
    expect(valloc_ex(&p2, &s, VIBEOS_NT_MEM_RESERVE, VIBEOS_NT_PAGE_READWRITE, 0, 0) ==
               ST(VIBEOS_STATUS_SUCCESS) && p2 < x,
           "and the arena below it still placed from");
    p3 = 0;
    expect(valloc_ex(&p3, &s, VIBEOS_NT_MEM_RESERVE, VIBEOS_NT_PAGE_READWRITE,
                     requirements(0, 0x20001000u - 1u, 0), 1) == ST(VIBEOS_STATUS_NO_MEMORY),
           "a ceiling below the arena cannot be met (the ZeroBits gap)");
    expect(kf_uaccess_under_mm() == 0u,
           "no store into the program's memory under its own mm lock");
}

/* ---- data sections (W3 step 5) ------------------------------------------------- */

static long sec_create(uint64_t *h, uint64_t oa, uint64_t size, uint32_t protect, uint32_t attributes,
                       uint64_t file) {
    uint64_t out = kf_ualloc(8), sz = 0;
    uint64_t rest[3] = { protect, attributes, file };
    long r;

    if (size != 0u) {
        sz = kf_ualloc(8);
        *(uint64_t *)kf_uptr(sz) = size;
    }
    kf_next_sp(stack_args(rest, 3));
    r = nt(VIBEOS_NT_NR_NtCreateSection, out, 0x000F001Fu, oa, sz, 0);
    *h = *(uint64_t *)kf_uptr(out);
    return r;
}

/* A view: at *base (0: anywhere), from `off` (all ones: no offset pointer),
 * *size bytes (0: to the end); the base and size it got come back. */
static long sec_map(uint64_t sec, uint64_t *base, uint64_t off, uint64_t *size, uint32_t protect) {
    uint64_t b = kf_ualloc(8), s = kf_ualloc(8), o = 0;
    uint64_t rest[6];
    long r;

    if (off != ~0ull) {
        o = kf_ualloc(8);
        *(uint64_t *)kf_uptr(o) = off;
    }
    rest[0] = 0;          /* CommitSize        */
    rest[1] = o;          /* SectionOffset     */
    rest[2] = s;          /* ViewSize          */
    rest[3] = 1;          /* InheritDisposition */
    rest[4] = 0;          /* AllocationType    */
    rest[5] = protect;    /* Win32Protect      */
    *(uint64_t *)kf_uptr(b) = *base;
    *(uint64_t *)kf_uptr(s) = *size;
    kf_next_sp(stack_args(rest, 7));
    r = nt(VIBEOS_NT_NR_NtMapViewOfSection, sec, VIBEOS_NT_CURRENT_PROCESS, b, 0, 0);
    *base = *(uint64_t *)kf_uptr(b);
    *size = *(uint64_t *)kf_uptr(s);
    return r;
}

static long sec_unmap(uint64_t base) {
    return nt(VIBEOS_NT_NR_NtUnmapViewOfSection, VIBEOS_NT_CURRENT_PROCESS, base, 0, 0, 0);
}

static long sec_query(uint64_t sec, uint32_t cls, uint64_t len, vibeos_nt_section_basic_t *out) {
    uint64_t buf = kf_ualloc(32), got = kf_ualloc(8);
    uint64_t rest[1] = { got };
    long r;

    kf_next_sp(stack_args(rest, 1));
    r = nt(VIBEOS_NT_NR_NtQuerySection, sec, cls, buf, len, 0);
    memcpy(out, kf_uptr(buf), sizeof(*out));
    return r;
}

static void t_data_sections(void) {
    vibeos_nt_section_basic_t sb;
    vibeos_nt_memory_basic_t m;
    uint64_t s1, s2, s3, fh, b1 = 0, z1 = 0, b2 = 0, z2 = 0, b3 = 0, z3 = 0, bx, zx, x, xs;
    uint8_t q = 'Q', w = 'W', y = 0;
    char text[12];

    fresh(44);
    /* Memory alone, named: two views of it are one memory. */
    expect(sec_create(&s1, oattr("\\BaseNamedObjects\\vibesec", 0), 0x5000u,
                      VIBEOS_NT_PAGE_READWRITE, 0x08000000u, 0) == ST(VIBEOS_STATUS_SUCCESS),
           "a section of memory alone, named, five pages");
    expect(sec_map(s1, &b1, ~0ull, &z1, VIBEOS_NT_PAGE_READWRITE) == ST(VIBEOS_STATUS_SUCCESS) &&
               sec_map(s1, &b2, 0, &z2, VIBEOS_NT_PAGE_READWRITE) == ST(VIBEOS_STATUS_SUCCESS) &&
               b1 != b2 && (b1 & 0xFFFFu) == 0u && (b2 & 0xFFFFu) == 0u && z1 == 0x5000u && z2 == 0x5000u,
           "two views, at 64 KiB, the whole section each");
    expect(kf_peek(b1 + 0x4FFFu, &y, 1) == 0 && y == 0 && kf_poke(b1 + 0x1234u, &q, 1) == 0 &&
               kf_peek(b2 + 0x1234u, &y, 1) == 0 && y == 'Q',
           "zeros to begin with, and a store through one view is in the other");
    expect(kf_poke(b1 + 0x5000u, &q, 1) != 0, "and nothing past the section's last page");
    expect(vquery(b2 + 0x10u, 0, sizeof(m), &m) == ST(VIBEOS_STATUS_SUCCESS) && m.base_address == b2 &&
               m.allocation_base == b2 && m.type == VIBEOS_NT_MEM_MAPPED &&
               m.state == VIBEOS_NT_MEM_COMMIT && m.protect == VIBEOS_NT_PAGE_READWRITE &&
               m.region_size == 0x5000u,
           "a view is an allocation of its own, mapped memory");

    /* Copy-on-write: the process's own copy, which nobody else sees. */
    expect(sec_map(s1, &b3, ~0ull, &z3, VIBEOS_NT_PAGE_WRITECOPY) == ST(VIBEOS_STATUS_SUCCESS) &&
               kf_peek(b3 + 0x1234u, &y, 1) == 0 && y == 'Q' && kf_poke(b3 + 0x1234u, &w, 1) == 0 &&
               kf_peek(b1 + 0x1234u, &y, 1) == 0 && y == 'Q' &&
               kf_peek(b3 + 0x1234u, &y, 1) == 0 && y == 'W',
           "a copy-on-write view starts with the section's bytes and keeps its stores");

    /* Opened by its name: the same memory. */
    {
        uint64_t out = kf_ualloc(8), bo = 0, zo = 0;

        expect(nt(VIBEOS_NT_NR_NtOpenSection, out, 0x0004u /* SECTION_MAP_READ */,
                  oattr("\\BaseNamedObjects\\vibesec", 0), 0, 0) == ST(VIBEOS_STATUS_SUCCESS),
               "NtOpenSection finds it by its name");
        s2 = *(uint64_t *)kf_uptr(out);
        expect(sec_map(s2, &bo, ~0ull, &zo, VIBEOS_NT_PAGE_READONLY) == ST(VIBEOS_STATUS_SUCCESS) &&
                   kf_peek(bo + 0x1234u, &y, 1) == 0 && y == 'Q' && kf_poke(bo, &q, 1) != 0,
               "a read-only view through it: the same bytes, and no store");
        bx = 0;
        zx = 0;
        expect(sec_map(s2, &bx, ~0ull, &zx, VIBEOS_NT_PAGE_READWRITE) == ST(VIBEOS_STATUS_ACCESS_DENIED),
               "a handle with SECTION_MAP_READ alone maps no writable view");
        expect(sec_unmap(bo) == ST(VIBEOS_STATUS_SUCCESS), "and the view goes");
    }

    /* What a view may not be. */
    bx = 0;
    zx = 0;
    expect(sec_map(s1, &bx, 0x1000u, &zx, VIBEOS_NT_PAGE_READWRITE) ==
               ST(VIBEOS_STATUS_MAPPED_ALIGNMENT), "an offset off 64 KiB");
    zx = 0x6000u;
    expect(sec_map(s1, &bx, 0, &zx, VIBEOS_NT_PAGE_READWRITE) == ST(VIBEOS_STATUS_INVALID_VIEW_SIZE),
           "a view longer than the section");
    bx = b1;
    zx = 0;
    expect(sec_map(s1, &bx, ~0ull, &zx, VIBEOS_NT_PAGE_READWRITE) ==
               ST(VIBEOS_STATUS_CONFLICTING_ADDRESSES), "at an address a view holds");
    bx = 0;
    expect(sec_map(s1, &bx, ~0ull, &zx, VIBEOS_NT_PAGE_EXECUTE_READ) ==
               ST(VIBEOS_STATUS_SECTION_PROTECTION), "executable, of a section that is not");

    expect(sec_query(s1, 0, sizeof(sb), &sb) == ST(VIBEOS_STATUS_SUCCESS) &&
               sb.attributes == 0x08000000u && sb.size == 0x5000u,
           "NtQuerySection: SEC_COMMIT, the size asked for");
    expect(sec_query(s1, 1, 64, &sb) == ST(VIBEOS_STATUS_SECTION_NOT_IMAGE) &&
               sec_query(s1, 0, sizeof(sb) - 1u, &sb) == ST(VIBEOS_STATUS_INFO_LENGTH_MISMATCH),
           "an image's information of a data section, a short buffer");

    /* A read-only section takes no writable view, whatever the handle may do. */
    {
        uint64_t sr = 0, br = 0, zr = 0;

        expect(sec_create(&sr, 0, 0x1000u, VIBEOS_NT_PAGE_READONLY, 0x08000000u, 0) ==
                   ST(VIBEOS_STATUS_SUCCESS) &&
                   sec_map(sr, &br, ~0ull, &zr, VIBEOS_NT_PAGE_READWRITE) ==
                   ST(VIBEOS_STATUS_SECTION_PROTECTION),
               "a writable view of a read-only section, through a handle with every right");
    }
    /* A view in two regions - a protection changed on part of it - still goes
     * whole; private memory is no view. */
    {
        uint32_t old = 0;
        uint64_t pa = 0, pz = 0x1000u;

        expect(vprotect(b2, 0x1000u, VIBEOS_NT_PAGE_READONLY, &old) == ST(VIBEOS_STATUS_SUCCESS) &&
                   old == VIBEOS_NT_PAGE_READWRITE,
               "a protection changed on the first page of a view");
        expect(valloc(&pa, &pz, 0, VIBEOS_NT_MEM_RESERVE | VIBEOS_NT_MEM_COMMIT,
                      VIBEOS_NT_PAGE_READWRITE) == ST(VIBEOS_STATUS_SUCCESS) &&
                   sec_unmap(pa) == ST(VIBEOS_STATUS_NOT_MAPPED_VIEW),
               "NtUnmapViewOfSection of private memory is no view's");
    }

    /* A view is not private memory, and goes whole by its base. */
    x = b1;
    xs = 0;
    expect(vfree(&x, &xs, VIBEOS_NT_MEM_RELEASE) == ST(VIBEOS_STATUS_UNABLE_TO_DELETE_SECTION),
           "NtFreeVirtualMemory does not release a view");
    x = b1;
    xs = 0x1000u;
    expect(valloc(&x, &xs, 0, VIBEOS_NT_MEM_COMMIT, VIBEOS_NT_PAGE_READWRITE) ==
               ST(VIBEOS_STATUS_CONFLICTING_ADDRESSES), "nor commits into one");
    /* Wine's NtUnmapViewOfSection takes the view an address is in, wherever in
     * it, and so does this. */
    expect(sec_unmap(b1 + 0x1000u) == ST(VIBEOS_STATUS_SUCCESS) && kf_peek(b1, &y, 1) != 0 &&
               kf_peek(b1 + 0x4000u, &y, 1) != 0 && sec_unmap(b1) == ST(VIBEOS_STATUS_NOT_MAPPED_VIEW),
           "a view is unmapped whole, from any address in it, once");
    expect(nt(VIBEOS_NT_NR_NtClose, s1, 0, 0, 0, 0) == ST(VIBEOS_STATUS_SUCCESS) &&
               kf_peek(b2 + 0x1234u, &y, 1) == 0 && y == 'Q',
           "the section closed, a view of it keeps its memory");
    expect(sec_unmap(b2 + 0x3000u) == ST(VIBEOS_STATUS_SUCCESS) && kf_peek(b2, &y, 1) != 0 &&
               kf_peek(b2 + 0x3000u, &y, 1) != 0,
           "a view of two regions is unmapped whole, from its second");

    /* A file's: read-only or copy-on-write, of the file's size. */
    kf_fs_add("/win", 0, 0, 1);
    kf_fs_add("/win/text.txt", "twelve bytes", 12, 0);
    kf_fs_add("/win/empty.txt", "", 0, 0);
    expect(open_file(&fh, 0x00120089u, "\\??\\C:\\win\\text.txt") == ST(VIBEOS_STATUS_SUCCESS),
           "a text file opened to read");
    expect(sec_create(&s3, 0, 0, VIBEOS_NT_PAGE_READWRITE, 0x08000000u, fh) ==
               ST(VIBEOS_STATUS_ACCESS_DENIED), "a writable section of a file the handle may not write");
    expect(sec_create(&s3, 0, 0x2000u, VIBEOS_NT_PAGE_READONLY, 0x08000000u, fh) ==
               ST(VIBEOS_STATUS_SECTION_TOO_BIG), "a section longer than a file nobody can grow");
    expect(sec_create(&s3, 0, 0, VIBEOS_NT_PAGE_WRITECOPY, 0x08000000u, fh) == ST(VIBEOS_STATUS_SUCCESS) &&
               sec_query(s3, 0, sizeof(sb), &sb) == ST(VIBEOS_STATUS_SUCCESS) && sb.size == 12u &&
               sb.attributes == (0x08000000u | 0x00800000u),
           "a copy-on-write section of the file: its size, SEC_FILE");
    bx = 0;
    zx = 0;
    expect(sec_map(s3, &bx, ~0ull, &zx, VIBEOS_NT_PAGE_WRITECOPY) == ST(VIBEOS_STATUS_SUCCESS) &&
               zx == 0x1000u && kf_peek(bx, text, 12) == 0 && memcmp(text, "twelve bytes", 12) == 0 &&
               kf_peek(bx + 12u, &y, 1) == 0 && y == 0 && kf_poke(bx, &w, 1) == 0,
           "its view holds the file's bytes, zeros after them, and takes a store of its own");
    {
        uint64_t fe = 0, se = 0;

        expect(open_file(&fe, 0x00120089u, "\\??\\C:\\win\\empty.txt") == ST(VIBEOS_STATUS_SUCCESS) &&
                   sec_create(&se, 0, 0, VIBEOS_NT_PAGE_READONLY, 0x08000000u, fe) ==
                       ST(VIBEOS_STATUS_MAPPED_FILE_SIZE_ZERO),
               "an empty file and no size");
    }
    expect(sec_create(&s3, 0, 0, VIBEOS_NT_PAGE_READWRITE, 0x08000000u, 0) ==
               ST(VIBEOS_STATUS_INVALID_PARAMETER_4) &&
               sec_create(&s3, 0, 0x1000u, VIBEOS_NT_PAGE_NOACCESS, 0x08000000u, 0) ==
               ST(VIBEOS_STATUS_INVALID_PAGE_PROTECTION) &&
               sec_create(&s3, 0, 0, VIBEOS_NT_PAGE_READONLY, 0x01000000u, 0) ==
               ST(VIBEOS_STATUS_INVALID_FILE_FOR_SECTION),
           "memory alone with no size, a section nobody may read, an image of no file");
    expect(kf_uaccess_under_mm() == 0u, "no store into the program's memory under its own mm lock");
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
    t_read_write_file();
    t_file_information();
    t_small_file_services();
    t_query_directory();
    t_names();
    t_create_file();
    t_sections();
    t_open_file_permissions();
    t_object_share();
    t_virtual_memory();
    t_placeholders();
    t_data_sections();
    return g_fail ? -1 : 0;
}
