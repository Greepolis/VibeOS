/* Linux ABI: uname, clock_gettime, time.
 *
 * Lifted out of arch_hw.c (C4 stage 3). Nothing here is new: the handlers and the
 * helpers only they use, moved as they were. */

#include "linux_internal.h"

/* uname(): six fixed 65-byte fields, in order. Programs branch on the release
 * string, so it carries a real version number rather than a placeholder. */
long hw_sys_uname(uint64_t buf) {
    static const char *const fields[6] = {
        "Linux",            /* sysname: the ABI implemented here, which is    */
                            /* what the question is actually about            */
        "vibeos",           /* nodename   */
        "6.1.0-vibeos",     /* release    */
        "VibeOS",           /* version    */
        "x86_64",           /* machine    */
        "(none)"            /* domainname */
    };
    uint32_t f, i;

    if (!hw_user_range_ok(buf, 6u * 65u, 1)) {
        return -VIBEOS_EFAULT;
    }
    for (f = 0; f < 6u; f++) {
        char *dst = (char *)(uintptr_t)(buf + (uint64_t)f * 65u);
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
    return 0;
}

/* clock_gettime(): derived from the timer tick, so it advances at the
 * resolution the timer really has rather than pretending to a nanosecond
 * accuracy it does not possess. */
long hw_sys_clock_gettime(uint64_t clk, uint64_t ts_uptr) {
    uint64_t ticks = g_timer_ticks;
    uint64_t kts[2];

    (void)clk;   /* monotonic and realtime are one clock here: uptime */
    if (!hw_user_range_ok(ts_uptr, 16, 1)) {
        return -VIBEOS_EFAULT;
    }
    kts[0] = ticks / VIBEOS_HW_TIMER_HZ;
    kts[1] = (ticks % VIBEOS_HW_TIMER_HZ) * (1000000000ull / VIBEOS_HW_TIMER_HZ);
    /* Built in the kernel and copied out: a sibling munmap between the check
     * and the write would fault in ring 0 (H-026). */
    if (vibeos_uaccess_copy((void *)(uintptr_t)ts_uptr, kts, sizeof(kts)) != 0) {
        return -VIBEOS_EFAULT;
    }
    return 0;
}

long hw_sys_time(uint64_t tptr) {
    uint64_t secs = g_timer_ticks / VIBEOS_HW_TIMER_HZ;

    if (tptr != 0u) {
        if (!hw_user_range_ok(tptr, 8, 1)) {
            return -VIBEOS_EFAULT;
        }
        if (vibeos_uaccess_copy((void *)(uintptr_t)tptr, &secs, sizeof(secs)) != 0) {
            return -VIBEOS_EFAULT;
        }
    }
    return (long)secs;
}
