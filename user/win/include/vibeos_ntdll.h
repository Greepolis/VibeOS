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

#include <stdarg.h>

#include "vibeos/nt_layout.h"
#include "vibeos_ntrtl.h"

typedef uint32_t NTSTATUS_T;

/* Inside ntdll the stubs are its own functions, called directly. */
#ifdef VIBEOS_BUILDING_NTDLL
#define NT_IMPORT
#else
#define NT_IMPORT __declspec(dllimport)
#endif

/* The Rtl functions are written in C, so unlike the stubs they are exported by
 * name here and not by a .drectve; x64 decorates nothing, so the export is the
 * Windows name. The same sources are compiled on the Linux host for the Rtl
 * tests, where there are no DLLs and the macro is empty. */
#ifndef _WIN32
#define RTL_API
#elif defined(VIBEOS_BUILDING_NTDLL)
#define RTL_API __declspec(dllexport)
#else
#define RTL_API __declspec(dllimport)
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
/* What ntdll's loader brings a DLL in with (W2). */
NT_IMPORT NTSTATUS_T NtOpenFile(uint64_t *file, uint32_t access, void *attributes,
                                vibeos_nt_iosb_t *iosb, uint32_t share, uint32_t options);
NT_IMPORT NTSTATUS_T NtCreateSection(uint64_t *section, uint32_t access, void *attributes,
                                     int64_t *max_size, uint32_t protection,
                                     uint32_t allocation, uint64_t file);
NT_IMPORT NTSTATUS_T NtMapViewOfSection(uint64_t section, uint64_t process, uint64_t *base,
                                        uint64_t zero_bits, uint64_t commit, int64_t *offset,
                                        uint64_t *size, uint32_t inherit, uint32_t allocation,
                                        uint32_t protection);

/* Virtual memory (W3). */
NT_IMPORT NTSTATUS_T NtAllocateVirtualMemory(uint64_t process, uint64_t *base, uint64_t zero_bits,
                                             uint64_t *size, uint32_t type, uint32_t protect);
NT_IMPORT NTSTATUS_T NtFreeVirtualMemory(uint64_t process, uint64_t *base, uint64_t *size,
                                         uint32_t type);
NT_IMPORT NTSTATUS_T NtProtectVirtualMemory(uint64_t process, uint64_t *base, uint64_t *size,
                                            uint32_t protect, uint32_t *old);
NT_IMPORT NTSTATUS_T NtQueryVirtualMemory(uint64_t process, uint64_t address, uint32_t cls,
                                          void *buf, uint64_t len, uint64_t *ret_len);

/* What kernel32 is written on (W2): the synchronisation objects, the waits and
 * Sleep. */
NT_IMPORT NTSTATUS_T NtCreateEvent(uint64_t *event, uint32_t access, void *attributes,
                                   uint32_t type, uint8_t initial);
NT_IMPORT NTSTATUS_T NtOpenEvent(uint64_t *event, uint32_t access, void *attributes);
NT_IMPORT NTSTATUS_T NtSetEvent(uint64_t event, int32_t *previous);
NT_IMPORT NTSTATUS_T NtResetEvent(uint64_t event, int32_t *previous);
NT_IMPORT NTSTATUS_T NtPulseEvent(uint64_t event, int32_t *previous);
NT_IMPORT NTSTATUS_T NtCreateMutant(uint64_t *mutant, uint32_t access, void *attributes,
                                    uint8_t initial_owner);
NT_IMPORT NTSTATUS_T NtOpenMutant(uint64_t *mutant, uint32_t access, void *attributes);
NT_IMPORT NTSTATUS_T NtReleaseMutant(uint64_t mutant, int32_t *previous);
NT_IMPORT NTSTATUS_T NtCreateSemaphore(uint64_t *semaphore, uint32_t access, void *attributes,
                                       int32_t initial, int32_t maximum);
NT_IMPORT NTSTATUS_T NtOpenSemaphore(uint64_t *semaphore, uint32_t access, void *attributes);
NT_IMPORT NTSTATUS_T NtReleaseSemaphore(uint64_t semaphore, int32_t count, int32_t *previous);
NT_IMPORT NTSTATUS_T NtWaitForSingleObject(uint64_t handle, uint8_t alertable,
                                           const int64_t *timeout);
