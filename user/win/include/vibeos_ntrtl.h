#ifndef VIBEOS_NTRTL_H
#define VIBEOS_NTRTL_H

/* The structures and constants of our ntdll's Rtl runtime (docs/abi/windows.md W2).
 *
 * Types only, and none of them under Windows' own names: the Rtl core is also
 * compiled on the Linux host for tests/win/ntrtl_tests.c, where no Windows
 * header exists, and user/win/ntdll/rtl/layout_check.c includes this file next
 * to mingw-w64's windows.h, whose names would collide. The layouts are x64's
 * (LLP64: ULONG is 32 bits, WCHAR 16), and layout_check.c holds every one that
 * mingw-w64 declares against mingw-w64's own, at every build.
 *
 * The prototypes are in vibeos_ntdll.h. */

#include <stddef.h>
#include <stdint.h>

typedef uint16_t vibeos_wchar_t;
typedef int32_t  vibeos_ntstatus_t;
typedef uint8_t  vibeos_boolean_t;

/* STATUS_* the Rtl functions return. Written as unsigned like nt_layout.h's;
 * a function returns them through the cast below. */
#define VIBEOS_RTL_ST(x) ((vibeos_ntstatus_t)(x))
#define VIBEOS_STATUS_BUFFER_OVERFLOW      0x80000005u
#define VIBEOS_STATUS_BUFFER_TOO_SMALL     0xC0000023u
#define VIBEOS_STATUS_VARIABLE_NOT_FOUND   0xC0000100u
#define VIBEOS_STATUS_NAME_TOO_LONG        0xC0000106u

/* STRING and UNICODE_STRING. Lengths are in BYTES, and a UNICODE_STRING's
 * Buffer need not be terminated. */
typedef struct {
    uint16_t Length;
    uint16_t MaximumLength;
    char    *Buffer;
} vibeos_astring_t;

typedef struct {
    uint16_t        Length;
    uint16_t        MaximumLength;
    vibeos_wchar_t *Buffer;
} vibeos_ustring_t;

#define VIBEOS_RTL_DUPLICATE_UNICODE_STRING_NULL_TERMINATE  1u
#define VIBEOS_RTL_DUPLICATE_UNICODE_STRING_ALLOCATE_NULL_STRING 2u

/* IN_ADDR and IN6_ADDR: network order, as in memory. */
typedef struct { uint8_t Byte[4]; }  vibeos_in_addr_t;
typedef struct { uint8_t Byte[16]; } vibeos_in6_addr_t;

/* RTL_HANDLE_TABLE and its entries. An entry's first word is the next free
 * entry while it is free and flags once it is allocated; RTL_HANDLE_ALLOCATED
 * (bit 0) is set by the caller of RtlAllocateHandle, not by it. */
typedef struct vibeos_rtl_handle {
    struct vibeos_rtl_handle *Next;
} vibeos_rtl_handle_t;

#define VIBEOS_RTL_HANDLE_ALLOCATED 1u

typedef struct {
    uint32_t             MaxHandleCount;
    uint32_t             HandleSize;
    uint32_t             Reserved[2];
    vibeos_rtl_handle_t *FreeHandles;
    vibeos_rtl_handle_t *CommittedHandles;
    vibeos_rtl_handle_t *UnCommittedHandles;
    vibeos_rtl_handle_t *MaxReservedHandles;
} vibeos_rtl_handle_table_t;

/* RTL_RELATIVE_NAME_U: what RtlDosPathNameToNtPathName_U may fill in beside the
 * name. We never produce a relative name, so it is zeroed. */
typedef struct {
    vibeos_ustring_t RelativeName;
    void            *ContainingDirectory;
    void            *CurDirRef;
} vibeos_rtl_relative_name_u_t;

/* RTL_CRITICAL_SECTION and its debug record, 40 and 48 bytes on x64. */
typedef struct vibeos_rtl_list_entry {
    struct vibeos_rtl_list_entry *Flink;
    struct vibeos_rtl_list_entry *Blink;
} vibeos_rtl_list_entry_t;

