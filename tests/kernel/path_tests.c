/* Host tests for paths (kernel/fs/path.c, docs/abi/ A4). Each check names the
 * defect it stands for. */

#include <stdio.h>
#include <string.h>

#include "vibeos/path.h"
#include "vibeos/abi_linux.h"

int test_path(void);

static int g_fail;

static void expect(int cond, const char *what) {
    if (!cond) {
        printf("FAIL:path %s\n", what);
        g_fail = 1;
    }
}

/* ---- normalization ------------------------------------------------------------------ */

static void norm(const char *root, const char *cwd, const char *path, const char *want) {
    char out[VIBEOS_PATH_MAX];
    int r = vibeos_path_normalize(root, cwd, path, out, sizeof(out));
    if (r != 0 || strcmp(out, want) != 0) {
        printf("FAIL:path normalize(root=%s, cwd=%s, %s) = %d '%s', want '%s'\n",
               root, cwd, path, r, r == 0 ? out : "-", want);
        g_fail = 1;
    }
}

/* ---- two filesystems, each a list of paths, told apart by the fs pointer ----------- */

typedef struct {
    const char *const *dirs;
    const char *const *files;
} tfs_t;

static int tfs_has(const char *const *list, const char *p) {
    for (; *list; list++) {
        if (strcmp(*list, p) == 0) {
            return 1;
        }
    }
    return 0;
}

static int tfs_lookup(void *fs, const char *path, vibeos_fs_node_t *out) {
    const tfs_t *t = (const tfs_t *)fs;
    while (*path == '/') {
        path++;
    }
    out->id = 7;
    out->size = 0;
    if (*path == 0 || tfs_has(t->dirs, path)) {
        out->is_dir = 1;
        return 0;
    }
    if (tfs_has(t->files, path)) {
        out->is_dir = 0;
        return 0;
    }
    return -1;
}

/* A filesystem must read to be mounted (vibeos_fs_mount refuses one that
 * cannot); these are never read from. */
static long tfs_read_at(void *fs, const vibeos_fs_node_t *node, uint64_t off,
                        void *buf, uint32_t len) {
    (void)fs; (void)node; (void)off; (void)buf; (void)len;
    return 0;
}

static const vibeos_fs_ops_t g_tfs_ops = { tfs_lookup, tfs_read_at, 0, 0, 0, 0 };

static void t_lock(void) {}
static void t_unlock(void) {}

int test_path(void) {
    static const char *const a_dirs[] = { "bin", "etc", 0 };
    static const char *const a_files[] = { "bin/sh", "etc/motd", 0 };
    static const char *const b_dirs[] = { "lib", 0 };
    static const char *const b_files[] = { "lib/x.so", 0 };
    static tfs_t a = { a_dirs, a_files }, b = { b_dirs, b_files };
    static vibeos_fsmount_t ma, mb;
    vibeos_fsmount_t *m;
    vibeos_fs_node_t node;
    const char *tail;
    char out[VIBEOS_PATH_MAX], small[8];

    g_fail = 0;

    norm("/", "/", "/a/b", "/a/b");
    norm("/", "/usr", "bin", "/usr/bin");
    norm("/", "/usr", "./bin/", "/usr/bin");
    norm("/", "/usr/lib", "../bin", "/usr/bin");
    norm("/", "/", "..", "/");
    norm("/", "/", "../../x", "/x");
    norm("/", "/a", "//b///c", "/b/c");
    norm("/", "/a/b", ".", "/a/b");
    norm("/", "/a", "b/../../..", "/");
    /* A root other than "/": ".." stops at it, and an absolute path is inside it. */
    norm("/jail", "/jail/home", "../../..", "/jail");
    norm("/jail", "/jail/home", "/etc", "/jail/etc");
    norm("/jail", "/elsewhere", "x", "/jail/x");

    expect(vibeos_path_normalize("/", "/", "", out, sizeof(out)) == -VIBEOS_ENOENT,
           "an empty path is ENOENT");
    expect(vibeos_path_normalize("/", "/", "abcdefgh", small, sizeof(small)) == -VIBEOS_ENAMETOOLONG,
           "a result longer than the buffer is ENAMETOOLONG, not cut short");
    {
        /* One character over NAME_MAX, into a buffer with room to spare, so the
         * only thing that can refuse it is the component limit. The first
         * version used a buffer PATH_MAX long, which the component overflowed
         * too - and removing the NAME_MAX check left it green. */
        char comp[VIBEOS_NAME_MAX + 2u];
        static char roomy[4u * VIBEOS_PATH_MAX];
        memset(comp, 'n', sizeof(comp) - 1u);
        comp[sizeof(comp) - 1u] = 0;
        expect(vibeos_path_normalize("/", "/", comp, roomy, sizeof(roomy)) == -VIBEOS_ENAMETOOLONG,
               "a component over NAME_MAX is ENAMETOOLONG");
        comp[sizeof(comp) - 2u] = 0;
        expect(vibeos_path_normalize("/", "/", comp, roomy, sizeof(roomy)) == 0,
               "a component of exactly NAME_MAX is not");
    }

    /* ---- the walk, through the mount table ---- */
    vibeos_fs_set_lock(t_lock, t_unlock);
    vibeos_fs_detach_all();
    vibeos_fs_unmount(&ma);
    vibeos_fs_unmount(&mb);
    (void)vibeos_fs_mount(&ma, &g_tfs_ops, &a, "a");
    (void)vibeos_fs_mount(&mb, &g_tfs_ops, &b, "b");
    expect(vibeos_fs_is_mounted(&ma) && vibeos_fs_is_mounted(&mb) &&
           vibeos_fs_attach("/", &ma) == 0 && vibeos_fs_attach("/mnt", &mb) == 0, "two mounts");

    expect(vibeos_path_lookup("/etc/motd", &m, &tail, &node) == 0 && m == &ma &&
           strcmp(tail, "etc/motd") == 0 && !node.is_dir, "a file on the root mount");
    expect(vibeos_path_lookup("/mnt/lib/x.so", &m, &tail, &node) == 0 && m == &mb &&
           strcmp(tail, "lib/x.so") == 0,
           "a path under /mnt is the second filesystem's - every path used to reach the boot volume");
    expect(vibeos_path_lookup("/mnt", &m, &tail, &node) == 0 && m == &mb && node.is_dir,
           "the mount point is the mounted filesystem's root");
    expect(vibeos_path_lookup("/etc/motd/x", &m, &tail, &node) == -VIBEOS_ENOTDIR,
           "a file used as a directory is ENOTDIR");
    expect(vibeos_path_lookup("/nope/x", &m, &tail, &node) == -VIBEOS_ENOENT,
           "a missing directory is ENOENT");
    expect(vibeos_path_lookup("/mnt/lib/none", &m, &tail, &node) == -VIBEOS_ENOENT,
           "a missing file under a mount is ENOENT");
    expect(vibeos_path_parent("/etc/new", &m, &tail) == 0 && strcmp(tail, "etc/new") == 0,
           "a new name's parent must exist, the name need not");
    expect(vibeos_path_parent("/bin/sh/new", &m, &tail) == -VIBEOS_ENOTDIR,
           "a new name under a file is ENOTDIR");
    expect(vibeos_path_parent("/mnt", &m, &tail) == -VIBEOS_EEXIST,
           "a mount point already exists");
    vibeos_fs_detach_all();
    return g_fail ? -1 : 0;
}
