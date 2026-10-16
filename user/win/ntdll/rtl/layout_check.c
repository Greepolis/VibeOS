/* vibeos_ntrtl.h against mingw-w64's own headers, at compile time
 * (docs/abi/windows.md W2). Compiled with ntdll and contributing nothing to it:
 * every structure the Rtl runtime declares under its own name, that mingw-w64
 * also declares, is compared field by field with mingw-w64's, and so are the
 * status codes. A structure mingw-w64 does not declare (RTL_HANDLE_TABLE,
 * RTL_RELATIVE_NAME_U) is held to the size and offsets the Windows SDK and
 * Microsoft's documentation give, written here as numbers because there is no
 * artefact to compare with.
 *
 * Wine's tests run against this code on VibeOS, and a layout that disagrees
 * with the compiler Wine's tests are built by is a failure they would find one
 * test at a time. */

#include <winsock2.h>
#include <ws2tcpip.h>
#define WIN32_NO_STATUS
#include <windows.h>
#include <winternl.h>
#undef WIN32_NO_STATUS
#include <ntstatus.h>

#include "vibeos/nt_layout.h"
#include "vibeos_ntrtl.h"

#define SAME(ours, theirs) _Static_assert((ours) == (theirs), #ours " is not what mingw-w64 has")
#define FIELD(vt, vf, wt, wf) \
    SAME(offsetof(vt, vf), offsetof(wt, wf)); \
    _Static_assert(sizeof(((vt *)0)->vf) == sizeof(((wt *)0)->wf), #vf " is not the size mingw-w64 has")

SAME(sizeof(vibeos_wchar_t), sizeof(WCHAR));
SAME(sizeof(vibeos_ntstatus_t), sizeof(NTSTATUS));
SAME(sizeof(vibeos_boolean_t), sizeof(BOOLEAN));

SAME(sizeof(vibeos_ustring_t), sizeof(UNICODE_STRING));
FIELD(vibeos_ustring_t, Length, UNICODE_STRING, Length);
FIELD(vibeos_ustring_t, MaximumLength, UNICODE_STRING, MaximumLength);
FIELD(vibeos_ustring_t, Buffer, UNICODE_STRING, Buffer);

SAME(sizeof(vibeos_astring_t), sizeof(STRING));
FIELD(vibeos_astring_t, Length, STRING, Length);
FIELD(vibeos_astring_t, MaximumLength, STRING, MaximumLength);
FIELD(vibeos_astring_t, Buffer, STRING, Buffer);

SAME(sizeof(vibeos_in_addr_t), sizeof(IN_ADDR));
SAME(sizeof(vibeos_in6_addr_t), sizeof(IN6_ADDR));

SAME(sizeof(vibeos_rtl_critical_section_t), sizeof(RTL_CRITICAL_SECTION));
SAME(sizeof(vibeos_rtl_critical_section_t), 40);
FIELD(vibeos_rtl_critical_section_t, DebugInfo, RTL_CRITICAL_SECTION, DebugInfo);
FIELD(vibeos_rtl_critical_section_t, LockCount, RTL_CRITICAL_SECTION, LockCount);
FIELD(vibeos_rtl_critical_section_t, RecursionCount, RTL_CRITICAL_SECTION, RecursionCount);
FIELD(vibeos_rtl_critical_section_t, OwningThread, RTL_CRITICAL_SECTION, OwningThread);
FIELD(vibeos_rtl_critical_section_t, LockSemaphore, RTL_CRITICAL_SECTION, LockSemaphore);
FIELD(vibeos_rtl_critical_section_t, SpinCount, RTL_CRITICAL_SECTION, SpinCount);

SAME(sizeof(vibeos_rtl_critical_section_debug_t), sizeof(RTL_CRITICAL_SECTION_DEBUG));
FIELD(vibeos_rtl_critical_section_debug_t, Type, RTL_CRITICAL_SECTION_DEBUG, Type);
FIELD(vibeos_rtl_critical_section_debug_t, CreatorBackTraceIndex, RTL_CRITICAL_SECTION_DEBUG,
      CreatorBackTraceIndex);
FIELD(vibeos_rtl_critical_section_debug_t, CriticalSection, RTL_CRITICAL_SECTION_DEBUG, CriticalSection);
FIELD(vibeos_rtl_critical_section_debug_t, ProcessLocksList, RTL_CRITICAL_SECTION_DEBUG, ProcessLocksList);
FIELD(vibeos_rtl_critical_section_debug_t, EntryCount, RTL_CRITICAL_SECTION_DEBUG, EntryCount);
FIELD(vibeos_rtl_critical_section_debug_t, ContentionCount, RTL_CRITICAL_SECTION_DEBUG, ContentionCount);
FIELD(vibeos_rtl_critical_section_debug_t, Flags, RTL_CRITICAL_SECTION_DEBUG, Flags);
SAME(sizeof(vibeos_rtl_list_entry_t), sizeof(LIST_ENTRY));

SAME(sizeof(vibeos_image_data_directory_t), sizeof(IMAGE_DATA_DIRECTORY));
SAME(sizeof(vibeos_image_file_header_t), sizeof(IMAGE_FILE_HEADER));
SAME(sizeof(vibeos_image_optional_header64_t), sizeof(IMAGE_OPTIONAL_HEADER64));
SAME(sizeof(vibeos_image_nt_headers64_t), sizeof(IMAGE_NT_HEADERS64));
SAME(sizeof(vibeos_image_section_header_t), sizeof(IMAGE_SECTION_HEADER));
FIELD(vibeos_image_nt_headers64_t, Signature, IMAGE_NT_HEADERS64, Signature);
FIELD(vibeos_image_nt_headers64_t, FileHeader, IMAGE_NT_HEADERS64, FileHeader);
FIELD(vibeos_image_nt_headers64_t, OptionalHeader, IMAGE_NT_HEADERS64, OptionalHeader);
FIELD(vibeos_image_file_header_t, NumberOfSections, IMAGE_FILE_HEADER, NumberOfSections);
FIELD(vibeos_image_file_header_t, SizeOfOptionalHeader, IMAGE_FILE_HEADER, SizeOfOptionalHeader);
FIELD(vibeos_image_optional_header64_t, Magic, IMAGE_OPTIONAL_HEADER64, Magic);
FIELD(vibeos_image_optional_header64_t, AddressOfEntryPoint, IMAGE_OPTIONAL_HEADER64, AddressOfEntryPoint);
FIELD(vibeos_image_optional_header64_t, ImageBase, IMAGE_OPTIONAL_HEADER64, ImageBase);
FIELD(vibeos_image_optional_header64_t, SizeOfHeaders, IMAGE_OPTIONAL_HEADER64, SizeOfHeaders);
FIELD(vibeos_image_optional_header64_t, SizeOfStackReserve, IMAGE_OPTIONAL_HEADER64, SizeOfStackReserve);
FIELD(vibeos_image_optional_header64_t, NumberOfRvaAndSizes, IMAGE_OPTIONAL_HEADER64, NumberOfRvaAndSizes);
FIELD(vibeos_image_optional_header64_t, DataDirectory, IMAGE_OPTIONAL_HEADER64, DataDirectory);
FIELD(vibeos_image_section_header_t, VirtualSize, IMAGE_SECTION_HEADER, Misc);
FIELD(vibeos_image_section_header_t, VirtualAddress, IMAGE_SECTION_HEADER, VirtualAddress);
FIELD(vibeos_image_section_header_t, SizeOfRawData, IMAGE_SECTION_HEADER, SizeOfRawData);
FIELD(vibeos_image_section_header_t, PointerToRawData, IMAGE_SECTION_HEADER, PointerToRawData);
FIELD(vibeos_image_section_header_t, Characteristics, IMAGE_SECTION_HEADER, Characteristics);
SAME(VIBEOS_IMAGE_DOS_SIGNATURE, IMAGE_DOS_SIGNATURE);
SAME(VIBEOS_IMAGE_NT_SIGNATURE, IMAGE_NT_SIGNATURE);
SAME(VIBEOS_IMAGE_NT_OPTIONAL_HDR32_MAGIC, IMAGE_NT_OPTIONAL_HDR32_MAGIC);
SAME(VIBEOS_IMAGE_NT_OPTIONAL_HDR64_MAGIC, IMAGE_NT_OPTIONAL_HDR64_MAGIC);

/* RTL_HANDLE_TABLE (Microsoft's ntrtl.h, no mingw-w64 declaration): four ULONGs
 * ahead of four pointers. */
SAME(sizeof(vibeos_rtl_handle_table_t), 0x30);
SAME(offsetof(vibeos_rtl_handle_table_t, MaxHandleCount), 0x00);
SAME(offsetof(vibeos_rtl_handle_table_t, HandleSize), 0x04);
SAME(offsetof(vibeos_rtl_handle_table_t, FreeHandles), 0x10);
SAME(offsetof(vibeos_rtl_handle_table_t, CommittedHandles), 0x18);
SAME(offsetof(vibeos_rtl_handle_table_t, UnCommittedHandles), 0x20);
SAME(offsetof(vibeos_rtl_handle_table_t, MaxReservedHandles), 0x28);
/* RTL_RELATIVE_NAME_U: a UNICODE_STRING and two pointers. */
SAME(sizeof(vibeos_rtl_relative_name_u_t), 0x20);

SAME(VIBEOS_STATUS_SUCCESS, (unsigned)STATUS_SUCCESS);
SAME(VIBEOS_STATUS_BUFFER_OVERFLOW, (unsigned)STATUS_BUFFER_OVERFLOW);
SAME(VIBEOS_STATUS_BUFFER_TOO_SMALL, (unsigned)STATUS_BUFFER_TOO_SMALL);
SAME(VIBEOS_STATUS_VARIABLE_NOT_FOUND, (unsigned)STATUS_VARIABLE_NOT_FOUND);
SAME(VIBEOS_STATUS_NAME_TOO_LONG, (unsigned)STATUS_NAME_TOO_LONG);
SAME(VIBEOS_STATUS_INVALID_PARAMETER, (unsigned)STATUS_INVALID_PARAMETER);
SAME(VIBEOS_STATUS_NO_MEMORY, (unsigned)STATUS_NO_MEMORY);
SAME(VIBEOS_STATUS_NOT_IMPLEMENTED, (unsigned)STATUS_NOT_IMPLEMENTED);