struct vibeos_rtl_critical_section;

typedef struct {
    uint16_t                            Type;
    uint16_t                            CreatorBackTraceIndex;
    struct vibeos_rtl_critical_section *CriticalSection;
    vibeos_rtl_list_entry_t             ProcessLocksList;
    uint32_t                            EntryCount;
    uint32_t                            ContentionCount;
    uint32_t                            Flags;
    uint16_t                            CreatorBackTraceIndexHigh;
    uint16_t                            SpareWORD;
} vibeos_rtl_critical_section_debug_t;

typedef struct vibeos_rtl_critical_section {
    vibeos_rtl_critical_section_debug_t *DebugInfo;
    int32_t                              LockCount;       /* -1: free             */
    int32_t                              RecursionCount;
    void                                *OwningThread;    /* ClientId.UniqueThread */
    void                                *LockSemaphore;
    uint64_t                             SpinCount;
} vibeos_rtl_critical_section_t;

/* IMAGE_NT_HEADERS64 and what it holds. */
typedef struct {
    uint32_t VirtualAddress;
    uint32_t Size;
} vibeos_image_data_directory_t;

typedef struct {
    uint16_t Machine;
    uint16_t NumberOfSections;
    uint32_t TimeDateStamp;
    uint32_t PointerToSymbolTable;
    uint32_t NumberOfSymbols;
    uint16_t SizeOfOptionalHeader;
    uint16_t Characteristics;
} vibeos_image_file_header_t;

typedef struct {
    uint16_t Magic;
    uint8_t  MajorLinkerVersion;
    uint8_t  MinorLinkerVersion;
    uint32_t SizeOfCode;
    uint32_t SizeOfInitializedData;
    uint32_t SizeOfUninitializedData;
    uint32_t AddressOfEntryPoint;
    uint32_t BaseOfCode;
    uint64_t ImageBase;
    uint32_t SectionAlignment;
    uint32_t FileAlignment;
    uint16_t MajorOperatingSystemVersion;
    uint16_t MinorOperatingSystemVersion;
    uint16_t MajorImageVersion;
    uint16_t MinorImageVersion;
    uint16_t MajorSubsystemVersion;
    uint16_t MinorSubsystemVersion;
    uint32_t Win32VersionValue;
    uint32_t SizeOfImage;
    uint32_t SizeOfHeaders;
    uint32_t CheckSum;
    uint16_t Subsystem;
    uint16_t DllCharacteristics;
    uint64_t SizeOfStackReserve;
    uint64_t SizeOfStackCommit;
    uint64_t SizeOfHeapReserve;
    uint64_t SizeOfHeapCommit;
    uint32_t LoaderFlags;
    uint32_t NumberOfRvaAndSizes;
    vibeos_image_data_directory_t DataDirectory[16];
} vibeos_image_optional_header64_t;

typedef struct {
    uint32_t                         Signature;
    vibeos_image_file_header_t       FileHeader;
    vibeos_image_optional_header64_t OptionalHeader;
} vibeos_image_nt_headers64_t;

typedef struct {
    uint8_t  Name[8];
    uint32_t VirtualSize;
    uint32_t VirtualAddress;
    uint32_t SizeOfRawData;
    uint32_t PointerToRawData;
    uint32_t PointerToRelocations;
    uint32_t PointerToLinenumbers;
    uint16_t NumberOfRelocations;
    uint16_t NumberOfLinenumbers;
    uint32_t Characteristics;
} vibeos_image_section_header_t;

#define VIBEOS_IMAGE_DOS_SIGNATURE        0x5A4Du
#define VIBEOS_IMAGE_NT_SIGNATURE         0x00004550u
#define VIBEOS_IMAGE_NT_OPTIONAL_HDR32_MAGIC 0x10Bu
#define VIBEOS_IMAGE_NT_OPTIONAL_HDR64_MAGIC 0x20Bu

#endif
