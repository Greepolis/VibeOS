/* The first Windows program VibeOS runs (docs/abi/windows.md W1).
 *
 * A PE32+ console executable built by mingw-w64, importing only our ntdll. It
 * asks each W1 service a question whose answer it can check, and prints one
 * line on the console through NtWriteFile: NT_HELLO_OK when every answer was
 * right, NT_HELLO_FAIL and the first that was not otherwise. The boot gate
 * reads that line - a program that merely started would print nothing at all.
 *
 * What each check is for:
 *  - %gs:0x30 is the TEB and points to itself: the kernel's swapgs, and the TEB
 *    it built. gs:0x60 is the PEB, which is where ntdll found this program.
 *  - ProcessBasicInformation names the same PEB and a process id: the kernel's
 *    process and the block it mapped agree.
 *  - the system time is after 1970, in 1601's count: the epoch conversion.
 *  - the performance counter runs at the frequency it says.
 *  - SystemBasicInformation says a page is 4096 bytes and there is a processor.
 *  - memory from NtAllocateVirtualMemory is there on first touch, for this
 *    program's store and for the kernel's into a page nobody touched; a
 *    reservation is committed a page at a time, protected, decommitted and
 *    released, each as NtQueryVirtualMemory reports it (W3).
 *  - a section of memory alone has two views that are one memory, and both
 *    unmap (W3 step 5).
 *  - a service no step has written answers STATUS_NOT_IMPLEMENTED, as Linux's
 *    syscall 1999 answers ENOSYS: the count the boot reports is seen moving.
 *  - an absolute address the linker wrote into read-only data points where the
 *    code says the object is: the program is linked at mingw's default base,
 *    0x140000000 - Wine's test programs' too - which is no user address here,
 *    so the kernel moves it and applies its base relocations (W2). */

#include "vibeos_ntdll.h"