NT_IMPORT NTSTATUS_T NtWaitForMultipleObjects(uint32_t count, const uint64_t *handles,
                                              uint32_t wait_type, uint8_t alertable,
                                              const int64_t *timeout);
/* Written over before the kernel has them (W2): until W4 and W5 they answer
 * STATUS_NOT_IMPLEMENTED, counted, and a program is told the call failed. */
NT_IMPORT NTSTATUS_T NtCreateFile(uint64_t *file, uint32_t access, void *attributes,
                                  vibeos_nt_iosb_t *iosb, int64_t *allocation, uint32_t file_attributes,
                                  uint32_t share, uint32_t disposition, uint32_t options, void *ea,
                                  uint32_t ea_length);
NT_IMPORT NTSTATUS_T NtCreateThreadEx(uint64_t *thread, uint32_t access, void *attributes,
                                      uint64_t process, void *start, void *param, uint32_t flags,
                                      uint64_t zero_bits, uint64_t stack_size, uint64_t max_stack,
                                      void *attribute_list);
NT_IMPORT NTSTATUS_T NtDelayExecution(uint8_t alertable, const int64_t *interval);
NT_IMPORT NTSTATUS_T NtDuplicateObject(uint64_t source_process, uint64_t source,
                                       uint64_t target_process, uint64_t *target,
                                       uint32_t access, uint32_t attributes, uint32_t options);

/* ---- the Rtl runtime (user/win/ntdll/rtl/, docs/abi/windows.md W2) -------------------
 * Windows' prototypes, with the structures under our own names (vibeos_ntrtl.h). */
RTL_API void RtlInitString(vibeos_astring_t *dest, const char *src);
RTL_API void RtlInitUnicodeString(vibeos_ustring_t *dest, const vibeos_wchar_t *src);
RTL_API vibeos_boolean_t RtlCreateUnicodeString(vibeos_ustring_t *dest, const vibeos_wchar_t *src);
RTL_API void RtlFreeUnicodeString(vibeos_ustring_t *str);
RTL_API vibeos_ntstatus_t RtlDuplicateUnicodeString(uint32_t flags, const vibeos_ustring_t *src,
                                                    vibeos_ustring_t *dest);
RTL_API int32_t RtlCompareString(const vibeos_astring_t *a, const vibeos_astring_t *b,
                                 vibeos_boolean_t case_insensitive);
RTL_API int32_t RtlCompareUnicodeString(const vibeos_ustring_t *a, const vibeos_ustring_t *b,
                                        vibeos_boolean_t case_insensitive);
RTL_API void RtlUpperString(vibeos_astring_t *dest, const vibeos_astring_t *src);
RTL_API vibeos_wchar_t RtlUpcaseUnicodeChar(vibeos_wchar_t c);
RTL_API vibeos_wchar_t RtlAnsiCharToUnicodeChar(unsigned char **src);
RTL_API vibeos_ntstatus_t RtlMultiByteToUnicodeN(vibeos_wchar_t *dest, uint32_t dest_size,
                                                 uint32_t *result_size, const char *src,
                                                 uint32_t src_size);
RTL_API size_t RtlCompareMemoryUlong(const void *src, size_t length, uint32_t pattern);
RTL_API uint32_t RtlComputeCrc32(uint32_t initial, const uint8_t *buf, int32_t length);
RTL_API uint32_t RtlRandom(uint32_t *seed);
RTL_API uint32_t RtlNtStatusToDosError(vibeos_ntstatus_t status);
/* The loader's answers to GetModuleHandle and GetProcAddress (ntdll/start.c). */
RTL_API NTSTATUS_T LdrGetDllHandle(const uint16_t *path, const uint32_t *flags,
                                   const vibeos_ustring_t *name, void **handle);
RTL_API NTSTATUS_T LdrLoadDll(const uint16_t *path, const uint32_t *flags,
                              const vibeos_ustring_t *name, void **handle);
RTL_API NTSTATUS_T LdrGetProcedureAddress(void *module, const vibeos_astring_t *name,
                                          uint32_t ordinal, void **address);
