/* SPDX-License-Identifier: GPL-3.0-or-later */
/* api-ms-win-core-synch-l1-2-0.dll (docs/shell/s7.md step 3): WaitOnAddress
 * and its two wakes, which Rust's standard library imports from this API set
 * rather than from kernel32 and uses for every lock and every park.
 *
 * Windows resolves an API set to kernelbase through a schema in the PEB; this
 * loader has no schema, so the set is a DLL of its own that does the work.
 *
 * Windows' design, which is ntdll's RtlWaitOnAddress: a waiter puts itself on
 * a list - its address and its thread id - looks at memory once more, and
 * sleeps on an alert to its thread id; a wake takes waiters for the address
 * off the list and alerts them. The kernel keeps an alert sent before the
 * sleep, so a wake between the last look and the sleep is not lost. The first
 * version polled once a millisecond, which was a tick per wake and made
 * FinalCommander's first listing take twenty seconds (WO-112). Built
 * freestanding against our ntdll alone. */

#include "vibeos_ntdll.h"

typedef int32_t BOOL32;

#define ERROR_INVALID_PARAMETER 87u
#define ERROR_TIMEOUT 1460u
#define INFINITE 0xFFFFFFFFu

typedef struct waiter {
    struct waiter *next;
    volatile void *addr;
    uint64_t tid;
    volatile int32_t woken;
} waiter_t;

static waiter_t *g_waiters;
static volatile int32_t g_lock;

static void set_error(uint32_t e) {
    __asm__ __volatile__("movl %0, %%gs:%c1" : : "r"(e), "i"(VIBEOS_TEB_LAST_ERROR) : "memory");
}

/* The list is held for a few instructions at a time; a thread that finds it
 * held gives its core away rather than spin against the holder. */
static void lock(void) {
    while (__atomic_exchange_n(&g_lock, 1, __ATOMIC_ACQUIRE) != 0) {
        (void)NtYieldExecution();
    }
}

static void unlock(void) {
    __atomic_store_n(&g_lock, 0, __ATOMIC_RELEASE);
}

static int same(const volatile void *addr, const void *cmp, uint64_t size) {
    switch (size) {
    case 1: return *(const volatile uint8_t *)addr == *(const uint8_t *)cmp;
    case 2: return *(const volatile uint16_t *)addr == *(const uint16_t *)cmp;
    case 4: return *(const volatile uint32_t *)addr == *(const uint32_t *)cmp;
    default: return *(const volatile uint64_t *)addr == *(const uint64_t *)cmp;
    }
}

static void unlink_waiter(waiter_t *w) {
    waiter_t **p;

    for (p = &g_waiters; *p; p = &(*p)->next) {
        if (*p == w) {
            *p = w->next;
            return;
        }
    }
}

static int64_t now_100ns(void) {
    int64_t c = 0, f = 10000000;

    (void)NtQueryPerformanceCounter(&c, &f);
    return f == 10000000 ? c : (int64_t)((uint64_t)c * 10000000u / (uint64_t)f);
}

/* TRUE when woken or when the value differs - a caller compares again in
 * either case, and may be woken for nothing, as on Windows - and FALSE with
 * ERROR_TIMEOUT when the time ran out first. */
BOOL32 WaitOnAddress(volatile void *addr, void *cmp, uint64_t size, uint32_t ms) {
    waiter_t me;
    int64_t end = 0;

    if (!addr || !cmp || (size != 1u && size != 2u && size != 4u && size != 8u)) {
        set_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    me.addr = addr;
    me.tid = vibeos_nt_rd64(vibeos_nt_teb() + VIBEOS_TEB_CLIENT_THREAD);
    me.woken = 0;
    if (ms != INFINITE) {
        end = now_100ns() + (int64_t)ms * 10000;
    }
    lock();
    if (!same(addr, cmp, size)) {
        unlock();
        return 1;
    }
    me.next = g_waiters;
    g_waiters = &me;
    unlock();
    for (;;) {
        int64_t left = 0;
        NTSTATUS_T st;

        if (ms != INFINITE) {
            left = end - now_100ns();
            if (left <= 0) {
                break;
            }
            left = -left;   /* relative */
        }
        st = NtWaitForAlertByThreadId((const void *)addr, ms != INFINITE ? &left : 0);
        if (__atomic_load_n(&me.woken, __ATOMIC_ACQUIRE)) {
            return 1;   /* the waker took us off the list */
        }
        if (st != VIBEOS_STATUS_ALERTED && st != VIBEOS_STATUS_TIMEOUT) {
            break;   /* a signal ends the wait: woken for nothing, which is allowed */
        }
        /* An alert left over from a wake we had already seen: wait again. */
    }
    lock();
    if (__atomic_load_n(&me.woken, __ATOMIC_ACQUIRE)) {
        unlock();
        return 1;   /* woken as the time ran out: the wake is ours */
    }
    unlink_waiter(&me);
    unlock();
    if (ms != INFINITE) {
        set_error(ERROR_TIMEOUT);
        return 0;
    }
    return 1;
}

/* Take waiters on `addr` off the list - one, or all - and alert them. Their
 * nodes are on their own stacks and stay valid until each sees `woken`, which
 * is set under the lock after the node is unlinked; the alert goes after the
 * lock, by thread id, so nothing of the node is touched once it is marked. */
static void wake(void *addr, int all) {
    uint64_t tids[64];
    uint32_t n = 0, i;
    waiter_t **p;

    lock();
    for (p = &g_waiters; *p && n < 64u;) {
        waiter_t *w = *p;

        if (w->addr == addr) {
            *p = w->next;
            tids[n++] = w->tid;
            __atomic_store_n(&w->woken, 1, __ATOMIC_RELEASE);
            if (!all) {
                break;
            }
        } else {
            p = &w->next;
        }
    }
    unlock();
    for (i = 0; i < n; i++) {
        (void)NtAlertThreadByThreadId(tids[i]);
    }
}

void WakeByAddressSingle(void *addr) {
    wake(addr, 0);
}

void WakeByAddressAll(void *addr) {
    wake(addr, 1);
}

int vibeos_synch_main(uint64_t instance, uint32_t reason, void *reserved) {
    (void)instance;
    (void)reason;
    (void)reserved;
    return 1;
}
