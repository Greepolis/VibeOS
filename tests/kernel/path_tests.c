/* Host tests for paths (kernel/fs/path.c, docs/abi/ A4 and L1) and for the
 * filesystem interface's wrappers (kernel/fs/vfs.c, L1). Each check names the
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
    const char *const *links;     /* pairs: path, target */
} tfs_t;

static int tfs_has(const char *const *list, const char *p) {
    for (; *list; list++) {
        if (strcmp(*list, p) == 0) {
            return 1;
        }
    }
    return 0;
}

static const char *tfs_link(const tfs_t *t, const char *p) {
    const char *const *l = t->links;
    for (; l && *l; l += 2) {
        if (strcmp(l[0], p) == 0) {
            return l[1];
        }
    }
    return 0;
}

static int tfs_lookup(void *fs, const char *path, vibeos_fs_node_t *out) {
    const tfs_t *t = (const tfs_t *)fs;
    const char *target;
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
    if ((target = tfs_link(t, path)) != 0) {
        out->mode = VIBEOS_S_IFLNK | 0777u;
        out->size = strlen(target);
        return 0;
    }
    return -1;
}

static long tfs_readlink(void *fs, const char *path, char *buf, uint32_t cap) {
    const char *target;
    size_t n;
    while (*path == '/') {
        path++;
    }
    if ((target = tfs_link((const tfs_t *)fs, path)) == 0) {
        return -VIBEOS_EINVAL;
    }
    n = strlen(target);
    if (n > cap) {
        n = cap;
    }
    memcpy(buf, target, n);
    return (long)n;
}

/* A filesystem must read to be mounted (vibeos_fs_mount refuses one that
 * cannot); these are never read from. */
static long tfs_read_at(void *fs, const vibeos_fs_node_t *node, uint64_t off,
                        void *buf, uint32_t len) {
    (void)fs; (void)node; (void)off; (void)buf; (void)len;
    return 0;
}

static const vibeos_fs_ops_t g_tfs_ops = {
    .lookup = tfs_lookup,
    .read_at = tfs_read_at,
    .readlink = tfs_readlink,
};

/* The same filesystem with a whole-file writer and nothing else: FAT's shape. */
static long tfs_write_file(void *fs, const char *path, const void *buf, uint32_t len) {
    (void)fs; (void)path; (void)buf;
    return (long)len;
}

static const vibeos_fs_ops_t g_wfs_ops = {
    .lookup = tfs_lookup,
    .read_at = tfs_read_at,
    .write_file = tfs_write_file,
};

/* One walk, and what it should have found: the resolved path, or an error. */
static void walk(const char *base, const char *path, uint32_t flags, int want_r,
                 const char *want_path, const char *what) {
    vibeos_path_t w;
    int r = vibeos_path_walk("/", base, path, flags, &w);
    if (r != want_r || (r == 0 && want_path && strcmp(w.path, want_path) != 0)) {
        printf("FAIL:path walk(%s, %s) = %d '%s', want %d '%s': %s\n", base, path, r,
               r == 0 ? w.path : "-", want_r, want_path ? want_path : "-", what);
        g_fail = 1;
    }
}

static void t_lock(void) {}
static void t_unlock(void) {}

