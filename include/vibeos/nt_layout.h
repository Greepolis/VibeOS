#ifndef VIBEOS_NT_LAYOUT_H
#define VIBEOS_NT_LAYOUT_H

/* The NT structures the kernel writes and our own ntdll reads, at Windows'
 * offsets (docs/abi/windows.md W1) - the counterpart of linux_layout.h.
 *
 * Offsets, not structures, for the blocks a program finds in its own memory
 * (TEB, PEB, process parameters): the kernel builds them a byte at a time into
 * pages it is about to map, and our ntdll reads the fields it needs through
 * these numbers too, so there is one statement of where each field is. They
 * are x64's. What says they are right is not this file: user/win/ntdll checks
 * each against mingw-w64's own winternl.h and winnt.h when it is built, the
 * artefact neither side here wrote.
 *
 * Freestanding: included by the kernel and by code mingw-w64 compiles. */

#include <stdint.h>

/* ---- status codes ---------------------------------------------------------------- */

#define VIBEOS_STATUS_SUCCESS                0x00000000u
#define VIBEOS_STATUS_UNSUCCESSFUL           0xC0000001u
#define VIBEOS_STATUS_NOT_IMPLEMENTED        0xC0000002u
#define VIBEOS_STATUS_INVALID_INFO_CLASS     0xC0000003u
#define VIBEOS_STATUS_INFO_LENGTH_MISMATCH   0xC0000004u
#define VIBEOS_STATUS_ACCESS_VIOLATION       0xC0000005u
#define VIBEOS_STATUS_INVALID_HANDLE         0xC0000008u
#define VIBEOS_STATUS_INVALID_PARAMETER      0xC000000Du
#define VIBEOS_STATUS_ACCESS_DENIED          0xC0000022u
#define VIBEOS_STATUS_OBJECT_TYPE_MISMATCH   0xC0000024u
#define VIBEOS_STATUS_DLL_NOT_FOUND          0xC0000135u
#define VIBEOS_STATUS_ENTRYPOINT_NOT_FOUND   0xC0000139u
#define VIBEOS_STATUS_PIPE_BROKEN            0xC000014Bu
#define VIBEOS_STATUS_NO_MEMORY              0xC0000017u
#define VIBEOS_STATUS_INVALID_IMAGE_FORMAT   0xC000007Bu
#define VIBEOS_STATUS_PROCEDURE_NOT_FOUND    0xC000007Au

/* Objects and waits (W2). The first two are successes - an NTSTATUS below
 * 0x80000000 - which a caller tests with NT_SUCCESS, not against zero. */
