#ifndef VIBEOS_NTDLL_H
#define VIBEOS_NTDLL_H

/* What our ntdll exports, as our own programs call it (docs/abi/windows.md W1).
 *
 * Not windows.h: the programs of ours that run on the NT personality are built
 * freestanding with mingw-w64, against these declarations and the import
 * library our ntdll's link writes. Wine's tests and anybody else's programs use
 * their own headers; the names and argument orders are the same, which is the
 * whole contract a Windows DLL has. */

#include <stdint.h>

#include "vibeos/nt_layout.h"

typedef uint32_t NTSTATUS_T;

/* Inside ntdll the stubs are its own functions, called directly. */
#ifdef VIBEOS_BUILDING_NTDLL
#define NT_IMPORT
#else
#define NT_IMPORT __declspec(dllimport)
#endif

NT_IMPORT NTSTATUS_T NtTerminateProcess(uint64_t process, NTSTATUS_T status);
NT_IMPORT NTSTATUS_T NtWriteFile(uint64_t file, uint64_t event, void *apc, void *apc_ctx,
                                 vibeos_nt_iosb_t *iosb, const void *buf, uint32_t len,
                                 const int64_t *offset, const uint32_t *key);
NT_IMPORT NTSTATUS_T NtQueryInformationProcess(uint64_t process, uint32_t cls, void *buf,
                                               uint32_t len, uint32_t *ret_len);
NT_IMPORT NTSTATUS_T NtQuerySystemInformation(uint32_t cls, void *buf, uint32_t len,
                                              uint32_t *ret_len);
NT_IMPORT NTSTATUS_T NtQuerySystemTime(int64_t *time);
NT_IMPORT NTSTATUS_T NtQueryPerformanceCounter(int64_t *counter, int64_t *frequency);
/* A service no step has implemented, which the kernel must answer
 * STATUS_NOT_IMPLEMENTED: hello.exe asks it on purpose, as the Linux self-test
 * asks for syscall 1999, so the count is seen moving. */
NT_IMPORT NTSTATUS_T NtYieldExecution(void);
/* W2's; declared so a sabotage case can make hello.exe ask a service nobody
 * wrote and watch the gate name it (scripts/dev/cases/nt-hello-boot.txt). */
NT_IMPORT NTSTATUS_T NtClose(uint64_t handle);

/* The TEB, through %gs, as every Windows program reaches it. */
static inline uint64_t vibeos_nt_teb(void) {
    uint64_t t;
    __asm__ __volatile__("movq %%gs:0x30, %0" : "=r"(t));
    return t;
}

static inline uint64_t vibeos_nt_rd64(uint64_t at) {
    return *(volatile const uint64_t *)(uintptr_t)at;
}

#endif
