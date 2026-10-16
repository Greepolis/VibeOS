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
