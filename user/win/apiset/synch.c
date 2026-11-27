/* SPDX-License-Identifier: GPL-3.0-or-later */
/* api-ms-win-core-synch-l1-2-0.dll (docs/shell/s7.md step 3): WaitOnAddress
 * and its two wakes, which Rust's standard library imports from this API set
 * rather than from kernel32 and uses for every lock and every park.
 *
 * Windows resolves an API set to kernelbase through a schema in the PEB; this
 * loader has no schema, so the set is a DLL of its own that does the work.
 *
 * The wait polls. WaitOnAddress may return before anybody wakes it - every
 * caller compares the value again and waits again, which is what the contract
 * says - so a wait that looks once a millisecond is correct, and the wakes have
 * nothing to do. It costs a millisecond of latency per contended wake. A real
 * wait, keyed on the address in the kernel, is WO-112. Built freestanding
 * against our ntdll alone. */

#include "vibeos_ntdll.h"

typedef int32_t BOOL32;

#define ERROR_INVALID_PARAMETER 87u
#define ERROR_TIMEOUT 1460u
#define INFINITE 0xFFFFFFFFu

static void set_error(uint32_t e) {
    __asm__ __volatile__("movl %0, %%gs:%c1" : : "r"(e), "i"(VIBEOS_TEB_LAST_ERROR) : "memory");
}

static int same(const volatile void *addr, const void *cmp, uint64_t size) {
    switch (size) {
    case 1: return *(const volatile uint8_t *)addr == *(const uint8_t *)cmp;
    case 2: return *(const volatile uint16_t *)addr == *(const uint16_t *)cmp;
    case 4: return *(const volatile uint32_t *)addr == *(const uint32_t *)cmp;
    default: return *(const volatile uint64_t *)addr == *(const uint64_t *)cmp;
    }
}

/* TRUE when the value differs - woken, as far as the caller can tell - and
 * FALSE with ERROR_TIMEOUT when it still matched after `ms`. */
BOOL32 WaitOnAddress(volatile void *addr, void *cmp, uint64_t size, uint32_t ms) {
    int64_t one_ms = -10000;   /* relative, in 100 ns */
    uint32_t waited = 0;

    if (!addr || !cmp || (size != 1u && size != 2u && size != 4u && size != 8u)) {
        set_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    while (same(addr, cmp, size)) {
        if (ms != INFINITE && waited >= ms) {
            set_error(ERROR_TIMEOUT);
            return 0;
        }
        (void)NtDelayExecution(0, &one_ms);
        waited++;
    }
    return 1;
}

void WakeByAddressSingle(void *addr) {
    (void)addr;
}

void WakeByAddressAll(void *addr) {
    (void)addr;
}

int vibeos_synch_main(uint64_t instance, uint32_t reason, void *reserved) {
    (void)instance;
    (void)reason;
    (void)reserved;
    return 1;
}
