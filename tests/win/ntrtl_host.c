/* The Rtl core's hooks, answered on the Linux host (docs/abi/windows.md W2).
 *
 * user/win/ntdll/rtl/platform_win.c answers the same five from the TEB, the PEB and
 * the kernel. Here the answers are variables the test sets: the process
 * parameters are a byte block laid out by include/vibeos/nt_layout.h's offsets
 * (so a wrong offset in the core is a wrong read in a test, not a coincidence),
 * the calling thread is a number, contention is counted instead of ending the
 * process, and what DbgPrint writes is kept. */

#include <stdint.h>
#include <string.h>

#include "rtl_internal.h"

uint8_t  g_host_params[VIBEOS_RUPP_SIZE];
int      g_host_params_present = 1;
void    *g_host_thread = (void *)(uintptr_t)0x1234;
int      g_host_contention;
char     g_host_dbg[4096];
size_t   g_host_dbg_len;
int      g_host_dbg_writes;

uint8_t *vibeos_rtl_process_params(void) {
    return g_host_params_present ? g_host_params : 0;
}

void *vibeos_rtl_current_thread(void) {
    return g_host_thread;
}

void vibeos_rtl_contention(void) {
    g_host_contention++;
}

void vibeos_rtl_dbg_write(const char *buf, size_t len) {
    g_host_dbg_writes++;
    if (len > sizeof(g_host_dbg) - g_host_dbg_len - 1u) {
        len = sizeof(g_host_dbg) - g_host_dbg_len - 1u;
    }
    memcpy(g_host_dbg + g_host_dbg_len, buf, len);
    g_host_dbg_len += len;
    g_host_dbg[g_host_dbg_len] = 0;
}

/* The heap's memory: a pool handed out upwards from its middle, with what a
 * test can change about it - a gap left before each segment, so that two
 * segments are not adjacent and an address between them is nobody's; `below`,
 * so that the next segment comes from under every other one, as nothing
 * promises it will not; and a budget, so that the machine can run out. */
#define HOST_POOL (64u << 20)
static uint8_t g_host_pool[HOST_POOL] __attribute__((aligned(16)));
static size_t g_host_pool_hi = HOST_POOL / 2u, g_host_pool_lo = HOST_POOL / 2u;
size_t g_host_pages_gap;
int g_host_pages_below;
size_t g_host_pages_budget = (size_t)-1;
int g_host_pages_calls;
uint8_t *g_host_pages_last;

void *vibeos_rtl_pages(size_t bytes) {
    size_t at;

    g_host_pages_calls++;
    if (bytes > g_host_pages_budget) {
        return 0;
    }
    if (g_host_pages_below) {
        if (bytes + g_host_pages_gap > g_host_pool_lo) {
            return 0;
        }
        at = g_host_pool_lo - bytes;
        g_host_pool_lo = at - g_host_pages_gap;
    } else {
        at = g_host_pool_hi + g_host_pages_gap;
        if (at > HOST_POOL || bytes > HOST_POOL - at) {
            return 0;
        }
        g_host_pool_hi = at + bytes;
        /* What was there before is the heap's to overwrite, as on Windows. A
         * gap is filled with something that looks like an allocated block's
         * header, so that only knowing which memory is a segment tells it
         * apart. */
        memset(g_host_pool + at - g_host_pages_gap, 0, g_host_pages_gap);
        if (g_host_pages_gap >= 32u) {
            uint64_t fake[2] = { 64u, 0x41425255524C4F00ull };
            memcpy(g_host_pool + at - 32u, fake, sizeof(fake));
        }
    }
    g_host_pages_budget -= g_host_pages_budget == (size_t)-1 ? 0 : bytes;
    g_host_pages_last = g_host_pool + at;
    return g_host_pages_last;
}