#define VIBEOS_STATUS_WAIT_0                 0x00000000u   /* + the index satisfied   */
#define VIBEOS_STATUS_ABANDONED_WAIT_0       0x00000080u
#define VIBEOS_STATUS_USER_APC               0x000000C0u
#define VIBEOS_STATUS_ALERTED                0x00000101u
#define VIBEOS_STATUS_TIMEOUT                0x00000102u
#define VIBEOS_STATUS_OBJECT_NAME_EXISTS     0x40000000u
#define VIBEOS_STATUS_OBJECT_NAME_INVALID    0xC0000033u
#define VIBEOS_STATUS_OBJECT_NAME_NOT_FOUND  0xC0000034u
#define VIBEOS_STATUS_OBJECT_NAME_COLLISION  0xC0000035u
#define VIBEOS_STATUS_OBJECT_PATH_NOT_FOUND  0xC000003Au
#define VIBEOS_STATUS_OBJECT_PATH_SYNTAX_BAD 0xC000003Bu
/* The file services' (W4). */
#define VIBEOS_STATUS_END_OF_FILE            0xC0000011u
#define VIBEOS_STATUS_FILE_IS_A_DIRECTORY    0xC00000BAu
#define VIBEOS_STATUS_NOT_A_DIRECTORY        0xC0000103u
#define VIBEOS_STATUS_CANNOT_DELETE          0xC0000121u
#define VIBEOS_STATUS_MUTANT_NOT_OWNED       0xC0000046u
#define VIBEOS_STATUS_SEMAPHORE_LIMIT_EXCEEDED 0xC0000047u
#define VIBEOS_STATUS_INSUFFICIENT_RESOURCES 0xC000009Au
#define VIBEOS_STATUS_HANDLE_NOT_CLOSABLE    0xC0000235u
#define VIBEOS_STATUS_BUFFER_TOO_SMALL       0xC0000023u
#define VIBEOS_STATUS_FILE_RENAMED           0xC00000D5u   /* Wine's ntstatus.h; not in mingw-w64's */
#define VIBEOS_STATUS_QUOTA_EXCEEDED         0xC0000044u
/* The memory services' (W3). */
#define VIBEOS_STATUS_CONFLICTING_ADDRESSES  0xC0000018u
#define VIBEOS_STATUS_UNABLE_TO_FREE_VM      0xC000001Au
#define VIBEOS_STATUS_NOT_COMMITTED          0xC000002Du
#define VIBEOS_STATUS_INVALID_PAGE_PROTECTION 0xC0000045u
#define VIBEOS_STATUS_FREE_VM_NOT_AT_BASE    0xC000009Fu
#define VIBEOS_STATUS_MEMORY_NOT_ALLOCATED   0xC00000A0u
#define VIBEOS_STATUS_NOT_SUPPORTED          0xC00000BBu
#define VIBEOS_STATUS_INVALID_PARAMETER_2    0xC00000F0u
#define VIBEOS_STATUS_INVALID_PARAMETER_3    0xC00000F1u
#define VIBEOS_STATUS_INVALID_PARAMETER_4    0xC00000F2u
#define VIBEOS_STATUS_INVALID_PARAMETER_6    0xC00000F4u
/* The sections' (W3 step 5). */
#define VIBEOS_STATUS_NOT_MAPPED_VIEW        0xC0000019u
#define VIBEOS_STATUS_UNABLE_TO_DELETE_SECTION 0xC000001Bu
#define VIBEOS_STATUS_INVALID_VIEW_SIZE      0xC000001Fu
#define VIBEOS_STATUS_INVALID_FILE_FOR_SECTION 0xC0000020u
#define VIBEOS_STATUS_SECTION_TOO_BIG        0xC0000040u
#define VIBEOS_STATUS_SECTION_NOT_IMAGE      0xC0000049u
#define VIBEOS_STATUS_SECTION_PROTECTION     0xC000004Eu
#define VIBEOS_STATUS_MAPPED_FILE_SIZE_ZERO  0xC000011Eu
#define VIBEOS_STATUS_MAPPED_ALIGNMENT       0xC0000220u

/* OBJECT_ATTRIBUTES on x64: 48 bytes. */
#define VIBEOS_OA_LENGTH          0x00u
#define VIBEOS_OA_ROOT_DIRECTORY  0x08u
#define VIBEOS_OA_OBJECT_NAME     0x10u   /* PUNICODE_STRING */
#define VIBEOS_OA_ATTRIBUTES      0x18u
#define VIBEOS_OA_SECURITY        0x20u
#define VIBEOS_OA_QOS             0x28u
#define VIBEOS_OA_SIZE            0x30u

#define VIBEOS_OBJ_INHERIT          0x00000002u
#define VIBEOS_OBJ_PERMANENT        0x00000010u
#define VIBEOS_OBJ_EXCLUSIVE        0x00000020u
#define VIBEOS_OBJ_CASE_INSENSITIVE 0x00000040u
#define VIBEOS_OBJ_OPENIF           0x00000080u
#define VIBEOS_OBJ_VALID_ATTRIBUTES 0x00001FF2u

/* Access rights: the standard ones, the generic ones each type maps onto its
 * own, and the types' own. */
#define VIBEOS_DELETE               0x00010000u
#define VIBEOS_READ_CONTROL         0x00020000u
#define VIBEOS_SYNCHRONIZE          0x00100000u
#define VIBEOS_STANDARD_RIGHTS_REQUIRED 0x000F0000u
#define VIBEOS_MAXIMUM_ALLOWED      0x02000000u
#define VIBEOS_GENERIC_READ         0x80000000u
#define VIBEOS_GENERIC_WRITE        0x40000000u
#define VIBEOS_GENERIC_EXECUTE      0x20000000u
#define VIBEOS_GENERIC_ALL          0x10000000u
#define VIBEOS_EVENT_QUERY_STATE    0x0001u
#define VIBEOS_EVENT_MODIFY_STATE   0x0002u
#define VIBEOS_EVENT_ALL_ACCESS     0x001F0003u

