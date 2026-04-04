#ifndef VIBEOS_FS_H
#define VIBEOS_FS_H

#include <stdint.h>

#include "vibeos/policy.h"
#include "vibeos/security_model.h"

#define VIBEOS_MAX_MOUNTS 16u
#define VIBEOS_MAX_OPEN_FILES 64u

typedef struct vibeos_mount_entry {
    uint32_t id;
    uint32_t active;
} vibeos_mount_entry_t;

typedef struct vibeos_file_entry {
    uint32_t fd;
    uint32_t in_use;
} vibeos_file_entry_t;

typedef struct vibeos_vfs_runtime {
    vibeos_mount_entry_t mounts[VIBEOS_MAX_MOUNTS];
    vibeos_file_entry_t files[VIBEOS_MAX_OPEN_FILES];
    uint32_t next_mount_id;
    uint32_t next_fd;
} vibeos_vfs_runtime_t;

int vibeos_vfs_runtime_init(vibeos_vfs_runtime_t *rt);
int vibeos_vfs_mount(vibeos_vfs_runtime_t *rt, uint32_t *out_mount_id);
int vibeos_vfs_unmount(vibeos_vfs_runtime_t *rt, uint32_t mount_id);
int vibeos_vfs_active_mounts(const vibeos_vfs_runtime_t *rt, uint32_t *out_count);
int vibeos_vfs_open(vibeos_vfs_runtime_t *rt, uint32_t mount_id, uint32_t *out_fd);
int vibeos_vfs_open_secure(vibeos_vfs_runtime_t *rt, uint32_t mount_id, const vibeos_policy_state_t *policy, const vibeos_security_token_t *token, uint32_t *out_fd);
int vibeos_vfs_close(vibeos_vfs_runtime_t *rt, uint32_t fd);

#endif
