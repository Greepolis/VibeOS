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

/* ---- the mount table (I4b step 4) ----------------------------------------
 *
 * See include/vibeos/vfs.h for why this is a fixed array, why resolution is
 * longest-prefix, and why a second claim on one path is refused rather than
 * allowed to overwrite.
 */

typedef struct {
    char path[VIBEOS_FS_MOUNT_PATH_MAX];
    uint32_t path_len;
    vibeos_fsmount_t *mnt;
} fs_attach_t;

static fs_attach_t g_mounts[VIBEOS_FS_MOUNTS_MAX];
static uint32_t g_mount_count;

static uint32_t fs_strlen(const char *s) {
    uint32_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

static int fs_same(const char *a, const char *b, uint32_t n) {
    uint32_t i;
    for (i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

int vibeos_fs_attach(const char *path, vibeos_fsmount_t *mnt) {
    uint32_t len, i;

    if (!path || !mnt || path[0] != '/') {
        return -1;
    }
    len = fs_strlen(path);
    if (len == 0u || len >= VIBEOS_FS_MOUNT_PATH_MAX) {
        return -1;
    }
    /* A trailing slash on anything but the root would make "/usr/" and "/usr"
     * two different mounts of the same directory, and the resolver below would
     * then answer differently for the same path depending on which was
     * registered. One spelling, checked here. */
    if (len > 1u && path[len - 1u] == '/') {
        return -1;
    }
    for (i = 0; i < g_mount_count; i++) {
        if (g_mounts[i].path_len == len && fs_same(g_mounts[i].path, path, len)) {
            return -1;   /* taken; see the header on why not overwritten */
        }
    }
    if (g_mount_count >= VIBEOS_FS_MOUNTS_MAX) {
        return -1;
    }
    for (i = 0; i < len; i++) {
        g_mounts[g_mount_count].path[i] = path[i];
    }
    g_mounts[g_mount_count].path[len] = 0;
    g_mounts[g_mount_count].path_len = len;
    g_mounts[g_mount_count].mnt = mnt;
    g_mount_count++;
    return 0;
}

int vibeos_fs_detach(const char *path) {
    uint32_t len, i;

    if (!path) {
        return -1;
    }
    len = fs_strlen(path);
    for (i = 0; i < g_mount_count; i++) {
        if (g_mounts[i].path_len == len && fs_same(g_mounts[i].path, path, len)) {
            /* The last entry moves into the hole. Order carries no meaning
             * here precisely because resolution is longest-prefix rather than
             * first-match, which is what makes that safe. */
            g_mounts[i] = g_mounts[g_mount_count - 1u];
            g_mount_count--;
            return 0;
        }
    }
    return -1;
}

int vibeos_fs_resolve(const char *path, vibeos_fsmount_t **out_mnt,
                      const char **out_tail) {
    uint32_t i;
    int best = -1;
    uint32_t best_len = 0;

    if (!path || !out_mnt || !out_tail || path[0] != '/') {
        return -1;
    }
    for (i = 0; i < g_mount_count; i++) {
        uint32_t n = g_mounts[i].path_len;
        if (!fs_same(g_mounts[i].path, path, n)) {
            continue;
        }
        /* The prefix has to end on a boundary. Without this "/usrlocal" would
         * match a mount at "/usr", which is a wrong answer that looks entirely
         * reasonable in a log. The root is the exception: it ends on '/'
         * already. */
        if (n > 1u && path[n] != 0 && path[n] != '/') {
            continue;
        }
        if (best < 0 || n > best_len) {
            best = (int)i;
            best_len = n;
        }
    }
    if (best < 0) {
        return -1;
    }
    *out_mnt = g_mounts[best].mnt;
    /* What is left, without the mount's own prefix and without a leading
     * slash: a driver is handed a path relative to its own root, which is what
     * lets one driver be mounted in two places. */
    {
        const char *tail = path + best_len;
        while (*tail == '/') {
            tail++;
        }
        *out_tail = tail;
    }
    return 0;
}

uint32_t vibeos_fs_mount_count(void) {
    return g_mount_count;
}

const char *vibeos_fs_mount_path(uint32_t index) {
    return (index < g_mount_count) ? g_mounts[index].path : 0;
}

vibeos_fsmount_t *vibeos_fs_mount_at(uint32_t index) {
    return (index < g_mount_count) ? g_mounts[index].mnt : 0;
}

void vibeos_fs_detach_all(void) {
    g_mount_count = 0;
}
