#include "uefi_file_io.h"

#include "uefi_serial.h"

#define UEFI_PAGE_SIZE 4096ull
#define EFI_OPEN_MODE_READ 0x0000000000000001ull

typedef struct efi_file_protocol EFI_FILE_PROTOCOL;

typedef EFI_STATUS (*EFI_FILE_OPEN)(
    EFI_FILE_PROTOCOL *This,
    EFI_FILE_PROTOCOL **NewHandle,
    const uint16_t *FileName,
    uint64_t OpenMode,
    uint64_t Attributes
);

typedef EFI_STATUS (*EFI_FILE_CLOSE)(EFI_FILE_PROTOCOL *This);

typedef EFI_STATUS (*EFI_FILE_READ)(
    EFI_FILE_PROTOCOL *This,
    size_t *BufferSize,
    void *Buffer
);

typedef EFI_STATUS (*EFI_FILE_GET_INFO)(
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
    EFI_STATUS (*OpenVolume)(
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

static const EFI_GUID VIBEOS_EFI_SIMPLE_FILE_SYSTEM_GUID = {
    0x964e5b22u, 0x6459u, 0x11d2u, {0x8eu, 0x39u, 0x00u, 0xa0u, 0xc9u, 0x69u, 0x72u, 0x3bu}
};

static const EFI_GUID VIBEOS_EFI_FILE_INFO_GUID = {
    0x09576e92u, 0x6d3fu, 0x11d2u, {0x8eu, 0x39u, 0x00u, 0xa0u, 0xc9u, 0x69u, 0x72u, 0x3bu}
};

static uint64_t round_up_pages(uint64_t size) {
    if (size == 0) {
        return 0;
    }
    return (size + (UEFI_PAGE_SIZE - 1ull)) / UEFI_PAGE_SIZE;
}

static int uefi_allocate_bytes(EFI_SYSTEM_TABLE *st, uint64_t size, void **out_buffer) {
    uint64_t pages;
    uint64_t address = 0;
    EFI_STATUS status;

    if (!st || !st->BootServices || !st->BootServices->AllocatePages || !out_buffer) {
        return -1;
    }
    if (size == 0) {
        return -1;
    }

    pages = round_up_pages(size);
    if (pages == 0) {
        return -1;
    }

    status = st->BootServices->AllocatePages(AllocateAnyPages, EfiLoaderData, (size_t)pages, &address);
    if (status != EFI_SUCCESS) {
        return -1;
    }

    *out_buffer = (void *)(uintptr_t)address;
    return 0;
}

static void uefi_close_file_if_open(EFI_FILE_PROTOCOL *file) {
    if (file && file->Close) {
        (void)file->Close(file);
    }
}

void uefi_file_free_buffer(EFI_SYSTEM_TABLE *st, void *buffer, uint64_t size) {
    uint64_t pages;
    if (!st || !st->BootServices || !st->BootServices->FreePages || !buffer || size == 0) {
        return;
    }
    pages = round_up_pages(size);
    if (pages == 0) {
        return;
    }
    (void)st->BootServices->FreePages((uint64_t)(uintptr_t)buffer, (size_t)pages);
}

int uefi_file_read_all(EFI_HANDLE image_handle __attribute__((unused)),
                       EFI_SYSTEM_TABLE *st,
                       const uint16_t *path,
                       void **out_buf,
                       uint64_t *out_size) {
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = 0;
    EFI_FILE_PROTOCOL *root = 0;
    EFI_FILE_PROTOCOL *file = 0;
    EFI_FILE_INFO *info = 0;
    void *file_buffer = 0;
    void *info_buffer = 0;
    uint64_t file_size = 0;
    size_t info_size = 0;
    size_t read_size = 0;
    EFI_STATUS status;

    if (!st || !st->BootServices || !st->BootServices->LocateProtocol || !path || !out_buf || !out_size) {
        return -1;
    }

    *out_buf = 0;
    *out_size = 0;

    status = st->BootServices->LocateProtocol((EFI_GUID *)&VIBEOS_EFI_SIMPLE_FILE_SYSTEM_GUID, 0, (void **)&fs);
    if (status != EFI_SUCCESS || !fs || !fs->OpenVolume) {
        uefi_serial_puts("[ERROR] UEFI file I/O: filesystem protocol unavailable\n");
        return -1;
    }

    status = fs->OpenVolume(fs, &root);
    if (status != EFI_SUCCESS || !root || !root->Open) {
        uefi_serial_puts("[ERROR] UEFI file I/O: failed to open volume\n");
        return -1;
    }

    status = root->Open(root, &file, path, EFI_OPEN_MODE_READ, 0);
    if (status != EFI_SUCCESS || !file || !file->GetInfo || !file->Read) {
        uefi_serial_puts("[ERROR] UEFI file I/O: kernel file open failed\n");
        uefi_close_file_if_open(root);
        return -1;
    }

    status = file->GetInfo(file, (EFI_GUID *)&VIBEOS_EFI_FILE_INFO_GUID, &info_size, 0);
    if (status != EFI_BUFFER_TOO_SMALL || info_size == 0) {
        uefi_serial_puts("[ERROR] UEFI file I/O: failed to query file metadata size\n");
        uefi_close_file_if_open(file);
        uefi_close_file_if_open(root);
        return -1;
    }

    if (uefi_allocate_bytes(st, (uint64_t)info_size, &info_buffer) != 0) {
        uefi_serial_puts("[ERROR] UEFI file I/O: metadata allocation failed\n");
        uefi_close_file_if_open(file);
        uefi_close_file_if_open(root);
        return -1;
    }
    info = (EFI_FILE_INFO *)info_buffer;

    status = file->GetInfo(file, (EFI_GUID *)&VIBEOS_EFI_FILE_INFO_GUID, &info_size, info);
    if (status != EFI_SUCCESS || info->FileSize == 0) {
        uefi_serial_puts("[ERROR] UEFI file I/O: invalid file metadata\n");
        uefi_file_free_buffer(st, info_buffer, (uint64_t)info_size);
        uefi_close_file_if_open(file);
        uefi_close_file_if_open(root);
        return -1;
    }

    file_size = info->FileSize;
    if (uefi_allocate_bytes(st, file_size, &file_buffer) != 0) {
        uefi_serial_puts("[ERROR] UEFI file I/O: file buffer allocation failed\n");
        uefi_file_free_buffer(st, info_buffer, (uint64_t)info_size);
        uefi_close_file_if_open(file);
        uefi_close_file_if_open(root);
        return -1;
    }

    read_size = (size_t)file_size;
    status = file->Read(file, &read_size, file_buffer);
    if (status != EFI_SUCCESS || (uint64_t)read_size != file_size) {
        uefi_serial_puts("[ERROR] UEFI file I/O: short/failed read\n");
        uefi_file_free_buffer(st, file_buffer, file_size);
        uefi_file_free_buffer(st, info_buffer, (uint64_t)info_size);
        uefi_close_file_if_open(file);
        uefi_close_file_if_open(root);
        return -1;
    }

    uefi_file_free_buffer(st, info_buffer, (uint64_t)info_size);
    uefi_close_file_if_open(file);
    uefi_close_file_if_open(root);

    *out_buf = file_buffer;
    *out_size = file_size;
    return 0;
}
