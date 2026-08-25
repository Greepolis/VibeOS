/* Filesystem dispatch: mount a driver, call it through function pointers.
 *
 * Deliberately thin. Everything hard belongs to the drivers; this file exists
 * so the syscall layer stops naming one of them. Portable, so the refusal
 * cases - an unmounted volume, a driver that does not implement an operation -
 * can be tested without a disk, and those are the cases a boot never shows
 * because a booted system always has its volume.
 */

#include "vibeos/vfs.h"

int vibeos_fs_mount(vibeos_fsmount_t *mnt, const vibeos_fs_ops_t *ops,
                     void *fs, const char *type) {
    if (!mnt || !ops || !ops->lookup || !ops->read_at) {
        /* lookup and read_at are the minimum: a filesystem that cannot resolve
         * a name or read a byte is not one. Everything else may be absent, and
         * the wrappers below report that as a refusal rather than a crash. */
        return -1;
    }
    mnt->ops = ops;
    mnt->fs = fs;
    mnt->type = type;
    mnt->mounted = 1;
    return 0;
}

void vibeos_fs_unmount(vibeos_fsmount_t *mnt) {
    if (mnt) {
        mnt->mounted = 0;
        mnt->ops = 0;
        mnt->fs = 0;
    }
}

int vibeos_fs_is_mounted(const vibeos_fsmount_t *mnt) {
    return (mnt && mnt->mounted && mnt->ops) ? 1 : 0;
}

const char *vibeos_fs_type(const vibeos_fsmount_t *mnt) {
    return (mnt && mnt->mounted && mnt->type) ? mnt->type : "none";
}

int vibeos_fs_lookup(vibeos_fsmount_t *mnt, const char *path,
                      vibeos_fs_node_t *out) {
    if (!vibeos_fs_is_mounted(mnt) || !path || !out) {
        return -1;
    }
    return mnt->ops->lookup(mnt->fs, path, out);
}

long vibeos_fs_read_at(vibeos_fsmount_t *mnt, const vibeos_fs_node_t *node,
                        uint64_t offset, void *buf, uint32_t len) {
    if (!vibeos_fs_is_mounted(mnt) || !node || !buf) {
        return -1;
    }
    return mnt->ops->read_at(mnt->fs, node, offset, buf, len);
}

long vibeos_fs_write_file(vibeos_fsmount_t *mnt, const char *path,
                           const void *buf, uint32_t len) {
    if (!vibeos_fs_is_mounted(mnt) || !path || !buf) {
        return -1;
    }
    if (!mnt->ops->write_file) {
        return -1;   /* a read-only filesystem, and saying so is the answer */
    }
    return mnt->ops->write_file(mnt->fs, path, buf, len);
}

int vibeos_fs_list(vibeos_fsmount_t *mnt, const char *path, uint32_t index,
                    char *name, uint32_t name_cap, uint64_t *out_size,
                    int *out_is_dir) {
    if (!vibeos_fs_is_mounted(mnt) || !path || !name || name_cap == 0u) {
        return -1;
    }
    if (!mnt->ops->list) {
        return -1;
    }
    return mnt->ops->list(mnt->fs, path, index, name, name_cap, out_size, out_is_dir);
}

int vibeos_fs_unlink(vibeos_fsmount_t *mnt, const char *path) {
    if (!vibeos_fs_is_mounted(mnt) || !path || !mnt->ops->unlink) {
        return -1;
    }
    return mnt->ops->unlink(mnt->fs, path);
}

int vibeos_fs_mkdir(vibeos_fsmount_t *mnt, const char *path) {
    if (!vibeos_fs_is_mounted(mnt) || !path || !mnt->ops->mkdir) {
        return -1;
    }
    return mnt->ops->mkdir(mnt->fs, path);
}

long vibeos_fs_read_file(vibeos_fsmount_t *mnt, const char *path,
                          void *buf, uint32_t cap) {
    vibeos_fs_node_t node;
    uint64_t done = 0;

    if (!vibeos_fs_is_mounted(mnt) || !path || !buf) {
        return -1;
    }
    if (mnt->ops->lookup(mnt->fs, path, &node) != 0) {
        return -1;
    }
    if (node.is_dir || node.size > (uint64_t)cap) {
        return -1;
    }
    while (done < node.size) {
        uint64_t want = node.size - done;
        long got;
        if (want > 0xFFFFFFFFull) {
            want = 0xFFFFFFFFull;
        }
        got = mnt->ops->read_at(mnt->fs, &node, done, (uint8_t *)buf + done,
                                (uint32_t)want);
        if (got <= 0) {
            /* Short of the declared size is a failure, not a smaller file.
             * Returning what was copied here is precisely how a truncated
             * program came to be handed to execve as if it were whole. */
            return -1;
        }
        done += (uint64_t)got;
    }
    return (long)done;
}