RTL_API void *RtlAllocateHeap(void *heap, uint32_t flags, size_t size);
RTL_API vibeos_boolean_t RtlFreeHeap(void *heap, uint32_t flags, void *p);
RTL_API void *RtlReAllocateHeap(void *heap, uint32_t flags, void *p, size_t size);
RTL_API size_t RtlSizeHeap(void *heap, uint32_t flags, const void *p);
RTL_API uint32_t RtlUniform(uint32_t *seed);
RTL_API char *RtlIpv4AddressToStringA(const vibeos_in_addr_t *addr, char *str);
RTL_API vibeos_ntstatus_t RtlIpv4StringToAddressA(const char *str, vibeos_boolean_t strict,
                                                  const char **terminator, vibeos_in_addr_t *addr);
RTL_API char *RtlIpv6AddressToStringA(const vibeos_in6_addr_t *addr, char *str);
RTL_API vibeos_ntstatus_t RtlIpv6StringToAddressA(const char *str, const char **terminator,
                                                  vibeos_in6_addr_t *addr);
RTL_API vibeos_ntstatus_t RtlIpv6StringToAddressW(const vibeos_wchar_t *str,
                                                  const vibeos_wchar_t **terminator,
                                                  vibeos_in6_addr_t *addr);
RTL_API void RtlInitializeHandleTable(uint32_t max_handle_count, uint32_t handle_size,
                                      vibeos_rtl_handle_table_t *table);
RTL_API vibeos_rtl_handle_t *RtlAllocateHandle(vibeos_rtl_handle_table_t *table, uint32_t *index);
RTL_API vibeos_boolean_t RtlFreeHandle(vibeos_rtl_handle_table_t *table, vibeos_rtl_handle_t *handle);
RTL_API vibeos_boolean_t RtlIsValidIndexHandle(const vibeos_rtl_handle_table_t *table, uint32_t index,
                                               vibeos_rtl_handle_t **handle);
RTL_API vibeos_ntstatus_t RtlDestroyHandleTable(vibeos_rtl_handle_table_t *table);
RTL_API vibeos_image_nt_headers64_t *RtlImageNtHeader(void *base);
RTL_API void *RtlImageDirectoryEntryToData(void *base, vibeos_boolean_t mapped_as_image,
                                           uint16_t directory_entry, uint32_t *size);
RTL_API vibeos_ntstatus_t RtlCreateEnvironment(vibeos_boolean_t inherit, vibeos_wchar_t **environment);
RTL_API vibeos_ntstatus_t RtlDestroyEnvironment(void *environment);
RTL_API void RtlSetCurrentEnvironment(void *environment, void **previous);
RTL_API vibeos_ntstatus_t RtlSetEnvironmentVariable(vibeos_wchar_t **environment,
                                                    const vibeos_ustring_t *name,
                                                    const vibeos_ustring_t *value);
RTL_API vibeos_ntstatus_t RtlQueryEnvironmentVariable_U(vibeos_wchar_t *environment,
                                                        const vibeos_ustring_t *name,
                                                        vibeos_ustring_t *value);
RTL_API vibeos_ntstatus_t RtlInitializeCriticalSection(vibeos_rtl_critical_section_t *cs);
RTL_API vibeos_ntstatus_t RtlDeleteCriticalSection(vibeos_rtl_critical_section_t *cs);
RTL_API vibeos_ntstatus_t RtlEnterCriticalSection(vibeos_rtl_critical_section_t *cs);
RTL_API vibeos_ntstatus_t RtlLeaveCriticalSection(vibeos_rtl_critical_section_t *cs);
RTL_API vibeos_boolean_t RtlDosPathNameToNtPathName_U(const vibeos_wchar_t *dos_name,
                                                      vibeos_ustring_t *nt_name,
                                                      vibeos_wchar_t **file_part,
                                                      vibeos_rtl_relative_name_u_t *relative);
RTL_API uint32_t DbgPrint(const char *format, ...);
RTL_API uint32_t DbgPrintEx(uint32_t component, uint32_t level, const char *format, ...);
RTL_API uint32_t vDbgPrintEx(uint32_t component, uint32_t level, const char *format, va_list args);
RTL_API uint32_t vDbgPrintExWithPrefix(const char *prefix, uint32_t component, uint32_t level,
                                       const char *format, va_list args);

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