static uint32_t g_len(const char *s) {
    uint32_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

static void say(const char *line) {
    uint64_t teb = vibeos_nt_teb();
    uint64_t peb = vibeos_nt_rd64(teb + VIBEOS_TEB_PEB);
    uint64_t params = vibeos_nt_rd64(peb + VIBEOS_PEB_PROCESS_PARAMS);
    uint64_t out = vibeos_nt_rd64(params + VIBEOS_RUPP_STD_OUTPUT);
    vibeos_nt_iosb_t iosb;

    (void)NtWriteFile(out, 0, 0, 0, &iosb, line, g_len(line), 0, 0);
}

static int fail(const char *line) {
    say(line);
    return 1;
}

/* From our msvcrt.dll, which ntdll's loader brings in from /win (W2). */
__declspec(dllimport) int printf(const char *format, ...);
__declspec(dllimport) void *malloc(size_t n);
__declspec(dllimport) void free(void *p);

static int g_target;
/* Written by the linker as an absolute address; right only once relocated.
 * volatile, so the compiler reads it rather than folding the comparison. */
static int *const volatile g_target_ptr = &g_target;

/* Virtual memory (W3): committed memory made on first touch - by this program's
 * store, and by the kernel's, which has to accept a page nobody has touched yet
 * - a reservation committed a page at a time, a protection changed, a decommit
 * and a release, each checked through NtQueryVirtualMemory. Null, or the
 * failure line. */
static const char *vm_check(void) {
    vibeos_nt_memory_basic_t m;
    volatile uint8_t *p;
    uint64_t base = 0, size = 0x10000u, got = 0, b2 = 0, s2 = 0x10000u, c, cs;
    uint32_t old = 0;
    int64_t *t;

    if (NtAllocateVirtualMemory(VIBEOS_NT_CURRENT_PROCESS, &base, 0, &size,
                                VIBEOS_NT_MEM_RESERVE | VIBEOS_NT_MEM_COMMIT,
                                VIBEOS_NT_PAGE_READWRITE) != VIBEOS_STATUS_SUCCESS ||
        base == 0u || (base & 0xFFFFu) != 0u || size != 0x10000u) {
        return "NT_HELLO_FAIL vm_allocate\n";
    }
    p = (volatile uint8_t *)(uintptr_t)base;
    p[0x3123] = 0x5Au;
    if (p[0x3123] != 0x5Au || p[0x5000] != 0u) {
        return "NT_HELLO_FAIL vm_first_touch\n";
    }
    /* The kernel stores into a committed page nobody has touched. */
    t = (int64_t *)(uintptr_t)(base + 0x8000u);
    if (NtQuerySystemTime(t) != VIBEOS_STATUS_SUCCESS || (uint64_t)*t < VIBEOS_NT_EPOCH_DELTA) {
        return "NT_HELLO_FAIL vm_kernel_store\n";
    }
    if (NtQueryVirtualMemory(VIBEOS_NT_CURRENT_PROCESS, base + 0x10u, VIBEOS_NT_MEMORY_BASIC_INFORMATION,
                             &m, sizeof(m), &got) != VIBEOS_STATUS_SUCCESS || got != sizeof(m) ||
        m.base_address != base || m.allocation_base != base || m.region_size != 0x10000u ||
        m.state != VIBEOS_NT_MEM_COMMIT || m.protect != VIBEOS_NT_PAGE_READWRITE ||
        m.type != VIBEOS_NT_MEM_PRIVATE) {
        return "NT_HELLO_FAIL vm_query\n";
    }
    c = base;
    cs = 0x1000u;
    if (NtProtectVirtualMemory(VIBEOS_NT_CURRENT_PROCESS, &c, &cs, VIBEOS_NT_PAGE_READONLY, &old) !=
            VIBEOS_STATUS_SUCCESS || old != VIBEOS_NT_PAGE_READWRITE ||
        NtQueryVirtualMemory(VIBEOS_NT_CURRENT_PROCESS, base, VIBEOS_NT_MEMORY_BASIC_INFORMATION,
                             &m, sizeof(m), &got) != VIBEOS_STATUS_SUCCESS ||
        m.protect != VIBEOS_NT_PAGE_READONLY || m.region_size != 0x1000u) {
        return "NT_HELLO_FAIL vm_protect\n";
    }
    /* A reservation, then one page of it. */
    if (NtAllocateVirtualMemory(VIBEOS_NT_CURRENT_PROCESS, &b2, 0, &s2, VIBEOS_NT_MEM_RESERVE,
                                VIBEOS_NT_PAGE_READWRITE) != VIBEOS_STATUS_SUCCESS ||
        b2 == base || (b2 & 0xFFFFu) != 0u) {
        return "NT_HELLO_FAIL vm_reserve\n";
    }
    c = b2 + 0x4000u;
    cs = 0x10u;
    if (NtAllocateVirtualMemory(VIBEOS_NT_CURRENT_PROCESS, &c, 0, &cs, VIBEOS_NT_MEM_COMMIT,
                                VIBEOS_NT_PAGE_READWRITE) != VIBEOS_STATUS_SUCCESS ||
        c != b2 + 0x4000u || cs != 0x1000u) {
        return "NT_HELLO_FAIL vm_commit\n";
    }
    p = (volatile uint8_t *)(uintptr_t)c;
    p[0xFFF] = 0xA5u;
    if (p[0xFFF] != 0xA5u ||
        NtQueryVirtualMemory(VIBEOS_NT_CURRENT_PROCESS, b2, VIBEOS_NT_MEMORY_BASIC_INFORMATION,
                             &m, sizeof(m), &got) != VIBEOS_STATUS_SUCCESS ||
        m.state != VIBEOS_NT_MEM_RESERVE || m.region_size != 0x4000u) {
        return "NT_HELLO_FAIL vm_reserved_query\n";
    }
    cs = 0x1000u;
    if (NtFreeVirtualMemory(VIBEOS_NT_CURRENT_PROCESS, &c, &cs, VIBEOS_NT_MEM_DECOMMIT) !=
            VIBEOS_STATUS_SUCCESS ||
        NtQueryVirtualMemory(VIBEOS_NT_CURRENT_PROCESS, c, VIBEOS_NT_MEMORY_BASIC_INFORMATION,
                             &m, sizeof(m), &got) != VIBEOS_STATUS_SUCCESS ||
        m.state != VIBEOS_NT_MEM_RESERVE) {
        return "NT_HELLO_FAIL vm_decommit\n";
    }
    s2 = 0;
    size = 0;
    if (NtFreeVirtualMemory(VIBEOS_NT_CURRENT_PROCESS, &b2, &s2, VIBEOS_NT_MEM_RELEASE) !=
            VIBEOS_STATUS_SUCCESS || s2 != 0x10000u ||
        NtFreeVirtualMemory(VIBEOS_NT_CURRENT_PROCESS, &base, &size, VIBEOS_NT_MEM_RELEASE) !=
            VIBEOS_STATUS_SUCCESS ||
        NtQueryVirtualMemory(VIBEOS_NT_CURRENT_PROCESS, base, VIBEOS_NT_MEMORY_BASIC_INFORMATION,
                             &m, sizeof(m), &got) != VIBEOS_STATUS_SUCCESS ||
        m.state != VIBEOS_NT_MEM_FREE) {
        return "NT_HELLO_FAIL vm_release\n";
    }
    return 0;
}

/* Sections (W3 step 5): one of memory alone, two views of it, and a store
 * through one read back through the other - the views are one memory - then
 * NtQuerySection's size and both views unmapped. Null, or the failure line. */
static const char *sec_check(void) {
    vibeos_nt_section_basic_t sb;
    volatile uint8_t *a, *b;
    uint64_t sec = 0, ba = 0, sa = 0, bb = 0, sz = 0, got = 0;
    int64_t size = 0x3000, off = 0;

    if (NtCreateSection(&sec, 0x000F001Fu, 0, &size, VIBEOS_NT_PAGE_READWRITE, 0x08000000u, 0) !=
        VIBEOS_STATUS_SUCCESS) {
        return "NT_HELLO_FAIL sec_create\n";
    }
    if (NtMapViewOfSection(sec, VIBEOS_NT_CURRENT_PROCESS, &ba, 0, 0, &off, &sa, 1, 0,
                           VIBEOS_NT_PAGE_READWRITE) != VIBEOS_STATUS_SUCCESS ||
        NtMapViewOfSection(sec, VIBEOS_NT_CURRENT_PROCESS, &bb, 0, 0, 0, &sz, 1, 0,
                           VIBEOS_NT_PAGE_READWRITE) != VIBEOS_STATUS_SUCCESS ||
        ba == bb || sa != 0x3000u || sz != 0x3000u) {
        return "NT_HELLO_FAIL sec_map\n";
    }
    a = (volatile uint8_t *)(uintptr_t)ba;
    b = (volatile uint8_t *)(uintptr_t)bb;
    a[0x2345] = 0x6Bu;
    if (b[0x2345] != 0x6Bu || b[0x1000] != 0u) {
        return "NT_HELLO_FAIL sec_shared\n";
    }
    if (NtQuerySection(sec, VIBEOS_NT_SECTION_BASIC_INFORMATION, &sb, sizeof(sb), &got) !=
            VIBEOS_STATUS_SUCCESS || sb.size != 0x3000u || got != sizeof(sb)) {
        return "NT_HELLO_FAIL sec_query\n";
    }
    if (NtUnmapViewOfSection(VIBEOS_NT_CURRENT_PROCESS, ba) != VIBEOS_STATUS_SUCCESS ||
        NtUnmapViewOfSection(VIBEOS_NT_CURRENT_PROCESS, bb + 0x1000u) != VIBEOS_STATUS_SUCCESS ||
        NtUnmapViewOfSection(VIBEOS_NT_CURRENT_PROCESS, ba) != VIBEOS_STATUS_NOT_MAPPED_VIEW ||
        NtClose(sec) != VIBEOS_STATUS_SUCCESS) {
        return "NT_HELLO_FAIL sec_unmap\n";
    }
    return 0;
}

/* A second thread (W5 step 1): it runs, on its own TEB, in this process's
 * memory - it stores what it was given where the first thread reads it - and
 * its handle is signalled when it returns. */
static volatile uint64_t g_thread_saw;
static volatile uint64_t g_thread_teb;

static uint32_t thread_routine(void *param) {
    g_thread_teb = vibeos_nt_teb();
    g_thread_saw = (uint64_t)(uintptr_t)param;
    return 42u;
}

static const char *thread_check(void) {
    uint64_t th = 0;

    if (NtCreateThreadEx(&th, 0x001FFFFFu, 0, VIBEOS_NT_CURRENT_PROCESS, (void *)thread_routine,
                         (void *)(uintptr_t)0x5EEDu, 0, 0, 0, 0, 0) != VIBEOS_STATUS_SUCCESS) {
        return "NT_HELLO_FAIL thread_create\n";
    }
    if (NtWaitForSingleObject(th, 0, 0) != VIBEOS_STATUS_SUCCESS) {
        return "NT_HELLO_FAIL thread_wait\n";
    }
    if (g_thread_saw != 0x5EEDu) {
        return "NT_HELLO_FAIL thread_ran\n";
    }
    if (g_thread_teb == 0u || g_thread_teb == vibeos_nt_teb() ||
        vibeos_nt_rd64(g_thread_teb + VIBEOS_TEB_SELF) != g_thread_teb) {
        return "NT_HELLO_FAIL thread_teb\n";
    }
    if (NtClose(th) != VIBEOS_STATUS_SUCCESS) {
        return "NT_HELLO_FAIL thread_close\n";
    }
    /* W5 step 3: made suspended, it does not run until it is resumed. */
    {
        int64_t wait20 = -200000;   /* 20 ms */
        uint32_t prev = 7;

        g_thread_saw = 0;
        if (NtCreateThreadEx(&th, 0x001FFFFFu, 0, VIBEOS_NT_CURRENT_PROCESS, (void *)thread_routine,
                             (void *)(uintptr_t)0xC0DEu, 1u /* CREATE_SUSPENDED */, 0, 0, 0, 0) !=
            VIBEOS_STATUS_SUCCESS) {
            return "NT_HELLO_FAIL thread_suspended_create\n";
        }
        (void)NtDelayExecution(0, &wait20);
        if (g_thread_saw != 0u) {
            return "NT_HELLO_FAIL thread_ran_suspended\n";
        }
        if (NtResumeThread(th, &prev) != VIBEOS_STATUS_SUCCESS || prev != 1u ||
            NtWaitForSingleObject(th, 0, 0) != VIBEOS_STATUS_SUCCESS || g_thread_saw != 0xC0DEu) {
            return "NT_HELLO_FAIL thread_resume\n";
        }
        (void)NtClose(th);
    }
    /* W5 step 7: a synchronization timer fires once, for one waiter. */
    {
        uint64_t tm = 0;
        int64_t due = -100000, none = 0;   /* 10 ms; and no wait at all */

        if (NtCreateTimer(&tm, 0x001F0003u, 0, 1u) != VIBEOS_STATUS_SUCCESS ||
            NtSetTimer(tm, &due, 0, 0, 0, 0, 0) != VIBEOS_STATUS_SUCCESS) {
            return "NT_HELLO_FAIL timer_set\n";
        }
        if (NtWaitForSingleObject(tm, 0, 0) != VIBEOS_STATUS_SUCCESS ||
            NtWaitForSingleObject(tm, 0, &none) != VIBEOS_STATUS_TIMEOUT) {
            return "NT_HELLO_FAIL timer_fire\n";
        }
        (void)NtClose(tm);
    }
    return 0;
}

int nt_hello_start(void) {
    uint64_t teb = vibeos_nt_teb();
    uint64_t peb;
    vibeos_nt_process_basic_t pbi;
    vibeos_nt_system_basic_t sbi;
    vibeos_nt_iosb_t iosb;
    int64_t now = 0, count = 0, freq = 0, again = 0;
    uint32_t got = 0;
    static const char ok[] = "NT_HELLO_OK: a PE program on our ntdll, TEB, PEB and the W1 services\n";

    if (teb == 0u || vibeos_nt_rd64(teb + VIBEOS_TEB_SELF) != teb) {
        return fail("NT_HELLO_FAIL teb\n");
    }
    if (g_target_ptr != &g_target) {
        return fail("NT_HELLO_FAIL relocation\n");
    }
    peb = vibeos_nt_rd64(teb + VIBEOS_TEB_PEB);
    if (peb == 0u || vibeos_nt_rd64(peb + VIBEOS_PEB_PROCESS_PARAMS) == 0u) {
        return fail("NT_HELLO_FAIL peb\n");
    }
    if (NtQueryInformationProcess(VIBEOS_NT_CURRENT_PROCESS, VIBEOS_NT_PROCESS_BASIC_INFORMATION,
                                  &pbi, sizeof(pbi), &got) != VIBEOS_STATUS_SUCCESS ||
        got != sizeof(pbi) || pbi.peb != peb || pbi.unique_process_id == 0u) {
        return fail("NT_HELLO_FAIL process_basic\n");
    }
    if (NtQueryInformationProcess(VIBEOS_NT_CURRENT_PROCESS, VIBEOS_NT_PROCESS_BASIC_INFORMATION,
                                  &pbi, 8u, &got) != VIBEOS_STATUS_INFO_LENGTH_MISMATCH) {
        return fail("NT_HELLO_FAIL process_basic_short\n");
    }
    if (NtQuerySystemTime(&now) != VIBEOS_STATUS_SUCCESS ||
        (uint64_t)now < VIBEOS_NT_EPOCH_DELTA) {
        return fail("NT_HELLO_FAIL system_time\n");
    }
    if (NtQueryPerformanceCounter(&count, &freq) != VIBEOS_STATUS_SUCCESS ||
        (uint64_t)freq != VIBEOS_NT_PERF_FREQUENCY ||
        NtQueryPerformanceCounter(&again, 0) != VIBEOS_STATUS_SUCCESS || again < count) {
        return fail("NT_HELLO_FAIL performance_counter\n");
    }
    if (NtQuerySystemInformation(VIBEOS_NT_SYSTEM_BASIC_INFORMATION, &sbi, sizeof(sbi), &got) !=
            VIBEOS_STATUS_SUCCESS ||
        sbi.page_size != 4096u || sbi.processors == 0u || got != sizeof(sbi)) {
        return fail("NT_HELLO_FAIL system_basic\n");
    }
    if (NtYieldExecution() != VIBEOS_STATUS_NOT_IMPLEMENTED) {
        return fail("NT_HELLO_FAIL not_implemented\n");
    }
    {
        const char *why = vm_check();
        if (!why) {
            why = sec_check();
        }
        if (!why) {
            why = thread_check();
        }
        if (why) {
            return fail(why);
        }
    }
    /* The line the gate reads, and the count NtWriteFile reports for it. */
    {
        uint64_t params = vibeos_nt_rd64(peb + VIBEOS_PEB_PROCESS_PARAMS);
        uint64_t out = vibeos_nt_rd64(params + VIBEOS_RUPP_STD_OUTPUT);

        if (NtWriteFile(out, 0, 0, 0, &iosb, ok, sizeof(ok) - 1u, 0, 0) != VIBEOS_STATUS_SUCCESS ||
            iosb.information != sizeof(ok) - 1u) {
            return fail("NT_HELLO_FAIL write_file\n");
        }
    }
    /* A second DLL, loaded from the disk, linked and initialised: its printf
     * formats and writes through the same NtWriteFile. */
    printf("NT_DLL_OK: msvcrt printf %d %s\n", 42, "loaded");
    /* The process heap grows (W3 step 3): three megabytes from malloc, past
     * anything the old arenas held, and malloc's memory is the process
     * heap's - HeapFree takes it. The small block comes first: the heap's
     * first segment is sized to its first request, so three megabytes asked
     * for first would never need a second one - the sabotage of growth went
     * NOT RED that way. */
    {
        void *small = malloc(100);
        volatile uint8_t *big = malloc((size_t)3 << 20);
        void *ph = (void *)(uintptr_t)vibeos_nt_rd64(peb + VIBEOS_PEB_PROCESS_HEAP);

        if (!big || !small) {
            return fail("NT_HELLO_FAIL heap_grow\n");
        }
        big[0] = 1;
        big[((size_t)3 << 20) - 1u] = 2;
        if (big[0] != 1 || big[((size_t)3 << 20) - 1u] != 2 || !RtlFreeHeap(ph, 0, small)) {
            return fail("NT_HELLO_FAIL heap_shared\n");
        }
        free((void *)big);
        printf("NT_HEAP_OK: the process heap grew to hold %d MiB, and malloc is HeapAlloc\n", 3);
    }
    return 0;
}
