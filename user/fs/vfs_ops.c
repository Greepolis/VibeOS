#include "vibeos/fs.h"

static int vfs_mount_is_active(const vibeos_vfs_runtime_t *rt, uint32_t mount_id) {
    uint32_t i;
    if (!rt || mount_id == 0) {
        return 0;
    }
    for (i = 0; i < VIBEOS_MAX_MOUNTS; i++) {
        if (rt->mounts[i].active && rt->mounts[i].id == mount_id) {
            return 1;
        }
    }
    return 0;
}

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
    for (i = 0; i < VIBEOS_MAX_PERSIST_FILES; i++) {
        rt->persist[i].mount_id = 0;
        rt->persist[i].key = 0;
        rt->persist[i].in_use = 0;
        rt->persist[i].size = 0;
    }
    rt->persistent_files = 0;
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

int vibeos_vfs_unmount(vibeos_vfs_runtime_t *rt, uint32_t mount_id) {
    uint32_t i;
    if (!rt || mount_id == 0) {
        return -1;
    }
    for (i = 0; i < VIBEOS_MAX_MOUNTS; i++) {
        if (rt->mounts[i].active && rt->mounts[i].id == mount_id) {
            uint32_t p;
            rt->mounts[i].active = 0;
            rt->mounts[i].id = 0;
            for (p = 0; p < VIBEOS_MAX_PERSIST_FILES; p++) {
                if (rt->persist[p].in_use && rt->persist[p].mount_id == mount_id) {
                    rt->persist[p].in_use = 0;
                    rt->persist[p].mount_id = 0;
                    rt->persist[p].key = 0;
                    rt->persist[p].size = 0;
                    if (rt->persistent_files > 0) {
                        rt->persistent_files--;
                    }
                }
            }
            return 0;
        }
    }
    return -1;
}

int vibeos_vfs_active_mounts(const vibeos_vfs_runtime_t *rt, uint32_t *out_count) {
    uint32_t i;
    uint32_t count = 0;
    if (!rt || !out_count) {
        return -1;
    }
    for (i = 0; i < VIBEOS_MAX_MOUNTS; i++) {
        if (rt->mounts[i].active) {
            count++;
        }
    }
    *out_count = count;
    return 0;
}

int vibeos_vfs_open(vibeos_vfs_runtime_t *rt, uint32_t mount_id, uint32_t *out_fd) {
    uint32_t i;
    if (!rt || !out_fd || mount_id == 0) {
        return -1;
    }
    if (!vfs_mount_is_active(rt, mount_id)) {
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

int vibeos_vfs_persist_write(vibeos_vfs_runtime_t *rt, uint32_t mount_id, uint32_t key, const void *data, size_t len) {
    uint32_t i;
    uint32_t free_slot = VIBEOS_MAX_PERSIST_FILES;
    const uint8_t *src = (const uint8_t *)data;
    if (!rt || !data || key == 0 || len == 0 || len > VIBEOS_PERSIST_FILE_MAX_BYTES) {
        return -1;
    }
    if (!vfs_mount_is_active(rt, mount_id)) {
        return -1;
    }
    for (i = 0; i < VIBEOS_MAX_PERSIST_FILES; i++) {
        if (rt->persist[i].in_use && rt->persist[i].mount_id == mount_id && rt->persist[i].key == key) {
            uint32_t j;
            for (j = 0; j < (uint32_t)len; j++) {
                rt->persist[i].data[j] = src[j];
            }
            rt->persist[i].size = (uint32_t)len;
            return 0;
        }
        if (!rt->persist[i].in_use && free_slot == VIBEOS_MAX_PERSIST_FILES) {
            free_slot = i;
        }
    }
    if (free_slot == VIBEOS_MAX_PERSIST_FILES) {
        return -1;
    }
    rt->persist[free_slot].in_use = 1;
    rt->persist[free_slot].mount_id = mount_id;
    rt->persist[free_slot].key = key;
    rt->persist[free_slot].size = (uint32_t)len;
    for (i = 0; i < (uint32_t)len; i++) {
        rt->persist[free_slot].data[i] = src[i];
    }
    rt->persistent_files++;
    return 0;
}

int vibeos_vfs_persist_read(vibeos_vfs_runtime_t *rt, uint32_t mount_id, uint32_t key, void *buf, size_t buf_len, size_t *out_len) {
    uint32_t i;
    uint8_t *dst = (uint8_t *)buf;
    if (!rt || !buf || !out_len || key == 0 || buf_len == 0) {
        return -1;
    }
    if (!vfs_mount_is_active(rt, mount_id)) {
        return -1;
    }
    for (i = 0; i < VIBEOS_MAX_PERSIST_FILES; i++) {
        uint32_t j;
        if (!rt->persist[i].in_use || rt->persist[i].mount_id != mount_id || rt->persist[i].key != key) {
            continue;
        }
        if (buf_len < rt->persist[i].size) {
            return -1;
        }
        for (j = 0; j < rt->persist[i].size; j++) {
            dst[j] = rt->persist[i].data[j];
        }
        *out_len = rt->persist[i].size;
        return 0;
    }
    return -1;
}

int vibeos_vfs_persist_delete(vibeos_vfs_runtime_t *rt, uint32_t mount_id, uint32_t key) {
    uint32_t i;
    if (!rt || key == 0) {
        return -1;
    }
    if (!vfs_mount_is_active(rt, mount_id)) {
        return -1;
    }
    for (i = 0; i < VIBEOS_MAX_PERSIST_FILES; i++) {
        if (rt->persist[i].in_use && rt->persist[i].mount_id == mount_id && rt->persist[i].key == key) {
            rt->persist[i].in_use = 0;
            rt->persist[i].mount_id = 0;
            rt->persist[i].key = 0;
            rt->persist[i].size = 0;
            if (rt->persistent_files > 0) {
                rt->persistent_files--;
            }
            return 0;
        }
    }
    return -1;
}

int vibeos_vfs_persist_count(const vibeos_vfs_runtime_t *rt, uint32_t mount_id, uint32_t *out_count) {
    uint32_t i;
    uint32_t count = 0;
    if (!rt || !out_count) {
        return -1;
    }
    for (i = 0; i < VIBEOS_MAX_PERSIST_FILES; i++) {
        if (!rt->persist[i].in_use) {
            continue;
        }
        if (mount_id == 0 || rt->persist[i].mount_id == mount_id) {
            count++;
        }
    }
    *out_count = count;
    return 0;
}
