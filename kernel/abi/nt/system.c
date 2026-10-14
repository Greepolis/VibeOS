/* NT personality: the system services of W1 (docs/abi/windows.md).
 *
 * The time, the performance counter, and SystemBasicInformation - what a
 * process's start and a C runtime ask before main. Every one is read from the
 * kernel's own clock and memory accounting, the same numbers Linux's
 * clock_gettime and sysinfo report, so a program on either personality sees one
 * machine. */

#include "nt_internal.h"
#include "vibeos/mm_stats.h"

/* The clock in 100-ns units, the unit of every NT time. It advances a tick at a
 * time, as Linux's clocks do here: a resolution the timer has, not one it
 * pretends to. */
static uint64_t nt_now_100ns(void) {
    return ks_ticks() * (VIBEOS_NT_PERF_FREQUENCY / (uint64_t)ks_hz());
}

static long nt_put64(uint64_t uptr, uint64_t v) {
    return vibeos_uaccess_copy((void *)(uintptr_t)uptr, &v, sizeof(v)) == 0
               ? NT_STATUS(VIBEOS_STATUS_SUCCESS)
               : NT_STATUS(VIBEOS_STATUS_ACCESS_VIOLATION);
}

/* NtQuerySystemTime(SystemTime): 100-ns intervals since 1601. The kernel's
 * realtime clock counts from 1970 - from boot, today, with no clock chip read
 * - and this is that clock moved to Windows' epoch, so the two personalities
 * agree to the tick about what time it is. */
static long nt_query_system_time(uint64_t out) {
    return nt_put64(out, VIBEOS_NT_EPOCH_DELTA + nt_now_100ns());
}

/* NtQueryPerformanceCounter(Counter, Frequency): a counter that never goes
 * backwards and the rate it runs at, 10 MHz - Windows' own rate on current
 * machines, and this clock's unit. The frequency pointer may be null. */
static long nt_query_performance_counter(uint64_t counter, uint64_t freq) {
    long st = nt_put64(counter, nt_now_100ns());

    if (st == NT_STATUS(VIBEOS_STATUS_SUCCESS) && freq != 0u) {
        st = nt_put64(freq, VIBEOS_NT_PERF_FREQUENCY);
    }
    return st;
}

/* NtQuerySystemInformation(Class, Buffer, Length, ReturnLength). W1 answers
 * SystemBasicInformation; any other class is STATUS_INVALID_INFO_CLASS, and a
 * length other than the structure's STATUS_INFO_LENGTH_MISMATCH with nothing
 * written. The user address range is Windows' own until W3 decides where an NT
 * process's allocations go. */
static long nt_query_system_information(uint64_t cls, uint64_t buf, uint64_t len,
                                        uint64_t ret_len) {
    vibeos_nt_system_basic_t sbi;
    const vibeos_mm_stats_t *st = vibeos_mm_stats();
    uint32_t got = (uint32_t)sizeof(sbi);
    uint32_t mask = ks_cpus_online_mask();
    uint32_t i, n = 0;

    if ((uint32_t)cls != VIBEOS_NT_SYSTEM_BASIC_INFORMATION) {
        return NT_STATUS(VIBEOS_STATUS_INVALID_INFO_CLASS);
    }
    if ((uint32_t)len != sizeof(sbi)) {
        return NT_STATUS(VIBEOS_STATUS_INFO_LENGTH_MISMATCH);
    }
    for (i = 0; i < sizeof(sbi); i++) {
        ((uint8_t *)&sbi)[i] = 0;
    }
    for (i = 0; i < 32u; i++) {
        n += (mask >> i) & 1u;
    }
    sbi.timer_resolution = (uint32_t)(VIBEOS_NT_PERF_FREQUENCY / (uint64_t)ks_hz());
    sbi.page_size = 4096u;
    sbi.physical_pages = (uint32_t)st->frames_total;
    sbi.lowest_physical_page = 1u;
    sbi.highest_physical_page = (uint32_t)st->frames_total;
    sbi.allocation_granularity = 0x10000u;
    sbi.minimum_user_address = 0x10000u;
    sbi.maximum_user_address = 0x00007FFFFFFEFFFFull;
    sbi.active_processors_mask = mask;
    sbi.processors = (uint8_t)n;
    if (vibeos_uaccess_copy((void *)(uintptr_t)buf, &sbi, sizeof(sbi)) != 0) {
        return NT_STATUS(VIBEOS_STATUS_ACCESS_VIOLATION);
    }
    if (ret_len != 0u &&
        vibeos_uaccess_copy((void *)(uintptr_t)ret_len, &got, sizeof(got)) != 0) {
        return NT_STATUS(VIBEOS_STATUS_ACCESS_VIOLATION);
    }
    return NT_STATUS(VIBEOS_STATUS_SUCCESS);
}

#define NT_SYSTEM_SERVICES(X) \
    X(VIBEOS_NT_NR_NtQuerySystemTime, NtQuerySystemTime, CLOCK_GETTIME, \
      PTRS(OUT(0, 8)), nt_query_system_time(ARG(0))) \
    X(VIBEOS_NT_NR_NtQueryPerformanceCounter, NtQueryPerformanceCounter, CLOCK_GETTIME, \
      PTRS(OUT(0, 8), OUT_OPT(1, 8)), nt_query_performance_counter(ARG(0), ARG(1))) \
    X(VIBEOS_NT_NR_NtQuerySystemInformation, NtQuerySystemInformation, SYSINFO, \
      PTRS(NT_OUT_BUF(1, 2), OUT_OPT(3, 4)), \
      nt_query_system_information(NT_ARG32(0), ARG(1), NT_ARG32(2), ARG(3)))

NT_DEFINE_SERVICES(system, NT_SYSTEM_SERVICES)
