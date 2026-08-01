#ifndef VIBEOS_UEFI_FILE_IO_H
#define VIBEOS_UEFI_FILE_IO_H

#include <stdint.h>

#include "uefi_protocol.h"

/* EFI file-system protocol layout.
 *
 * These live here rather than inside uefi_file_io.c because the bootloader
 * tests need the exact same types. When the tests declared their own
 * look-alike structs, the mock functions had different parameter types from
 * the pointers they were installed on, so every call through them was
 * undefined behaviour - which is what the sanitizer job caught. Sharing one
 * definition removes the mismatch instead of silencing it.
 */
typedef struct efi_file_protocol EFI_FILE_PROTOCOL;

typedef EFI_STATUS(EFIAPI *EFI_FILE_OPEN)(
    EFI_FILE_PROTOCOL *This,
    EFI_FILE_PROTOCOL **NewHandle,
    const uint16_t *FileName,
    uint64_t OpenMode,
    uint64_t Attributes
);

typedef EFI_STATUS(EFIAPI *EFI_FILE_CLOSE)(EFI_FILE_PROTOCOL *This);

typedef EFI_STATUS(EFIAPI *EFI_FILE_READ)(
    EFI_FILE_PROTOCOL *This,
    size_t *BufferSize,
    void *Buffer
);

typedef EFI_STATUS(EFIAPI *EFI_FILE_GET_INFO)(
    EFI_FILE_PROTOCOL *This,
    EFI_GUID *InformationType,
    size_t *BufferSize,
    void *Buffer
);

struct efi_file_protocol {
    uint64_t Revision;
    EFI_FILE_OPEN Open;
    EFI_FILE_CLOSE Close;
    void *Delete;
    EFI_FILE_READ Read;
    void *Write;
    void *GetPosition;
    void *SetPosition;
    EFI_FILE_GET_INFO GetInfo;
    void *SetInfo;
    void *Flush;
};

typedef struct efi_simple_file_system_protocol {
    uint64_t Revision;
    EFI_STATUS(EFIAPI *OpenVolume)(
        struct efi_simple_file_system_protocol *This,
        EFI_FILE_PROTOCOL **Root
    );
} EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

typedef struct efi_file_info {
    uint64_t Size;
    uint64_t FileSize;
    uint64_t PhysicalSize;
    EFI_TIME CreateTime;
    EFI_TIME LastAccessTime;
    EFI_TIME ModificationTime;
    uint64_t Attribute;
    uint16_t FileName[1];
} EFI_FILE_INFO;

/* Read an entire file from the active EFI system partition.
 * The returned buffer is allocated with AllocatePages and must be released
 * through uefi_file_free_buffer().
 */
int uefi_file_read_all(EFI_HANDLE image_handle,
                       EFI_SYSTEM_TABLE *st,
                       const uint16_t *path,
                       void **out_buf,
                       uint64_t *out_size);

/* Release a buffer previously allocated by uefi_file_read_all(). */
void uefi_file_free_buffer(EFI_SYSTEM_TABLE *st, void *buffer, uint64_t size);

#endif