int test_path(void) {
    static const char *const a_dirs[] = { "bin", "etc", "usr", "usr/share", 0 };
    static const char *const a_files[] = { "bin/sh", "etc/motd", "usr/share/doc", 0 };
    static const char *const a_links[] = {
        "etc/rel", "motd",               /* relative, beside it          */
        "etc/abs", "/bin/sh",            /* absolute                     */
        "sd", "usr/share",               /* a directory                  */
        "chain", "etc/rel",              /* a link to a link             */
        "loop1", "loop2", "loop2", "loop1",
        "dangling", "nowhere",
        "tolib", "/mnt/lib",             /* across a mount               */
        "abcd", "usr",                   /* a target shorter than the name, with
                                          * path after it: the rest of the path
                                          * moves down, overlapping itself */
        0
    };
    static const char *const b_dirs[] = { "lib", 0 };
    static const char *const b_files[] = { "lib/x.so", 0 };
    static tfs_t a = { a_dirs, a_files, a_links }, b = { b_dirs, b_files, 0 };
    static vibeos_fsmount_t ma, mb;
    vibeos_path_t w;
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

    expect(vibeos_path_walk("/", "/", "/etc/motd", 0, &w) == 0 && w.mnt == &ma &&
           strcmp(w.tail, "etc/motd") == 0 && !w.node.is_dir && w.exists, "a file on the root mount");
    expect(vibeos_path_walk("/", "/", "/mnt/lib/x.so", 0, &w) == 0 && w.mnt == &mb &&
           strcmp(w.tail, "lib/x.so") == 0,
           "a path under /mnt is the second filesystem's - every path used to reach the boot volume");
    expect(vibeos_path_walk("/", "/", "/mnt", 0, &w) == 0 && w.mnt == &mb && w.node.is_dir,
           "the mount point is the mounted filesystem's root");
    walk("/", "/etc/motd/x", 0, -VIBEOS_ENOTDIR, 0, "a file used as a directory is ENOTDIR");
    walk("/", "/nope/x", 0, -VIBEOS_ENOENT, 0, "a missing directory is ENOENT");
    walk("/", "/mnt/lib/none", 0, -VIBEOS_ENOENT, 0, "a missing file under a mount is ENOENT");
    expect(vibeos_path_walk("/", "/", "/etc/new", VIBEOS_PATH_CREATE, &w) == 0 && !w.exists &&
           strcmp(w.tail, "etc/new") == 0, "a new name's parent must exist, the name need not");
    walk("/", "/bin/sh/new", VIBEOS_PATH_CREATE, -VIBEOS_ENOTDIR, 0, "a new name under a file is ENOTDIR");
    walk("/", "/nope/new", VIBEOS_PATH_CREATE, -VIBEOS_ENOENT, 0, "a new name in a missing directory is ENOENT");
    expect(vibeos_path_walk("/", "/", "/mnt", VIBEOS_PATH_CREATE, &w) == 0 && w.exists,
           "a mount point already exists");
    walk("/usr", "share/doc", 0, 0, "/usr/share/doc", "relative to the base");
    walk("/usr", "..", 0, 0, "/", "the base's parent");
    walk("/", "/etc/motd/", 0, -VIBEOS_ENOTDIR, 0, "a trailing slash asks for a directory");
    walk("/", "/usr/share/", 0, 0, "/usr/share", "which a directory is");

    /* ---- symbolic links (L1) ---- */
    walk("/", "/etc/rel", 0, 0, "/etc/motd", "a relative link resolves beside itself");
    walk("/", "/etc/abs", 0, 0, "/bin/sh", "an absolute link starts again from the root");
    walk("/", "/chain", 0, 0, "/etc/motd", "a link to a link is followed to the end");
    walk("/", "/sd/doc", 0, 0, "/usr/share/doc", "a link in the middle of a path is followed");
    walk("/", "/sd/..", 0, 0, "/usr",
         "'link/..' is the parent of where the link points, not the directory holding it");
    walk("/", "/tolib/x.so", 0, 0, "/mnt/lib/x.so", "a link can cross into another mount");
    walk("/", "/abcd/share/doc", 0, 0, "/usr/share/doc",
         "a target shorter than the link keeps the rest of the path intact");
    walk("/", "/loop1", 0, -VIBEOS_ELOOP, 0, "a cycle is ELOOP, not a hang");
    walk("/", "/dangling", 0, -VIBEOS_ENOENT, 0, "a dangling link followed is ENOENT");
    expect(vibeos_path_walk("/", "/", "/dangling", VIBEOS_PATH_NOFOLLOW, &w) == 0 &&
           (w.node.mode & VIBEOS_S_IFMT) == VIBEOS_S_IFLNK && strcmp(w.path, "/dangling") == 0,
           "NOFOLLOW answers with the link itself");
    expect(vibeos_path_walk("/", "/", "/sd/", VIBEOS_PATH_NOFOLLOW, &w) == 0 && w.node.is_dir,
           "a trailing slash follows the last link even under NOFOLLOW, as Linux does");
    expect(vibeos_path_walk("/", "/", "/sd", VIBEOS_PATH_NOFOLLOW, &w) == 0 &&
           (w.node.mode & VIBEOS_S_IFMT) == VIBEOS_S_IFLNK, "and without one does not");
    walk("/", "/etc/abs/x", 0, -VIBEOS_ENOTDIR, 0, "a link to a file used as a directory is ENOTDIR");
    expect(vibeos_path_walk("/", "/", "/sd/new", VIBEOS_PATH_CREATE, &w) == 0 && !w.exists &&
           strcmp(w.path, "/usr/share/new") == 0,
           "a name created through a link is created where the link points");

    /* ---- the wrappers (vfs.c, L1) ---- */
    {
        vibeos_fs_node_t n;
        vibeos_fs_statfs_t sf;
        static vibeos_fsmount_t mw;
        char lb[8];

        /* Five drivers predate every field but three. A node filled with
         * garbage before the call must come back with nothing of it left. */
        memset(&n, 0xAA, sizeof(n));
        expect(vibeos_fs_lookup(&ma, "etc/motd", &n) == 0 &&
               n.mode == (VIBEOS_S_IFREG | 0644u) && n.nlink == 1u && n.uid == 0u &&
               n.gid == 0u && n.atime_ns == 0u && n.mtime_ns == 0u && !n.is_dir,
               "a driver that fills three fields leaves no stack garbage in the rest");
        memset(&n, 0xAA, sizeof(n));
        expect(vibeos_fs_lookup(&ma, "etc", &n) == 0 && n.mode == (VIBEOS_S_IFDIR | 0755u) &&
               n.is_dir, "and a directory is a directory by its type too");
        memset(&n, 0, sizeof(n));
        expect(vibeos_fs_lookup(&ma, "sd", &n) == 0 && !n.is_dir &&
               (n.mode & VIBEOS_S_IFMT) == VIBEOS_S_IFLNK, "a type the driver gave is kept");

        /* A read-only filesystem says EROFS; one that writes but lacks the
         * operation says EPERM, or EOPNOTSUPP where the caller has a fallback. */
        expect(vibeos_fs_rmdir(&ma, "etc") == -VIBEOS_EROFS, "rmdir on a read-only filesystem is EROFS");
        expect(vibeos_fs_write_at(&ma, &n, 0, "x", 1) == -VIBEOS_EROFS,
               "writing to a read-only filesystem is EROFS");
        (void)vibeos_fs_mount(&mw, &g_wfs_ops, &a, "w");
        expect(vibeos_fs_rmdir(&mw, "etc") == -VIBEOS_EPERM,
               "rmdir on a filesystem that writes but has no rmdir is EPERM");
        expect(vibeos_fs_write_at(&mw, &n, 0, "x", 1) == -VIBEOS_EOPNOTSUPP,
               "a writable filesystem without in-place writes says so, for the caller's fallback");
        expect(vibeos_fs_readlink(&mw, "etc", lb, sizeof(lb)) == -VIBEOS_EINVAL,
               "readlink on a filesystem without links: nothing is one");
        expect(vibeos_fs_sync(&ma) == 0, "sync with nothing held back succeeds");
        expect(vibeos_fs_statfs(&ma, &sf) == 0 && sf.read_only && sf.name_max == 255u,
               "statfs from a driver without one still says read-only");
        expect(vibeos_fs_statfs(&mw, &sf) == 0 && !sf.read_only, "and writable");
    }

    vibeos_fs_detach_all();
    return g_fail ? -1 : 0;
}
