#include "vibeos/fs.h"

int vibeos_vfs_runtime_init(vibeos_vfs_runtime_t *rt) {
    uint32_t i;
    if (!rt) {
        return -1;
    }
    rt->next_mount_id = 1;
    rt->next_fd = 3;
    for (i = 0; i < VIBEOS_MAX_MOUNTS; i++) {
        rt->mounts[i].id = 0;
        rt->mounts[i].active = 0;
    }
    for (i = 0; i < VIBEOS_MAX_OPEN_FILES; i++) {
        rt->files[i].fd = 0;
        rt->files[i].in_use = 0;
    }
    return 0;
}

int vibeos_vfs_mount(vibeos_vfs_runtime_t *rt, uint32_t *out_mount_id) {
    uint32_t i;
    if (!rt || !out_mount_id) {
        return -1;
    }
    for (i = 0; i < VIBEOS_MAX_MOUNTS; i++) {
        if (!rt->mounts[i].active) {
            rt->mounts[i].active = 1;
            rt->mounts[i].id = rt->next_mount_id++;
            *out_mount_id = rt->mounts[i].id;
            return 0;
        }
    }
    return -1;
}

int vibeos_vfs_open(vibeos_vfs_runtime_t *rt, uint32_t mount_id, uint32_t *out_fd) {
    uint32_t i;
    uint32_t mount_ok = 0;
    if (!rt || !out_fd || mount_id == 0) {
        return -1;
    }
    for (i = 0; i < VIBEOS_MAX_MOUNTS; i++) {
        if (rt->mounts[i].active && rt->mounts[i].id == mount_id) {
            mount_ok = 1;
            break;
        }
    }
    if (!mount_ok) {
        return -1;
    }
    for (i = 0; i < VIBEOS_MAX_OPEN_FILES; i++) {
        if (!rt->files[i].in_use) {
            rt->files[i].in_use = 1;
            rt->files[i].fd = rt->next_fd++;
            *out_fd = rt->files[i].fd;
            return 0;
        }
    }
    return -1;
}

int vibeos_vfs_open_secure(vibeos_vfs_runtime_t *rt, uint32_t mount_id, const vibeos_policy_state_t *policy, const vibeos_security_token_t *token, uint32_t *out_fd) {
    if (!policy || !token) {
        return -1;
    }
    if (vibeos_policy_can_fs_open(policy, token->capability_mask) != VIBEOS_POLICY_ALLOW) {
        return -1;
    }
    return vibeos_vfs_open(rt, mount_id, out_fd);
}

int vibeos_vfs_close(vibeos_vfs_runtime_t *rt, uint32_t fd) {
    uint32_t i;
    if (!rt || fd == 0) {
        return -1;
    }
    for (i = 0; i < VIBEOS_MAX_OPEN_FILES; i++) {
        if (rt->files[i].in_use && rt->files[i].fd == fd) {
            rt->files[i].in_use = 0;
            rt->files[i].fd = 0;
            return 0;
        }
    }
    return -1;
}
