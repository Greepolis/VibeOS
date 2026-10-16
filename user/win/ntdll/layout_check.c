/* include/vibeos/nt_layout.h against mingw-w64's own headers, at compile time
 * (docs/abi/windows.md W1). Compiled with ntdll and contributing nothing to it:
 * if an offset the kernel writes and our ntdll reads disagrees with where
 * mingw-w64's winternl.h and winnt.h put the field, the ntdll build stops here.
 *
 * mingw-w64's headers are the artefact neither the kernel nor our ntdll wrote -
 * the layout test against the host's uapi headers is the Linux side's (A5).
 * They declare most of these blocks as Reserved arrays, so the comparison is
 * the offset of what they do name; the fields they leave unnamed are checked
 * where Windows programs reach them, by the tests that run on W2 onwards. */

#include <stddef.h>
#include <windows.h>
#include <winternl.h>

#include "vibeos/nt_layout.h"

#define SAME(ours, theirs) _Static_assert((ours) == (theirs), #ours " is not where mingw-w64 has it")

SAME(VIBEOS_TEB_EXCEPTION_LIST, offsetof(NT_TIB, ExceptionList));
SAME(VIBEOS_TEB_STACK_BASE, offsetof(NT_TIB, StackBase));
SAME(VIBEOS_TEB_STACK_LIMIT, offsetof(NT_TIB, StackLimit));
SAME(VIBEOS_TEB_SELF, offsetof(NT_TIB, Self));
SAME(VIBEOS_TEB_PEB, offsetof(TEB, ProcessEnvironmentBlock));
SAME(VIBEOS_TEB_TLS_SLOTS, offsetof(TEB, TlsSlots));
SAME(VIBEOS_TEB_TLS_SLOT_COUNT, sizeof(((TEB *)0)->TlsSlots) / sizeof(void *));

SAME(VIBEOS_PEB_BEING_DEBUGGED, offsetof(PEB, BeingDebugged));
SAME(VIBEOS_PEB_IMAGE_BASE, offsetof(PEB, Reserved3) + sizeof(void *));
SAME(VIBEOS_PEB_LDR, offsetof(PEB, Ldr));
SAME(VIBEOS_PEB_PROCESS_PARAMS, offsetof(PEB, ProcessParameters));
SAME(VIBEOS_PEB_SESSION_ID, offsetof(PEB, SessionId));

SAME(VIBEOS_RUPP_IMAGE_PATH, offsetof(RTL_USER_PROCESS_PARAMETERS, ImagePathName));
SAME(VIBEOS_RUPP_COMMAND_LINE, offsetof(RTL_USER_PROCESS_PARAMETERS, CommandLine));

SAME(VIBEOS_USTR_LENGTH, offsetof(UNICODE_STRING, Length));
SAME(VIBEOS_USTR_MAXIMUM, offsetof(UNICODE_STRING, MaximumLength));
SAME(VIBEOS_USTR_BUFFER, offsetof(UNICODE_STRING, Buffer));
SAME(VIBEOS_USTR_SIZE, sizeof(UNICODE_STRING));

SAME(sizeof(vibeos_nt_iosb_t), sizeof(IO_STATUS_BLOCK));
SAME(offsetof(vibeos_nt_iosb_t, information), offsetof(IO_STATUS_BLOCK, Information));

SAME(sizeof(vibeos_nt_process_basic_t), sizeof(PROCESS_BASIC_INFORMATION));
SAME(offsetof(vibeos_nt_process_basic_t, peb), offsetof(PROCESS_BASIC_INFORMATION, PebBaseAddress));
SAME(offsetof(vibeos_nt_process_basic_t, unique_process_id),
     offsetof(PROCESS_BASIC_INFORMATION, UniqueProcessId));

SAME(sizeof(vibeos_nt_system_basic_t), sizeof(SYSTEM_BASIC_INFORMATION));

SAME(VIBEOS_STATUS_ACCESS_VIOLATION, (unsigned)STATUS_ACCESS_VIOLATION);
SAME(VIBEOS_STATUS_INVALID_HANDLE, (unsigned)STATUS_INVALID_HANDLE);
SAME(VIBEOS_STATUS_DLL_NOT_FOUND, (unsigned)STATUS_DLL_NOT_FOUND);
SAME(VIBEOS_STATUS_ENTRYPOINT_NOT_FOUND, (unsigned)STATUS_ENTRYPOINT_NOT_FOUND);
SAME(VIBEOS_STATUS_INVALID_PARAMETER, (unsigned)STATUS_INVALID_PARAMETER);
SAME(VIBEOS_STATUS_NO_MEMORY, (unsigned)STATUS_NO_MEMORY);