/* NtDuplicateObject's options. */
#define VIBEOS_DUPLICATE_CLOSE_SOURCE 0x1u
#define VIBEOS_DUPLICATE_SAME_ACCESS  0x2u

/* EVENT_TYPE, and EventBasicInformation (class 0): the type, then the state. */
#define VIBEOS_NT_NOTIFICATION_EVENT    0u
#define VIBEOS_NT_SYNCHRONIZATION_EVENT 1u
typedef struct {
    uint32_t event_type;
    int32_t event_state;
} vibeos_nt_event_basic_t;

/* Mutant and semaphore rights, and their basic information (class 0 each).
 * A mutant's "count" is its signal state: 1 free, 0 held once, -n held n+1
 * times. */
#define VIBEOS_MUTANT_QUERY_STATE     0x0001u
#define VIBEOS_MUTANT_ALL_ACCESS      0x001F0001u
#define VIBEOS_SEMAPHORE_QUERY_STATE  0x0001u
#define VIBEOS_SEMAPHORE_MODIFY_STATE 0x0002u
#define VIBEOS_SEMAPHORE_ALL_ACCESS   0x001F0003u
typedef struct {
    int32_t current_count;
    uint8_t owned_by_caller;
    uint8_t abandoned_state;
    uint8_t pad[2];
} vibeos_nt_mutant_basic_t;
typedef struct {
    int32_t current_count;
    int32_t maximum_count;
} vibeos_nt_semaphore_basic_t;

/* How many handles one wait may name: MAXIMUM_WAIT_OBJECTS. */
#define VIBEOS_NT_MAX_WAIT_OBJECTS 64u

/* The pseudo-handles every Windows program names itself by. */
#define VIBEOS_NT_CURRENT_PROCESS ((uint64_t)-1)
#define VIBEOS_NT_CURRENT_THREAD  ((uint64_t)-2)

/* ---- TEB ------------------------------------------------------------------------- *
 * Reached through %gs: gs:[0x30] is the TEB's own address, gs:[0x60] the PEB's. */
#define VIBEOS_TEB_EXCEPTION_LIST   0x000u
#define VIBEOS_TEB_STACK_BASE       0x008u
#define VIBEOS_TEB_STACK_LIMIT      0x010u
#define VIBEOS_TEB_SELF             0x030u
#define VIBEOS_TEB_CLIENT_PROCESS   0x040u   /* ClientId.UniqueProcess       */
#define VIBEOS_TEB_CLIENT_THREAD    0x048u   /* ClientId.UniqueThread        */
#define VIBEOS_TEB_TLS_POINTER      0x058u   /* ThreadLocalStoragePointer    */
#define VIBEOS_TEB_PEB              0x060u
#define VIBEOS_TEB_LAST_ERROR       0x068u
#define VIBEOS_TEB_TLS_SLOTS        0x1480u  /* TlsSlots[64]: TlsAlloc's   */
#define VIBEOS_TEB_TLS_SLOT_COUNT   64u
#define VIBEOS_TEB_SIZE             0x1838u  /* x64's TEB; we map two pages  */

/* ---- PEB ------------------------------------------------------------------------- */
#define VIBEOS_PEB_BEING_DEBUGGED   0x002u
#define VIBEOS_PEB_IMAGE_BASE       0x010u
#define VIBEOS_PEB_LDR              0x018u
#define VIBEOS_PEB_PROCESS_PARAMS   0x020u
#define VIBEOS_PEB_PROCESS_HEAP     0x030u
#define VIBEOS_PEB_NUMBER_OF_PROCESSORS 0x0B8u
#define VIBEOS_PEB_OS_MAJOR         0x118u
#define VIBEOS_PEB_OS_MINOR         0x11Cu
#define VIBEOS_PEB_OS_BUILD         0x120u
#define VIBEOS_PEB_OS_PLATFORM      0x124u
#define VIBEOS_PEB_SESSION_ID       0x2C0u
#define VIBEOS_PEB_SIZE             0x7C8u

