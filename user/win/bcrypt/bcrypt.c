/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Random bytes for Windows programs (docs/shell/s7.md step 3): built twice, as
 * bcryptprimitives.dll (ProcessPrng, what Rust's standard library seeds its
 * hash maps from) and as bcrypt.dll (BCryptGenRandom with the system's
 * preferred generator). Each .def exports its own half.
 *
 * Both read the kernel's random pool - the one /dev/urandom reads - through
 * the private NtQuerySystemInformation class that stands in for Windows'
 * \Device\CNG (WO-110). Built freestanding against our ntdll alone. */

#include "vibeos_ntdll.h"

typedef int32_t BOOL32;

#define BCRYPT_USE_SYSTEM_PREFERRED_RNG 0x2u

static int fill(void *buf, uint64_t len) {
    uint8_t *p = (uint8_t *)buf;

    while (len > 0u) {
        uint32_t n = len > VIBEOS_NT_RANDOM_MAX ? VIBEOS_NT_RANDOM_MAX : (uint32_t)len;

        if ((NtQuerySystemInformation(VIBEOS_NT_SYSTEM_VIBEOS_RANDOM, p, n, 0) & 0xC0000000u) ==
            0xC0000000u) {
            return 0;
        }
        p += n;
        len -= n;
    }
    return 1;
}

/* Documented as always succeeding; a pool that cannot be read is a machine
 * that cannot be trusted with anything, so it ends the process. */
BOOL32 ProcessPrng(void *buf, uint64_t len) {
    if (!fill(buf, len)) {
        (void)NtTerminateProcess((uint64_t)-1, VIBEOS_STATUS_ACCESS_VIOLATION);
    }
    return 1;
}

/* Only the system's preferred generator: there are no algorithm handles to
 * open, so a handle without that flag is one this DLL never gave out. */
NTSTATUS_T BCryptGenRandom(void *alg, void *buf, uint32_t len, uint32_t flags) {
    if (alg || !(flags & BCRYPT_USE_SYSTEM_PREFERRED_RNG)) {
        return VIBEOS_STATUS_INVALID_HANDLE;
    }
    if (!buf && len) {
        return VIBEOS_STATUS_INVALID_PARAMETER;
    }
    return fill(buf, len) ? VIBEOS_STATUS_SUCCESS : VIBEOS_STATUS_ACCESS_VIOLATION;
}

int vibeos_bcrypt_main(uint64_t instance, uint32_t reason, void *reserved) {
    (void)instance;
    (void)reason;
    (void)reserved;
    return 1;
}
