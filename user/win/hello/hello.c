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
 *  - a service no step has written answers STATUS_NOT_IMPLEMENTED, as Linux's
 *    syscall 1999 answers ENOSYS: the count the boot reports is seen moving. */

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
    /* The line the gate reads, and the count NtWriteFile reports for it. */
    {
        uint64_t params = vibeos_nt_rd64(peb + VIBEOS_PEB_PROCESS_PARAMS);
        uint64_t out = vibeos_nt_rd64(params + VIBEOS_RUPP_STD_OUTPUT);

        if (NtWriteFile(out, 0, 0, 0, &iosb, ok, sizeof(ok) - 1u, 0, 0) != VIBEOS_STATUS_SUCCESS ||
            iosb.information != sizeof(ok) - 1u) {
            return fail("NT_HELLO_FAIL write_file\n");
        }
    }
    return 0;
}
