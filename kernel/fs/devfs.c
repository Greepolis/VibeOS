/* /dev. See include/vibeos/devfs.h. */

#include "vibeos/devfs.h"
#include "vibeos/abi_linux.h"

typedef struct {
    const char *name;
    uint32_t mode;
    uint32_t rdev;
    const char *link;     /* a symbolic link's target, or 0 */
} devfs_entry_t;

/* Linux's permissions: anybody reads and writes the bit buckets and the random
 * devices, /dev/tty is everybody's own terminal, /dev/console is root's. */
static const devfs_entry_t g_dev[] = {
    {"null", VIBEOS_S_IFCHR | 0666u, VIBEOS_DEV_NULL, 0},
    {"zero", VIBEOS_S_IFCHR | 0666u, VIBEOS_DEV_ZERO, 0},
    {"full", VIBEOS_S_IFCHR | 0666u, VIBEOS_DEV_FULL, 0},
    {"random", VIBEOS_S_IFCHR | 0666u, VIBEOS_DEV_RANDOM, 0},
    {"urandom", VIBEOS_S_IFCHR | 0666u, VIBEOS_DEV_URANDOM, 0},
    {"tty", VIBEOS_S_IFCHR | 0666u, VIBEOS_DEV_TTY, 0},
    {"console", VIBEOS_S_IFCHR | 0600u, VIBEOS_DEV_CONSOLE, 0},
    {"fd", VIBEOS_S_IFLNK | 0777u, 0, "/proc/self/fd"},
    {"stdin", VIBEOS_S_IFLNK | 0777u, 0, "/proc/self/fd/0"},
    {"stdout", VIBEOS_S_IFLNK | 0777u, 0, "/proc/self/fd/1"},
    {"stderr", VIBEOS_S_IFLNK | 0777u, 0, "/proc/self/fd/2"},
};
#define DEVFS_ENTRIES ((uint32_t)(sizeof(g_dev) / sizeof(g_dev[0])))

static int devfs_same(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == 0 && *b == 0;
}

static uint32_t devfs_len(const char *s) {
    uint32_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

/* The entry `path` names, or -1 for the directory itself, or -2 for nothing. */
static int devfs_find(const char *path) {
    uint32_t i;

    while (*path == '/') {
        path++;
    }
    if (*path == 0) {
        return -1;
    }
    for (i = 0; i < DEVFS_ENTRIES; i++) {
        if (devfs_same(path, g_dev[i].name)) {
            return (int)i;
        }
    }
    return -2;
}

/* Node ids: the directory is 1, an entry 2 + its place in the table. */
static int devfs_lookup(void *fs, const char *path, vibeos_fs_node_t *out) {
    int i = devfs_find(path);

    (void)fs;
    if (i == -2) {
        return -VIBEOS_ENOENT;
    }
    out->nlink = 1u;
    if (i < 0) {
        out->id = 1u;
        out->is_dir = 1;
        out->mode = VIBEOS_S_IFDIR | 0755u;
        out->nlink = 2u;
        return 0;
    }
    out->id = 2u + (uint32_t)i;
    out->is_dir = 0;
    out->mode = g_dev[i].mode;
    out->rdev = g_dev[i].rdev;
    out->size = g_dev[i].link ? devfs_len(g_dev[i].link) : 0u;
    return 0;
}

/* Never reached for a device - opening one is the device's - and a link is
 * read with readlink. Asked anyway, a device has no contents to give. */
static long devfs_read_at(void *fs, const vibeos_fs_node_t *node, uint64_t off, void *buf, uint32_t len) {
    (void)fs;
    (void)off;
    (void)buf;
    (void)len;
    return node->is_dir ? -VIBEOS_EISDIR : -VIBEOS_ENODEV;
}

static int devfs_list(void *fs, const char *path, uint32_t index, char *name, uint32_t cap,
                      uint64_t *out_size, int *out_is_dir) {
    uint32_t k;

    (void)fs;
    if (devfs_find(path) != -1) {
        return -VIBEOS_ENOTDIR;
    }
    if (index >= DEVFS_ENTRIES || cap == 0u) {
        return -1;
    }
    for (k = 0; k + 1u < cap && g_dev[index].name[k]; k++) {
        name[k] = g_dev[index].name[k];
    }
    name[k] = 0;
    if (out_size) {
        *out_size = 0;
    }
    if (out_is_dir) {
        *out_is_dir = 0;
    }
    return 0;
}

static long devfs_readlink(void *fs, const char *path, char *buf, uint32_t cap) {
    int i = devfs_find(path);
    uint32_t k;

    (void)fs;
    if (i < 0) {
        return i == -1 ? -VIBEOS_EINVAL : -VIBEOS_ENOENT;
    }
    if (!g_dev[i].link) {
        return -VIBEOS_EINVAL;
    }
    for (k = 0; k < cap && g_dev[i].link[k]; k++) {
        buf[k] = g_dev[i].link[k];
    }
    return (long)k;
}

static int devfs_statfs(void *fs, vibeos_fs_statfs_t *out) {
    (void)fs;
    out->magic = 0x01021994u;   /* TMPFS_MAGIC, which Linux's devtmpfs reports */
    out->block_size = 4096u;
    out->name_max = 255u;
    out->read_only = 1;
    return 0;
}

/* No lock: nothing here changes. The table is constant and every answer is
 * computed from it. */
static const vibeos_fs_ops_t g_devfs_ops = {
    .lookup = devfs_lookup,
    .read_at = devfs_read_at,
    .list = devfs_list,
    .readlink = devfs_readlink,
    .statfs = devfs_statfs,
};

const vibeos_fs_ops_t *vibeos_devfs_ops(void) {
    return &g_devfs_ops;
}