/* ---- RTL_USER_PROCESS_PARAMETERS ------------------------------------------------- */
#define VIBEOS_RUPP_MAXIMUM_LENGTH  0x000u
#define VIBEOS_RUPP_LENGTH          0x004u
#define VIBEOS_RUPP_FLAGS           0x008u   /* 1: normalised, pointers absolute */
#define VIBEOS_RUPP_CONSOLE_HANDLE  0x010u
#define VIBEOS_RUPP_STD_INPUT       0x020u
#define VIBEOS_RUPP_STD_OUTPUT      0x028u
#define VIBEOS_RUPP_STD_ERROR       0x030u
#define VIBEOS_RUPP_CURDIR_PATH     0x038u   /* UNICODE_STRING                   */
#define VIBEOS_RUPP_CURDIR_HANDLE   0x048u
#define VIBEOS_RUPP_DLL_PATH        0x050u   /* UNICODE_STRING                   */
#define VIBEOS_RUPP_IMAGE_PATH      0x060u   /* UNICODE_STRING                   */
#define VIBEOS_RUPP_COMMAND_LINE    0x070u   /* UNICODE_STRING                   */
#define VIBEOS_RUPP_ENVIRONMENT     0x080u
#define VIBEOS_RUPP_SIZE            0x400u   /* the fields, padded; strings follow */
#define VIBEOS_RUPP_FLAG_NORMALIZED 0x1u

/* UNICODE_STRING on x64: Length and MaximumLength in bytes, then the pointer. */
#define VIBEOS_USTR_LENGTH          0x0u
#define VIBEOS_USTR_MAXIMUM         0x2u
#define VIBEOS_USTR_BUFFER          0x8u
#define VIBEOS_USTR_SIZE            0x10u

/* ---- the services' structures ----------------------------------------------------- */

/* IO_STATUS_BLOCK: the status (or a pointer, unioned), then Information. */
typedef struct {
    uint64_t status;      /* NTSTATUS in the low 32 bits */
    uint64_t information;
} vibeos_nt_iosb_t;

/* PROCESS_BASIC_INFORMATION, ProcessBasicInformation (class 0). */
typedef struct {
    uint64_t exit_status;
    uint64_t peb;
    uint64_t affinity_mask;
    uint64_t base_priority;
    uint64_t unique_process_id;
    uint64_t inherited_from_pid;
} vibeos_nt_process_basic_t;

#define VIBEOS_NT_PROCESS_BASIC_INFORMATION 0u

/* SYSTEM_BASIC_INFORMATION, SystemBasicInformation (class 0). */
typedef struct {
    uint32_t reserved;
    uint32_t timer_resolution;        /* in 100 ns */
    uint32_t page_size;
    uint32_t physical_pages;
    uint32_t lowest_physical_page;
    uint32_t highest_physical_page;
    uint32_t allocation_granularity;
    uint32_t pad;
    uint64_t minimum_user_address;
    uint64_t maximum_user_address;
    uint64_t active_processors_mask;
    uint8_t  processors;
    uint8_t  pad2[7];
} vibeos_nt_system_basic_t;

#define VIBEOS_NT_SYSTEM_BASIC_INFORMATION 0u

/* MEMORY_BASIC_INFORMATION, NtQueryVirtualMemory's MemoryBasicInformation
 * (class 0): 48 bytes on x64. */
typedef struct {
    uint64_t base_address;
    uint64_t allocation_base;
    uint32_t allocation_protect;
    uint16_t partition_id;
    uint16_t pad;
    uint64_t region_size;
    uint32_t state;
    uint32_t protect;
    uint32_t type;
    uint32_t pad2;
} vibeos_nt_memory_basic_t;

#define VIBEOS_NT_MEMORY_BASIC_INFORMATION 0u

/* SECTION_BASIC_INFORMATION, NtQuerySection's class 0: 24 bytes on x64. */
typedef struct {
    uint64_t base_address;
    uint32_t attributes;
    uint32_t pad;
    uint64_t size;
} vibeos_nt_section_basic_t;

#define VIBEOS_NT_SECTION_BASIC_INFORMATION 0u

