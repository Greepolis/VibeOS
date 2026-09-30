/* Linux ABI: uname, clock_gettime, time, sysinfo.
 *
 * Lifted out of arch_hw.c (C4 stage 3). Nothing here is new: the handlers and the
 * helpers only they use, moved as they were. */

#include "linux_internal.h"

/* uname(): six fixed 65-byte fields, in order. Programs branch on the release
 * string, so it carries a real version number rather than a placeholder. */
static long linux_sys_uname(uint64_t buf) {
    static const char *const fields[6] = {
        "Linux",            /* sysname: the ABI implemented here, which is    */
                            /* what the question is actually about            */
        "vibeos",           /* nodename   */
        "6.1.0-vibeos",     /* release    */
        "VibeOS",           /* version    */
        "x86_64",           /* machine    */
        "(none)"            /* domainname */
    };
    /* Built in the kernel and copied out once (M-052): the fields were written
     * into the user's buffer directly, which faults in ring 0 if a sibling
     * unmaps it after the dispatcher's range check. */
    linux_utsname_t out;
    char *const dsts[6] = { out.sysname, out.nodename, out.release,
                            out.version, out.machine, out.domainname };
    uint32_t f, i;

    for (f = 0; f < 6u; f++) {
        char *dst = dsts[f];
        const char *src = fields[f];
        for (i = 0; i < 65u; i++) {
            dst[i] = (i < 64u) ? src[i] : 0;
            if (!dst[i]) {
                break;
            }
        }
        for (; i < 65u; i++) {
            dst[i] = 0;
        }
    }
    return vibeos_uaccess_copy((void *)(uintptr_t)buf, &out, sizeof(out)) == 0
               ? 0 : -VIBEOS_EFAULT;
}

/* clock_gettime(): derived from the timer tick, so it advances at the
 * resolution the timer really has rather than pretending to a nanosecond
 * accuracy it does not possess. */
static long linux_sys_clock_gettime(uint64_t clk, uint64_t ts_uptr) {
    uint64_t ticks = ks_ticks();
    linux_timespec_t kts;

    (void)clk;   /* monotonic and realtime are one clock here: uptime */
    kts.tv_sec = (int64_t)(ticks / ks_hz());
    kts.tv_nsec = (int64_t)((ticks % ks_hz()) * (1000000000ull / ks_hz()));
    /* Built in the kernel and copied out: a sibling munmap between the check
     * and the write would fault in ring 0 (H-026). */
    if (vibeos_uaccess_copy((void *)(uintptr_t)ts_uptr, &kts, sizeof(kts)) != 0) {
        return -VIBEOS_EFAULT;
    }
    return 0;
}

static long linux_sys_time(uint64_t tptr) {
    uint64_t secs = ks_ticks() / ks_hz();

    if (tptr != 0u) {
        if (vibeos_uaccess_copy((void *)(uintptr_t)tptr, &secs, sizeof(secs)) != 0) {
            return -VIBEOS_EFAULT;
        }
    }
    return (long)secs;
}

/* sysinfo(): how much memory there is and how much is free, in Linux's layout.
 *
 * Added for svc-reclaim, which has to know where the low watermark is to reach
 * it without running the machine into its minimum; BusyBox's `free` asks the
 * same question the same way. Free is the frame layer's free count: the page
 * cache is not counted as free, as Linux counts it in bufferram instead. */
static long linux_sys_sysinfo(uint64_t buf) {
    linux_sysinfo_t si;
    uint8_t *raw = (uint8_t *)&si;
    const vibeos_mm_stats_t *st = vibeos_mm_stats();
    uint64_t slots = (uint64_t)vibeos_swap_slots();
    uint64_t used = vibeos_swap_stats()->allocated;
    uint32_t i;

    for (i = 0; i < sizeof(si); i++) {
        raw[i] = 0;   /* padding included: it is copied out */
    }
    si.uptime = (int64_t)(ks_ticks() / ks_hz());
    si.totalram = st->frames_total * 4096ull;
    si.freeram = st->frames_free * 4096ull;
    si.totalswap = slots * 4096ull;
    si.freeswap = (used < slots ? slots - used : 0ull) * 4096ull;
    si.mem_unit = 1u;                                   /* bytes */
    return vibeos_uaccess_copy((void *)(uintptr_t)buf, &si, sizeof(si)) == 0
               ? 0 : -VIBEOS_EFAULT;
}

/* ---- the syscalls this file implements --------------------------------------- */
#define LINUX_MISC_SYSCALLS(X) \
    X(63,  uname,         UNAME,         PTRS(OUT(0, 6u * 65u)), linux_sys_uname(ARG(0))) \
    X(99,  sysinfo,       SYSINFO,       PTRS(OUT(0, 112)), linux_sys_sysinfo(ARG(0))) \
    X(201, time,          TIME,          PTRS(OUT_OPT(0, 8)), linux_sys_time(ARG(0))) \
    X(228, clock_gettime, CLOCK_GETTIME, PTRS(OUT(1, 16)), linux_sys_clock_gettime(ARG(0), ARG(1)))

LINUX_DEFINE_SYSCALLS(misc, LINUX_MISC_SYSCALLS)
