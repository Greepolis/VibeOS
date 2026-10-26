/* NT personality: the process services of W1 (docs/abi/windows.md).
 *
 * NtTerminateProcess and NtQueryInformationProcess, for the calling process
 * only: a handle to another process is W2's object manager and W6's process
 * creation, and until they exist no program can hold one. */

#include "nt_internal.h"

/* A process handle W1 can answer for: the pseudo-handle for oneself. Windows
 * also lets NtTerminateProcess take 0, meaning "every thread but this one";
 * with one thread per process in W1 there is nothing for it to end, and it is
 * refused rather than half-honoured. */
static int nt_is_self(uint64_t h) {
    return h == VIBEOS_NT_CURRENT_PROCESS;
}

/* NtTerminateProcess(ProcessHandle, ExitStatus). The whole process ends and the
 * parent's wait sees the status - its low byte, as the wait status of the
 * kernel underneath carries it; an NT parent reads the full 32 bits from W6. */
static long nt_terminate_process(uint64_t process, uint64_t status) {
    if (!nt_is_self(process)) {
        return NT_STATUS(VIBEOS_STATUS_INVALID_HANDLE);
    }
    ks_task_exit_group((uint64_t)(uint32_t)status);
    return NT_STATUS(VIBEOS_STATUS_SUCCESS);   /* not reached */
}

/* NtQueryInformationProcess(Process, Class, Buffer, Length, ReturnLength).
 * ProcessBasicInformation only, in W1; any other class is
 * STATUS_INVALID_INFO_CLASS, which a program asking about a class it does not
 * need takes in its stride. A length that is not the structure's is
 * STATUS_INFO_LENGTH_MISMATCH, with nothing written - Windows' rule for this
 * class. */
static long nt_query_information_process(uint64_t process, uint64_t cls, uint64_t buf,
                                         uint64_t len, uint64_t ret_len) {
    vibeos_nt_process_basic_t pbi;
    uint32_t got = (uint32_t)sizeof(pbi);
    int me = ks_current();
    const vibeos_task_t *t;
    uint32_t i;

    if (!nt_is_self(process) || me < 0) {
        return NT_STATUS(VIBEOS_STATUS_INVALID_HANDLE);
    }
    if ((uint32_t)cls != VIBEOS_NT_PROCESS_BASIC_INFORMATION) {
        return NT_STATUS(VIBEOS_STATUS_INVALID_INFO_CLASS);
    }
    if ((uint32_t)len != sizeof(pbi)) {
        return NT_STATUS(VIBEOS_STATUS_INFO_LENGTH_MISMATCH);
    }
    for (i = 0; i < sizeof(pbi); i++) {
        ((uint8_t *)&pbi)[i] = 0;
    }
    t = ks_id(me);
    pbi.exit_status = 0x103u;   /* STATUS_PENDING: the process is running */
    pbi.peb = ks_image(me)->start_arg;
    pbi.affinity_mask = ks_cpus_online_mask();
    pbi.base_priority = 8u;     /* NORMAL_PRIORITY_CLASS's base */
    pbi.unique_process_id = t->tgid;
    pbi.inherited_from_pid = t->ppid;
    /* The buffer was judged by the row; the copy is fault-safe for the sibling
     * that unmaps it in between. */
    if (nt_copy_out(buf, &pbi, sizeof(pbi)) != 0) {
        return NT_STATUS(VIBEOS_STATUS_ACCESS_VIOLATION);
    }
    if (ret_len != 0u &&
        nt_copy_out(ret_len, &got, sizeof(got)) != 0) {
        return NT_STATUS(VIBEOS_STATUS_ACCESS_VIOLATION);
    }
    return NT_STATUS(VIBEOS_STATUS_SUCCESS);
}

#define NT_PROCESS_SERVICES(X) \
    X(VIBEOS_NT_NR_NtTerminateProcess, NtTerminateProcess, EXIT_GROUP, NOPTR, \
      nt_terminate_process(ARG(0), NT_ARG32(1))) \
    X(VIBEOS_NT_NR_NtQueryInformationProcess, NtQueryInformationProcess, IDENTITY_GET, \
      PTRS(NT_OUT_BUF(2, 3), OUT_OPT(4, 4)), \
      nt_query_information_process(ARG(0), NT_ARG32(1), ARG(2), NT_ARG32(3), ARG(4)))

NT_DEFINE_SERVICES(process, NT_PROCESS_SERVICES)