/* AllocationType, FreeType, and MEMORY_BASIC_INFORMATION's State and Type. */
#define VIBEOS_NT_MEM_COMMIT      0x00001000u
#define VIBEOS_NT_MEM_RESERVE     0x00002000u
#define VIBEOS_NT_MEM_DECOMMIT    0x00004000u
#define VIBEOS_NT_MEM_RELEASE     0x00008000u
#define VIBEOS_NT_MEM_FREE        0x00010000u
#define VIBEOS_NT_MEM_PRIVATE     0x00020000u
#define VIBEOS_NT_MEM_MAPPED      0x00040000u
#define VIBEOS_NT_MEM_RESET       0x00080000u
#define VIBEOS_NT_MEM_TOP_DOWN    0x00100000u
#define VIBEOS_NT_MEM_WRITE_WATCH 0x00200000u
#define VIBEOS_NT_MEM_IMAGE       0x01000000u
#define VIBEOS_NT_MEM_RESET_UNDO  0x01000000u   /* AllocationType's, not Type's */
#define VIBEOS_NT_MEM_LARGE_PAGES 0x20000000u

/* Windows 10's placeholders (NtAllocateVirtualMemoryEx's AllocationType, and
 * NtFreeVirtualMemory's FreeType for the last two): reserved address space
 * that can be split, joined and replaced by an allocation in place. */
#define VIBEOS_NT_MEM_REPLACE_PLACEHOLDER   0x00004000u
#define VIBEOS_NT_MEM_RESERVE_PLACEHOLDER   0x00040000u
#define VIBEOS_NT_MEM_COALESCE_PLACEHOLDERS 0x00000001u
#define VIBEOS_NT_MEM_PRESERVE_PLACEHOLDER  0x00000002u

/* MEM_EXTENDED_PARAMETER: 16 bytes, the type in the low eight bits of the
 * first quadword and a value or a pointer in the second. */
typedef struct {
    uint64_t type;
    uint64_t value;
} vibeos_nt_mem_ext_param_t;

#define VIBEOS_NT_MEM_EXT_TYPE_MASK            0xFFull
#define VIBEOS_NT_MEM_EXT_ADDRESS_REQUIREMENTS 1u
#define VIBEOS_NT_MEM_EXT_NUMA_NODE            2u
#define VIBEOS_NT_MEM_EXT_PARTITION_HANDLE     3u
#define VIBEOS_NT_MEM_EXT_USER_PHYSICAL_HANDLE 4u
#define VIBEOS_NT_MEM_EXT_ATTRIBUTE_FLAGS      5u
#define VIBEOS_NT_MEM_EXT_IMAGE_MACHINE        6u
#define VIBEOS_NT_MEM_EXT_MAX                  7u
#define VIBEOS_NT_MEM_EXT_EC_CODE              0x40u   /* an attribute flag: ARM64EC */

/* MEM_ADDRESS_REQUIREMENTS: 24 bytes. */
typedef struct {
    uint64_t lowest_starting_address;
    uint64_t highest_ending_address;
    uint64_t alignment;
} vibeos_nt_mem_address_requirements_t;

/* The highest address SystemBasicInformation reports a program may use - and
 * what an allocation's highest ending address is measured against. */
#define VIBEOS_NT_MAX_USER_ADDRESS 0x00007FFFFFFEFFFFull

/* Page protections: one of the first eight, with modifiers. */
#define VIBEOS_NT_PAGE_NOACCESS          0x001u
#define VIBEOS_NT_PAGE_READONLY          0x002u
#define VIBEOS_NT_PAGE_READWRITE         0x004u
#define VIBEOS_NT_PAGE_WRITECOPY         0x008u
#define VIBEOS_NT_PAGE_EXECUTE           0x010u
#define VIBEOS_NT_PAGE_EXECUTE_READ      0x020u
#define VIBEOS_NT_PAGE_EXECUTE_READWRITE 0x040u
#define VIBEOS_NT_PAGE_EXECUTE_WRITECOPY 0x080u
#define VIBEOS_NT_PAGE_GUARD             0x100u
#define VIBEOS_NT_PAGE_NOCACHE           0x200u
#define VIBEOS_NT_PAGE_WRITECOMBINE      0x400u

/* Windows reserves address space in pieces of this; it commits by the page. */
#define VIBEOS_NT_ALLOCATION_GRANULARITY 0x10000ull

/* 100-ns intervals between 1601-01-01 and 1970-01-01: Windows counts from the
 * first, the kernel's clock from the second. */
#define VIBEOS_NT_EPOCH_DELTA 116444736000000000ull

/* The frequency NtQueryPerformanceCounter reports: Windows' own on current
 * machines, and the clock's unit, 100 ns. */
#define VIBEOS_NT_PERF_FREQUENCY 10000000ull

#endif
