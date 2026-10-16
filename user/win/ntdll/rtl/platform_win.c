/* The Rtl core's five hooks, answered on Windows' side of the machine
 * (docs/abi/windows.md W2): the TEB and PEB through %gs, and the kernel through
 * NtWriteFile and NtTerminateProcess. tests/win/ntrtl_host.c answers the same
 * hooks on the Linux host; nothing else in rtl/ knows which it is running on. */

#include "rtl_internal.h"

uint8_t *vibeos_rtl_process_params(void) {
    uint64_t peb = 0;

    __asm__ __volatile__("movq %%gs:%c1, %0" : "=r"(peb) : "i"(VIBEOS_TEB_PEB));
    if (!peb) {
        return 0;
    }
    return (uint8_t *)(uintptr_t)vibeos_nt_rd64(peb + VIBEOS_PEB_PROCESS_PARAMS);
}

void *vibeos_rtl_current_thread(void) {
    uint64_t tid = 0;

    __asm__ __volatile__("movq %%gs:%c1, %0" : "=r"(tid) : "i"(VIBEOS_TEB_CLIENT_THREAD));
    return (void *)(uintptr_t)tid;
}

void vibeos_rtl_contention(void) {
    (void)NtTerminateProcess(VIBEOS_NT_CURRENT_PROCESS, VIBEOS_STATUS_NOT_IMPLEMENTED);
    for (;;) {
        /* NtTerminateProcess of ourselves does not return. */
    }
}

void vibeos_rtl_dbg_write(const char *buf, size_t len) {
    uint8_t *pp = vibeos_rtl_process_params();
    vibeos_nt_iosb_t iosb;
    uint64_t h;

    if (!pp || len == 0) {
        return;
    }
    h = rtl_rd64(pp, VIBEOS_RUPP_STD_ERROR);
    if (h == 0 || h == (uint64_t)-1) {
        return;                         /* no standard error to write to */
    }
    (void)NtWriteFile(h, 0, 0, 0, &iosb, buf, (uint32_t)len, 0, 0);
}
