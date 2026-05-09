#ifndef VIBEOS_FS_H
#define VIBEOS_FS_H

#include <stdint.h>
#include <stddef.h>

#include "vibeos/policy.h"
#include "vibeos/security_model.h"

#define VIBEOS_MAX_MOUNTS 16u
#define VIBEOS_MAX_OPEN_FILES 64u
#define VIBEOS_MAX_PERSIST_FILES 32u
#define VIBEOS_PERSIST_FILE_MAX_BYTES 256u

typedef struct vibeos_mount_entry {
    uint32_t id;
    uint32_t active;
} vibeos_mount_entry_t;

typedef struct vibeos_file_entry {
    uint32_t fd;
    uint32_t in_use;
} vibeos_file_entry_t;

typedef struct vibeos_persist_file_entry {
    uint32_t mount_id;
    uint32_t key;
    uint32_t in_use;
    uint32_t size;
    uint8_t data[VIBEOS_PERSIST_FILE_MAX_BYTES];
} vibeos_persist_file_entry_t;

typedef struct vibeos_vfs_runtime {
    vibeos_mount_entry_t mounts[VIBEOS_MAX_MOUNTS];
    vibeos_file_entry_t files[VIBEOS_MAX_OPEN_FILES];
    vibeos_persist_file_entry_t persist[VIBEOS_MAX_PERSIST_FILES];
    uint32_t next_mount_id;
    uint32_t next_fd;
    uint32_t persistent_files;
} vibeos_vfs_runtime_t;

int vibeos_vfs_runtime_init(vibeos_vfs_runtime_t *rt);
int vibeos_vfs_mount(vibeos_vfs_runtime_t *rt, uint32_t *out_mount_id);
int vibeos_vfs_unmount(vibeos_vfs_runtime_t *rt, uint32_t mount_id);
int vibeos_vfs_active_mounts(const vibeos_vfs_runtime_t *rt, uint32_t *out_count);
int vibeos_vfs_open(vibeos_vfs_runtime_t *rt, uint32_t mount_id, uint32_t *out_fd);
int vibeos_vfs_open_secure(vibeos_vfs_runtime_t *rt, uint32_t mount_id, const vibeos_policy_state_t *policy, const vibeos_security_token_t *token, uint32_t *out_fd);
int vibeos_vfs_close(vibeos_vfs_runtime_t *rt, uint32_t fd);
int vibeos_vfs_persist_write(vibeos_vfs_runtime_t *rt, uint32_t mount_id, uint32_t key, const void *data, size_t len);
int vibeos_vfs_persist_read(vibeos_vfs_runtime_t *rt, uint32_t mount_id, uint32_t key, void *buf, size_t buf_len, size_t *out_len);
int vibeos_vfs_persist_delete(vibeos_vfs_runtime_t *rt, uint32_t mount_id, uint32_t key);
int vibeos_vfs_persist_count(const vibeos_vfs_runtime_t *rt, uint32_t mount_id, uint32_t *out_count);

#endif
