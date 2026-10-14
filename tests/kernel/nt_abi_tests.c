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
    expect(vibeos_abi_nt()->lookup(VIBEOS_NT_NR_NtClose) == 0, "a MISSING service has no row");
}

static void t_dispatch_refusals(void) {
    uint64_t un, ref;

    fresh(31);
    un = g_nt_unimplemented;
    ref = g_nt_refused;
    expect(nt(VIBEOS_NT_SERVICES + 5u, 0, 0, 0, 0, 0) == ST(VIBEOS_STATUS_INVALID_SYSTEM_SERVICE),
           "a number past the table names no service");
    expect(nt(VIBEOS_NT_NR_NtClose, 4, 0, 0, 0, 0) == ST(VIBEOS_STATUS_NOT_IMPLEMENTED) &&
           g_nt_unimplemented == un + 1u && g_nt_last_nr == (uint64_t)VIBEOS_NT_NR_NtClose &&
           strstr(kf_console(), "unimplemented NT service") &&
           strstr(kf_console(), "NtClose"),
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
           rd16(peb + VIBEOS_PEB_OS_BUILD) == 19045u,
           "the PEB: the program, the parameters, Windows 10");
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
    return g_fail ? -1 : 0;
}
