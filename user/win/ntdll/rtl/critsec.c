/* RTL_CRITICAL_SECTION (docs/abi/windows.md W2).
 *
 * The structure is Windows' (40 bytes on x64), because programs and Wine's tests
 * read its fields: LockCount is -1 while free and counts the owner's recursion
 * above that, RecursionCount is how many times the owner has entered, and
 * OwningThread is the TEB's ClientId.UniqueThread.
 *
 * There is one thread per process until W5, so a section is never contended and
 * no wait is implemented. A second thread finding it held calls
 * vibeos_rtl_contention(), which ends the process with STATUS_NOT_IMPLEMENTED:
 * spinning would hang the machine on a lock that nobody running can release, and
 * granting it would be a lie about mutual exclusion. W5 replaces that call with
 * a wait on LockSemaphore. */

#include "rtl_internal.h"

vibeos_ntstatus_t RtlInitializeCriticalSection(vibeos_rtl_critical_section_t *cs) {
    vibeos_rtl_critical_section_debug_t *dbg;

    dbg = (vibeos_rtl_critical_section_debug_t *)vibeos_rtl_alloc(sizeof(*dbg));
    if (!dbg) {
        return VIBEOS_RTL_ST(VIBEOS_STATUS_NO_MEMORY);
    }
    memset(dbg, 0, sizeof(*dbg));
    dbg->CriticalSection = cs;
    dbg->ProcessLocksList.Flink = &dbg->ProcessLocksList;
    dbg->ProcessLocksList.Blink = &dbg->ProcessLocksList;
    cs->DebugInfo = dbg;
    cs->LockCount = -1;
    cs->RecursionCount = 0;
    cs->OwningThread = 0;
    cs->LockSemaphore = 0;
    cs->SpinCount = 0;
    return VIBEOS_RTL_ST(VIBEOS_STATUS_SUCCESS);
}

vibeos_ntstatus_t RtlDeleteCriticalSection(vibeos_rtl_critical_section_t *cs) {
    if (cs->DebugInfo) {
        vibeos_rtl_free(cs->DebugInfo);
    }
    /* Windows leaves the structure unusable rather than zeroed; zeroed is
     * unusable too (LockCount 0 reads as held) and does not keep a pointer into
     * freed memory. */
    memset(cs, 0, sizeof(*cs));
    return VIBEOS_RTL_ST(VIBEOS_STATUS_SUCCESS);
}

vibeos_ntstatus_t RtlEnterCriticalSection(vibeos_rtl_critical_section_t *cs) {
    void *me = vibeos_rtl_current_thread();

    if (cs->LockCount == -1) {
        cs->LockCount = 0;
        cs->RecursionCount = 1;
        cs->OwningThread = me;
        return VIBEOS_RTL_ST(VIBEOS_STATUS_SUCCESS);
    }
    if (cs->OwningThread == me) {
        cs->LockCount++;
        cs->RecursionCount++;
        return VIBEOS_RTL_ST(VIBEOS_STATUS_SUCCESS);
    }
    vibeos_rtl_contention();
    return VIBEOS_RTL_ST(VIBEOS_STATUS_NOT_IMPLEMENTED);   /* only a host test sees this */
}

vibeos_ntstatus_t RtlLeaveCriticalSection(vibeos_rtl_critical_section_t *cs) {
    if (--cs->RecursionCount > 0) {
        cs->LockCount--;
        return VIBEOS_RTL_ST(VIBEOS_STATUS_SUCCESS);
    }
    cs->OwningThread = 0;
    cs->LockCount--;
    return VIBEOS_RTL_ST(VIBEOS_STATUS_SUCCESS);
}
