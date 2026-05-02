#ifndef VIBEOS_UEFI_FILE_IO_H
#define VIBEOS_UEFI_FILE_IO_H

#include <stdint.h>

#include "uefi_protocol.h"

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
