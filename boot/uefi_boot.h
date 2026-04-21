#ifndef VIBEOS_UEFI_BOOT_H
#define VIBEOS_UEFI_BOOT_H

#include <stddef.h>
#include <stdint.h>

/* EFI types - minimal subset for bootloader */
typedef uint64_t EFI_STATUS;
typedef void *EFI_HANDLE;
typedef void *EFI_EVENT;
typedef struct {
    uint32_t Data1;
    uint16_t Data2;
    uint16_t Data3;
    uint8_t Data4[8];
} EFI_GUID;

/* EFI status codes */
#define EFI_SUCCESS 0x0000000000000000ULL
#define EFI_LOAD_ERROR 0x0000000000000001ULL
#define EFI_INVALID_PARAMETER 0x0000000000000002ULL
#define EFI_UNSUPPORTED 0x0000000000000003ULL
#define EFI_BAD_BUFFER_SIZE 0x0000000000000004ULL
#define EFI_BUFFER_TOO_SMALL 0x0000000000000005ULL
#define EFI_NOT_READY 0x0000000000000006ULL
#define EFI_DEVICE_ERROR 0x0000000000000007ULL
#define EFI_WRITE_PROTECTED 0x0000000000000008ULL
#define EFI_OUT_OF_RESOURCES 0x0000000000000009ULL
#define EFI_VOLUME_CORRUPTED 0x000000000000000aULL

/* Memory types */
typedef enum {
    EfiReservedMemoryType = 0,
    EfiLoaderCode = 1,
    EfiLoaderData = 2,
    EfiBootServicesCode = 3,
    EfiBootServicesData = 4,
    EfiRuntimeServicesCode = 5,
    EfiRuntimeServicesData = 6,
    EfiConventionalMemory = 7,
    EfiUnusableMemory = 8,
    EfiACPIReclaimMemory = 9,
    EfiACPIMemoryNVS = 10,
    EfiMemoryMappedIO = 11,
    EfiMemoryMappedIOPortSpace = 12,
    EfiPalCode = 13,
    EfiPersistentMemory = 14,
    EfiMaxMemoryType = 15
} EFI_MEMORY_TYPE;

/* EFI memory descriptor */
typedef struct {
    uint32_t Type;
    uint32_t Pad;
    uint64_t PhysicalStart;
    uint64_t VirtualStart;
    uint64_t NumberOfPages;
    uint64_t Attribute;
} EFI_MEMORY_DESCRIPTOR;

/* EFI memory descriptor attributes */
#define EFI_MEMORY_UC 0x0000000000000001ULL
#define EFI_MEMORY_WC 0x0000000000000002ULL
#define EFI_MEMORY_WT 0x0000000000000004ULL
#define EFI_MEMORY_WB 0x0000000000000008ULL
#define EFI_MEMORY_UNCACHABLE 0x0000000000000010ULL
#define EFI_MEMORY_RUNTIME 0x8000000000000000ULL

/* EFI Time */
typedef struct {
    uint16_t Year;
    uint8_t Month;
    uint8_t Day;
    uint8_t Hour;
    uint8_t Minute;
    uint8_t Second;
    uint8_t Pad1;
    uint32_t Nanosecond;
    int16_t TimeZone;
    uint8_t Daylight;
    uint8_t Pad2;
} EFI_TIME;

/* Graphic Output Pixel Format */
typedef enum {
    PixelRedGreenBlueReserved8BitPerColor = 0,
    PixelBlueGreenRedReserved8BitPerColor = 1,
    PixelBitMask = 2,
    PixelBltOnly = 3,
    PixelFormatMax = 4
} EFI_GRAPHICS_PIXEL_FORMAT;

/* Graphics Output Blt Operation */
typedef enum {
    EfiBltVideoFill,
    EfiBltVideoToBltBuffer,
    EfiBltBufferToVideo,
    EfiBltVideoToVideo,
    EfiGraphicsOutputBltOperationMax
} EFI_GRAPHICS_OUTPUT_BLT_OPERATION;

/* Graphics Output Mode Information */
typedef struct {
    uint32_t Version;
    uint32_t HorizontalResolution;
    uint32_t VerticalResolution;
    EFI_GRAPHICS_PIXEL_FORMAT PixelFormat;
    uint32_t PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

/* Graphics Output Protocol Blt Pixel Union */
typedef union {
    uint32_t Raw;
    struct {
        uint8_t Blue;
        uint8_t Green;
        uint8_t Red;
        uint8_t Reserved;
    } Rgb;
} EFI_GRAPHICS_OUTPUT_BLT_PIXEL;

#endif
