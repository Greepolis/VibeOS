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

/* ---- sleeping -------------------------------------------------------------------------
 *
 * nanosleep and clock_nanosleep (docs/abi/ L3 step 3; they are L2's, and were
 * taken early because nothing that waits for anything can be tested without
 * them). Until they existed every sleep returned ENOSYS at once, and a C
 * library's usleep and sleep hid that: they return nothing a caller checks, so
 * a program that slept simply did not.
 *
 * The clock is the timer's: a sleep ends on a tick, and never before the time
 * asked for - the tick in progress is not counted, so the wait is the request
 * rounded up to whole ticks plus the rest of the current one. A signal that
 * needs acting on ends it early with EINTR and, for a relative sleep, the time
 * that was left. That EINTR is the program's whatever SA_RESTART says, as on
 * Linux: run again from the start it would sleep the whole time over. */

/* A timespec as ticks, rounded up. -1 if it is not a time. */
int64_t linux_ticks_of(const linux_timespec_t *ts) {
    const uint64_t per = 1000000000ull / ks_hz();

    if (ts->tv_sec < 0 || ts->tv_nsec < 0 || ts->tv_nsec >= 1000000000ll) {
        return -1;
    }
    if ((uint64_t)ts->tv_sec > (~0ull >> 2) / ks_hz()) {
        return (int64_t)(~0ull >> 2);   /* longer than the machine will be up */
    }
    return (int64_t)((uint64_t)ts->tv_sec * ks_hz() + ((uint64_t)ts->tv_nsec + per - 1u) / per);
}

/* Wait until the clock reads `deadline`. 0, or -EINTR with the ticks left. */
static long linux_sleep_until(uint64_t deadline, uint64_t *left) {
    for (;;) {
        uint64_t now = ks_ticks();

        if (now >= deadline) {
            return 0;
        }
        if (ks_current() >= 0 && ks_signal_interrupts(ks_current())) {
            *left = deadline - now;
            return -VIBEOS_EINTR;
        }
        ks_wait_tick();
    }
}

static long linux_sleep(uint64_t clk, uint64_t flags, uint64_t req_uptr, uint64_t rem_uptr) {
    linux_timespec_t req, rem;
    uint64_t deadline, left = 0;
    int64_t ticks;
    long r;

    /* The clocks that count time passing; they are one clock here. A CPU-time
     * clock is not something to sleep on. */
    if (clk != LINUX_CLOCK_REALTIME && clk != LINUX_CLOCK_MONOTONIC && clk != LINUX_CLOCK_BOOTTIME) {
        return -VIBEOS_EINVAL;
    }
    if (flags & ~(uint64_t)LINUX_TIMER_ABSTIME) {
        return -VIBEOS_EINVAL;
    }
    if (vibeos_uaccess_copy(&req, (const void *)(uintptr_t)req_uptr, sizeof(req)) != 0) {
        return -VIBEOS_EFAULT;
    }
    if ((ticks = linux_ticks_of(&req)) < 0) {
        return -VIBEOS_EINVAL;
    }
    if (flags & LINUX_TIMER_ABSTIME) {
        deadline = (uint64_t)ticks;          /* a reading of the clock, not a length */
    } else if (ticks == 0) {
        return 0;
    } else {
        deadline = ks_ticks() + (uint64_t)ticks + 1u;
    }
    r = linux_sleep_until(deadline, &left);
    if (r == -VIBEOS_EINTR && rem_uptr != 0u && !(flags & LINUX_TIMER_ABSTIME)) {
        rem.tv_sec = (int64_t)(left / ks_hz());
        rem.tv_nsec = (int64_t)((left % ks_hz()) * (1000000000ull / ks_hz()));
        if (vibeos_uaccess_copy((void *)(uintptr_t)rem_uptr, &rem, sizeof(rem)) != 0) {
            return -VIBEOS_EFAULT;
        }
    }
    return r;
}

static long linux_sys_nanosleep(uint64_t req_uptr, uint64_t rem_uptr) {
    return linux_sleep(LINUX_CLOCK_MONOTONIC, 0, req_uptr, rem_uptr);
}

static long linux_sys_clock_nanosleep(uint64_t clk, uint64_t flags, uint64_t req_uptr,
                                      uint64_t rem_uptr) {
    return linux_sleep(clk, flags, req_uptr, rem_uptr);
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
    X(35,  nanosleep,     NANOSLEEP,     PTRS(IN(0, 16), OUT_OPT(1, 16)), linux_sys_nanosleep(ARG(0), ARG(1))) \
    X(228, clock_gettime, CLOCK_GETTIME, PTRS(OUT(1, 16)), linux_sys_clock_gettime(ARG(0), ARG(1))) \
    X(230, clock_nanosleep, CLOCK_NANOSLEEP, PTRS(IN(2, 16), OUT_OPT(3, 16)), linux_sys_clock_nanosleep(ARG(0), ARG(1), ARG(2), ARG(3)))

LINUX_DEFINE_SYSCALLS(misc, LINUX_MISC_SYSCALLS)
