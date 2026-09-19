#ifndef VIBEOS_CEILDIV_H
#define VIBEOS_CEILDIV_H

#include <stdint.h>

/* ceil(n / unit) that cannot wrap.
 *
 * (n + unit - 1) / unit overflows for an n within `unit` of 2^64 and returns a
 * tiny number - which turned a huge file into "almost no blocks" in st_blocks
 * (M-037), and is the same shape as M-006, M-019 and M-022. A size that comes
 * from a volume is somebody else's number; this form has no sum to overflow.
 * `unit` of zero returns zero rather than dividing by it. */
static inline uint64_t vibeos_ceil_div_u64(uint64_t n, uint64_t unit) {
    if (unit == 0u) {
        return 0u;
    }
    return n / unit + ((n % unit) != 0u ? 1u : 0u);
}

#endif
