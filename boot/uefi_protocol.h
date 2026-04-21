#ifndef VIBEOS_UEFI_PROTOCOL_H
#define VIBEOS_UEFI_PROTOCOL_H

#include "vibeos/boot.h"
#include "vibeos/bootloader.h"
#include "uefi_boot.h"

/* Forward declarations */
struct _EFI_BOOT_SERVICES;
struct _EFI_RUNTIME_SERVICES;
struct _EFI_SYSTEM_TABLE;

/* EFI Boot Services */
typedef EFI_STATUS (*EFI_GET_MEMORY_MAP)(
    size_t *MemoryMapSize,
    EFI_MEMORY_DESCRIPTOR *MemoryMap,
    size_t *MapKey,
    size_t *DescriptorSize,
    uint32_t *DescriptorVersion
);

typedef EFI_STATUS (*EFI_ALLOCATE_PAGES)(
    uint32_t Type,
    EFI_MEMORY_TYPE MemoryType,
    size_t Pages,
    uint64_t *Memory
);

typedef EFI_STATUS (*EFI_EXIT_BOOT_SERVICES)(
    EFI_HANDLE ImageHandle,
    size_t MapKey
);

typedef EFI_STATUS (*EFI_STALL)(
    size_t MicroSeconds
);

typedef EFI_STATUS (*EFI_LOCATE_PROTOCOL)(
    EFI_GUID *Protocol,
    void *Registration,
    void **Interface
);

typedef struct {
    EFI_GET_MEMORY_MAP GetMemoryMap;
    EFI_ALLOCATE_PAGES AllocatePages;
    void *FreePages;
    void *AllocatePool;
    void *FreePool;
    void *CreateEvent;
    void *SetTimer;
    void *WaitForEvent;
    void *SignalEvent;
    void *CloseEvent;
    void *CheckEvent;
    void *InstallProtocolInterface;
    void *ReinstallProtocolInterface;
    void *UninstallProtocolInterface;
    void *HandleProtocol;
    void *Reserved;
    void *RegisterProtocolNotify;
    void *LocateHandle;
    EFI_LOCATE_PROTOCOL LocateProtocol;
    void *InstallMultipleProtocolInterfaces;
    void *UninstallMultipleProtocolInterfaces;
    void *CalculateCrc32;
    void *CopyMem;
    void *SetMem;
    void *CreateEventEx;
    EFI_EXIT_BOOT_SERVICES ExitBootServices;
    EFI_STALL Stall;
    void *SetWatchdogTimer;
    void *ConnectController;
    void *DisconnectController;
    void *OpenProtocol;
    void *CloseProtocol;
    void *OpenProtocolInformation;
    void *ProtocolsPerHandle;
    void *LocateHandleBuffer;
    void *LocateProtocolBuffer;
    void *InstallConfigurationTable;
} EFI_BOOT_SERVICES;

/* EFI Configuration Table Entry */
typedef struct {
    EFI_GUID VendorGuid;
    void *VendorTable;
} EFI_CONFIGURATION_TABLE_ENTRY;

/* EFI System Table */
typedef struct _EFI_SYSTEM_TABLE {
    uint64_t Signature;
    uint32_t Revision;
    uint32_t HeaderSize;
    uint32_t CRC32;
    uint32_t Reserved;
    EFI_BOOT_SERVICES *BootServices;
    struct _EFI_RUNTIME_SERVICES *RuntimeServices;
    size_t NumberOfTableEntries;
    EFI_CONFIGURATION_TABLE_ENTRY *ConfigurationTable;
    void *ConOut;
    void *ConIn;
    void *ConErrOut;
} EFI_SYSTEM_TABLE;

/* Common GUIDs */
extern EFI_GUID ACPI_TABLE_GUID;
extern EFI_GUID SMBIOS_TABLE_GUID;
extern EFI_GUID GRAPHICS_OUTPUT_PROTOCOL_GUID;
extern EFI_GUID SERIAL_IO_PROTOCOL_GUID;

/* AllocatePages Type */
#define AllocateAnyPages 0
#define AllocateMaxAddress 1
#define AllocateAddress 2

/* Serial I/O Protocol */
typedef EFI_STATUS (*EFI_SERIAL_SET_ATTRIBUTES)(
    void *This,
    uint64_t BaudRate,
    uint32_t ReceiveFifoDepth,
    uint32_t Timeout,
    uint32_t Parity,
    uint8_t DataBits,
    uint32_t StopBits
);

typedef EFI_STATUS (*EFI_SERIAL_WRITE)(
    void *This,
    size_t *BufferSize,
    void *Buffer
);

typedef EFI_STATUS (*EFI_SERIAL_READ)(
    void *This,
    size_t *BufferSize,
    void *Buffer
);

typedef struct {
    uint32_t ControlMask;
    uint32_t ControlSet;
    uint32_t ControlClear;
} EFI_SERIAL_IO_MODE;

typedef struct {
    EFI_SERIAL_WRITE Write;
    EFI_SERIAL_READ Read;
    EFI_SERIAL_SET_ATTRIBUTES SetAttributes;
    void *GetControl;
    void *SetControl;
    EFI_SERIAL_IO_MODE *Mode;
    uint32_t DeviceTypeGuid;
} EFI_SERIAL_IO_PROTOCOL;

/* Graphics Output Protocol */
typedef EFI_STATUS (*EFI_GRAPHICS_OUTPUT_QUERY_MODE)(
    void *This,
    uint32_t ModeNumber,
    size_t *SizeOfInfo,
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **Info
);

typedef EFI_STATUS (*EFI_GRAPHICS_OUTPUT_SET_MODE)(
    void *This,
    uint32_t ModeNumber
);

typedef EFI_STATUS (*EFI_GRAPHICS_OUTPUT_BLT)(
    void *This,
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL *BltBuffer,
    EFI_GRAPHICS_OUTPUT_BLT_OPERATION BltOperation,
    size_t SourceX,
    size_t SourceY,
    size_t DestinationX,
    size_t DestinationY,
    size_t Width,
    size_t Height,
    size_t Delta
);

typedef struct {
    uint32_t MaxMode;
    uint32_t Mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
    size_t SizeOfInfo;
    uint64_t FrameBufferBase;
    size_t FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

typedef struct {
    EFI_GRAPHICS_OUTPUT_QUERY_MODE QueryMode;
    EFI_GRAPHICS_OUTPUT_SET_MODE SetMode;
    EFI_GRAPHICS_OUTPUT_BLT Blt;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *Mode;
} EFI_GRAPHICS_OUTPUT_PROTOCOL;

/* Protocol wrapper functions */
int uefi_acquire_memory_map(EFI_SYSTEM_TABLE *st, vibeos_memory_region_t *out_regions, uint64_t max_regions, uint64_t *out_count);
int uefi_discover_firmware_tables(EFI_SYSTEM_TABLE *st, uint64_t *out_acpi_rsdp, uint64_t *out_smbios_entry);
int uefi_get_framebuffer(EFI_SYSTEM_TABLE *st, uint64_t *out_buffer_base, uint32_t *out_width, uint32_t *out_height);
int uefi_serial_init(EFI_SYSTEM_TABLE *st);
int uefi_serial_puts(const char *str);
int uefi_serial_putc(int c);

#endif
