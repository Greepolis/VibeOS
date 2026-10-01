/* Host tests for tmpfs (kernel/fs/tmpfs.c, docs/abi/ L1 step 2), through the
 * filesystem interface's wrappers as the kernel calls it. Each check names the
 * defect it stands for.
 *
 * The page allocator and the lock are the test's, and both watch the rules
 * tmpfs promises: every page it takes comes back (destroy leaves the count at
 * zero), it never takes its own lock twice, and it never allocates while
 * holding it - the allocator may reclaim, and the lock masks interrupts. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vibeos/tmpfs.h"
#include "vibeos/path.h"
#include "vibeos/abi_linux.h"
#include "vibeos/mbz.h"

int test_tmpfs(void);

static int g_fail;
static int g_pages_out;
static int g_lock_depth;
static int g_alloc_limit = -1;     /* -1: no limit */
static uint64_t g_clock;

static void expect(int cond, const char *what) {
    if (!cond) {
        printf("FAIL:tmpfs %s\n", what);
        g_fail = 1;
    }
}

static void *t_page(void) {
    void *p;
    if (g_lock_depth != 0) {
        printf("FAIL:tmpfs allocated a page while holding its own lock\n");
        g_fail = 1;
    }
    if (g_alloc_limit == 0) {
        return 0;
    }
    if (g_alloc_limit > 0) {
        g_alloc_limit--;
    }
    p = malloc(VIBEOS_TMPFS_PAGE);
    if (p) {
        memset(p, 0x5A, VIBEOS_TMPFS_PAGE);   /* what a freed page looks like */
        g_pages_out++;
    }
    return p;
}

static void t_page_free(void *p) {
    g_pages_out--;
    free(p);
}

static void t_lock(void) {
    if (g_lock_depth != 0) {
        printf("FAIL:tmpfs took its lock twice\n");
        g_fail = 1;
    }
    g_lock_depth++;
}

static void t_unlock(void) {
    g_lock_depth--;
}

static uint64_t t_now(void) {
    return g_clock;
}

static vibeos_tmpfs_t g_t;
static vibeos_fsmount_t g_m;

static void fresh(uint64_t pages) {
    vibeos_tmpfs_destroy(&g_t);
    expect(vibeos_tmpfs_init(&g_t, pages, t_page, t_page_free, t_lock, t_unlock) == 0, "init");
    expect(vibeos_fs_mount(&g_m, vibeos_tmpfs_ops(), &g_t, "tmpfs") == 0, "mount");
}

static int node(const char *path, vibeos_fs_node_t *n) {
    return vibeos_fs_lookup(&g_m, path, n);
}

static long wr(const char *path, uint64_t off, const void *buf, uint32_t len) {
    vibeos_fs_node_t n;
    if (node(path, &n) != 0) {
        return -999;
    }
    return vibeos_fs_write_at(&g_m, &n, off, buf, len);
}

static long rd(const char *path, uint64_t off, void *buf, uint32_t len) {
    vibeos_fs_node_t n;
    if (node(path, &n) != 0) {
        return -999;
    }
    return vibeos_fs_read_at(&g_m, &n, off, buf, len);
}

static int mk(const char *path) {
    vibeos_fs_node_t n;
    return vibeos_fs_create(&g_m, path, 0644u, &n);
}

/* Is `name` among the entries of `dir`? */
static int listed(const char *dir, const char *name) {
    char buf[VIBEOS_NAME_MAX + 1u];
    uint32_t i;
    for (i = 0; vibeos_fs_list(&g_m, dir, i, buf, sizeof(buf), 0, 0) == 0; i++) {
        if (strcmp(buf, name) == 0) {
            return 1;
        }
    }
    return 0;
}

int test_tmpfs(void) {
    static uint8_t big[3 * VIBEOS_TMPFS_PAGE], back[3 * VIBEOS_TMPFS_PAGE];
    vibeos_fs_node_t n, n2;
    vibeos_fs_statfs_t sf;
    vibeos_fs_attr_t a;
    char lb[64];
    uint32_t i;
    long r;

    g_fail = 0;
    g_pages_out = 0;
    g_lock_depth = 0;
    g_alloc_limit = -1;
    g_clock = 1000;
    vibeos_fs_set_clock(t_now);
    memset(&g_t, 0, sizeof(g_t));
    g_t.lock = t_lock;
    g_t.unlock = t_unlock;
    fresh(4096);

    /* ---- files: offsets, holes, sizes ---- */
    expect(node("/", &n) == 0 && n.is_dir && (n.mode & 07777u) == 01777u,
           "the root is a directory, mode 1777 as /tmp is");
    expect(mk("f") == 0 && node("f", &n) == 0 && n.mode == (VIBEOS_S_IFREG | 0644u) &&
           n.size == 0u && n.nlink == 1u, "create makes an empty file with the mode asked");
    expect(mk("f") == -VIBEOS_EEXIST, "create on a taken name is EEXIST");
    for (i = 0; i < sizeof(big); i++) {
        big[i] = (uint8_t)(i * 7u + 3u);
    }
    expect(wr("f", 100, big, sizeof(big)) == (long)sizeof(big),
           "a write across three page boundaries is whole");
    memset(back, 0xEE, sizeof(back));
    expect(rd("f", 100, back, sizeof(back)) == (long)sizeof(back) &&
           memcmp(back, big, sizeof(big)) == 0, "and reads back as written");
    expect(rd("f", 0, back, 100) == 100 && back[0] == 0 && back[99] == 0,
           "the bytes before the first write read as zero");
    expect(node("f", &n) == 0 && n.size == 100u + sizeof(big), "the size is where the write ended");
    expect(rd("f", n.size, back, 10) == 0, "reading at the end is end of file");

    /* A sparse write far out: past the direct and single-indirect pages, into
     * the double-indirect ones. */
    {
        uint64_t far = (uint64_t)(VIBEOS_TMPFS_DIRECT + 512u + 700u) * VIBEOS_TMPFS_PAGE + 5u;
        uint8_t x = 0x42;
        int before = g_pages_out;
        expect(wr("f", far, &x, 1) == 1, "a write in the double-indirect range");
        expect(g_pages_out - before == 3, "costs one data page and two pointer pages, not the gap");
        expect(rd("f", far - 1u, back, 2) == 2 && back[0] == 0 && back[1] == 0x42,
               "the hole before it reads zero and the byte is there");
        expect(node("f", &n) == 0 && n.size == far + 1u, "and the size reaches it");
    }

    /* truncate: shrinking drops pages and zeroes the cut tail. */
    {
        int before;
        expect(node("f", &n) == 0 && vibeos_fs_truncate(&g_m, &n, 150) == 0 &&
               node("f", &n2) == 0 && n2.size == 150u, "truncate shrinks");
        before = g_pages_out;
        expect(node("f", &n) == 0 && vibeos_fs_truncate(&g_m, &n, 5000) == 0,
               "and grows without writing");
        expect(g_pages_out == before, "growing allocates nothing");
        expect(rd("f", 100, back, 100) == 100 && memcmp(back, big, 50) == 0 && back[50] == 0 &&
               back[99] == 0, "bytes past the old cut read zero, not what was cut off");
    }

    /* ---- directories ---- */
    expect(vibeos_fs_mkdir(&g_m, "d") == 0 && node("d", &n) == 0 && n.is_dir && n.nlink == 2u,
           "a new directory has two links");
    expect(node("/", &n) == 0 && n.nlink == 3u, "and its parent gains one");
    expect(vibeos_fs_mkdir(&g_m, "d") == -VIBEOS_EEXIST, "mkdir on a taken name is EEXIST");
    expect(mk("d/g") == 0 && listed("d", "g") && listed("d", ".") && listed("d", ".."),
           "a listing has the name, '.' and '..'");
    expect(vibeos_fs_rmdir(&g_m, "d") == -VIBEOS_ENOTEMPTY, "rmdir of a non-empty directory");
    expect(vibeos_fs_rmdir(&g_m, "f") == -VIBEOS_ENOTDIR, "rmdir of a file is ENOTDIR");
    expect(vibeos_fs_unlink(&g_m, "d") == -VIBEOS_EISDIR, "unlink of a directory is EISDIR");
    expect(mk("f/x") == -VIBEOS_ENOTDIR, "a name under a file is ENOTDIR");
    expect(mk("nope/x") == -VIBEOS_ENOENT, "a name under a missing directory is ENOENT");
    expect(node("d/g/..", &n) != 0, "'..' through a file is refused");
    expect(node("d/..", &n) == 0 && n.is_dir && n.nlink == 3u, "'..' is the parent");

    /* ---- names: unlink, link, stale ids ---- */
    expect(vibeos_fs_link(&g_m, "f", "hard") == 0 && node("f", &n) == 0 && n.nlink == 2u,
           "a hard link counts");
    expect(vibeos_fs_link(&g_m, "d", "dl") == -VIBEOS_EPERM, "a hard link to a directory is EPERM");
    expect(vibeos_fs_unlink(&g_m, "f") == 0 && rd("hard", 100, back, 10) == 10 &&
           memcmp(back, big, 10) == 0, "unlinking one name keeps the data for the other");
    expect(node("hard", &n) == 0 && n.nlink == 1u, "one link left");
    expect(vibeos_fs_unlink(&g_m, "hard") == 0, "the last name goes");
    expect(vibeos_fs_read_at(&g_m, &n, 0, back, 1) == -VIBEOS_ENOENT,
           "a description holding a freed file gets ENOENT, not another file's bytes");
    expect(mk("reuse") == 0 && vibeos_fs_read_at(&g_m, &n, 0, back, 1) == -VIBEOS_ENOENT,
           "even after its inode is reused");

    /* ---- symbolic links ---- */
    expect(vibeos_fs_symlink(&g_m, "../somewhere/else", "sl") == 0 && node("sl", &n) == 0 &&
           (n.mode & VIBEOS_S_IFMT) == VIBEOS_S_IFLNK && n.size == 17u,
           "a symbolic link is a link, its size the target's length");
    r = vibeos_fs_readlink(&g_m, "sl", lb, sizeof(lb));
    expect(r == 17 && memcmp(lb, "../somewhere/else", 17) == 0, "readlink gives the target unresolved");
    expect(vibeos_fs_readlink(&g_m, "reuse", lb, sizeof(lb)) == -VIBEOS_EINVAL,
           "readlink of a file is EINVAL");
    expect(vibeos_fs_symlink(&g_m, "x", "sl") == -VIBEOS_EEXIST, "a link on a taken name");

    /* ---- rename ---- */
    expect(mk("a") == 0 && wr("a", 0, "AAAA", 4) == 4 && mk("b") == 0 && wr("b", 0, "BB", 2) == 2,
           "two files");
    {
        int before = g_pages_out;
        expect(vibeos_fs_rename(&g_m, "a", "b", 0) == 0, "rename over an existing file");
        expect(g_pages_out == before - 1, "the replaced file's page is given back");
    }
    expect(node("a", &n) != 0 && rd("b", 0, back, 4) == 4 && memcmp(back, "AAAA", 4) == 0,
           "the old name is gone and the new one has the moved contents");
    /* The arrangement the second lookup exists for: target and source in one
     * directory, the source's record last. Removing the target's record moves
     * the last one - the source's - into its place, so the slot remembered for
     * the source now names the end of the directory. The first rename above
     * had the target last, which moves nothing, and a sabotage of this walked
     * straight through it - and so did a second version with the target just
     * before the source, where the stale slot is exactly the new end and the
     * wrong removal happens to remove the right record. A record between the
     * two is what makes the stale slot name somebody else. */
    expect(vibeos_fs_mkdir(&g_m, "r") == 0 && mk("r/tgt") == 0 && mk("r/keep") == 0 &&
           mk("r/src") == 0 && wr("r/src", 0, "SRC", 3) == 3, "a directory of three");
    expect(vibeos_fs_rename(&g_m, "r/src", "r/tgt", 0) == 0 && node("r/src", &n) != 0 &&
           rd("r/tgt", 0, back, 3) == 3 && memcmp(back, "SRC", 3) == 0 && node("r/keep", &n) == 0,
           "rename over a name when the source is the directory's last record");
    expect(mk("c") == 0 && vibeos_fs_rename(&g_m, "b", "c", VIBEOS_RENAME_NOREPLACE) == -VIBEOS_EEXIST,
           "NOREPLACE refuses a taken name");
    expect(vibeos_fs_link(&g_m, "c", "c2") == 0 && vibeos_fs_rename(&g_m, "c", "c2", 0) == 0 &&
           node("c", &n) == 0 && node("c2", &n) == 0, "renaming onto another name of the same file does nothing");
    expect(vibeos_fs_rename(&g_m, "c", "c2", VIBEOS_RENAME_NOREPLACE) == -VIBEOS_EEXIST,
           "unless NOREPLACE, which Linux checks first");
    expect(vibeos_fs_mkdir(&g_m, "e") == 0 && vibeos_fs_mkdir(&g_m, "e/sub") == 0 &&
           vibeos_fs_rename(&g_m, "e", "e/sub/x", 0) == -VIBEOS_EINVAL,
           "a directory cannot move inside itself");
    expect(vibeos_fs_rename(&g_m, "d", "e", 0) == -VIBEOS_ENOTEMPTY,
           "a directory over a non-empty one");
    expect(vibeos_fs_rename(&g_m, "c", "e", 0) == -VIBEOS_EISDIR, "a file over a directory");
    expect(vibeos_fs_rename(&g_m, "e", "c", 0) == -VIBEOS_ENOTDIR, "a directory over a file");
    expect(vibeos_fs_mkdir(&g_m, "empty") == 0 && vibeos_fs_rename(&g_m, "d", "empty", 0) == 0 &&
           node("empty/g", &n) == 0 && node("d", &n) != 0, "a directory over an empty one");
    expect(vibeos_fs_rename(&g_m, "empty", "e/sub/moved", 0) == 0 &&
           node("e/sub/moved/..", &n) == 0 && node("e/sub", &n2) == 0 && n.id == n2.id,
           "a moved directory's '..' is its new parent");
    expect(node("e/sub", &n) == 0 && n.nlink == 3u && node("/", &n2) == 0,
           "and the link counts moved with it");

    /* ---- attributes, times, statfs ---- */
    g_clock = 5000;
    memset(&a, 0, sizeof(a));
    a.valid = VIBEOS_ATTR_MODE | VIBEOS_ATTR_UID | VIBEOS_ATTR_MTIME;
    a.mode = 0100600u;   /* a type in the mode must not change the type */
    a.uid = 42;
    a.mtime_ns = 77;
    expect(vibeos_fs_setattr(&g_m, "c", &a) == 0 && node("c", &n) == 0 &&
           n.mode == (VIBEOS_S_IFREG | 0600u) && n.uid == 42u && n.mtime_ns == 77u &&
           n.ctime_ns == 5000u, "setattr changes what it names, keeps the type, and moves ctime");
    g_clock = 6000;
    expect(wr("c", 0, "z", 1) == 1 && node("c", &n) == 0 && n.mtime_ns == 6000u,
           "a write moves mtime");
    expect(vibeos_fs_statfs(&g_m, &sf) == 0 && sf.magic == VIBEOS_TMPFS_MAGIC &&
           sf.blocks == 4096u && sf.blocks_free == 4096u - (uint64_t)g_t.pages_used && !sf.read_only,
           "statfs reports TMPFS_MAGIC and what is left");

    /* ---- a record that names a free inode ---- */
    {
        uint64_t before = vibeos_mbz_count(VIBEOS_MBZ_TMPFS_BAD_RECORD);
        uint32_t victim;
        expect(mk("corrupt") == 0 && node("corrupt", &n) == 0, "a file to corrupt");
        /* Free the inode behind tmpfs's back, as a rename that forgot a
         * record would leave it: the name still points at the slot. */
        victim = (uint32_t)(n.id & 0xFFFFFFFFu) - 1u;
        g_t.inode[victim].mode = 0;
        g_t.inodes_used--;
        expect(node("corrupt", &n) == -VIBEOS_EIO,
               "a record naming a free inode is refused, not followed");
        expect(vibeos_mbz_count(VIBEOS_MBZ_TMPFS_BAD_RECORD) == before + 1u,
               "and counted as tmpfs_bad_record");
    }

    /* ---- running out ---- */
    fresh(3);
    expect(mk("x") == 0, "a file");
    r = wr("x", 0, big, sizeof(big));
    expect(r == 2 * VIBEOS_TMPFS_PAGE, "a write that runs out of pages keeps what fitted");
    expect(wr("x", sizeof(big), big, 10) == -VIBEOS_ENOSPC, "and the next is ENOSPC");
    fresh(64);
    g_alloc_limit = 0;
    expect(mk("y") == -VIBEOS_ENOSPC, "no page for a directory record is ENOSPC");
    expect(node("y", &n) != 0, "and leaves no name behind");
    g_alloc_limit = -1;
    /* Enough pages that only the inode table can run out: every directory
     * record takes room too, and a small filesystem would run out of that
     * first and prove nothing about inodes. */
    fresh(4096);
    for (i = 0; i < VIBEOS_TMPFS_INODES; i++) {
        char nm[16];
        snprintf(nm, sizeof(nm), "n%u", i);
        if (mk(nm) != 0) {
            break;
        }
    }
    expect(i == VIBEOS_TMPFS_INODES - 1u, "every inode but the root can be used, and then ENOSPC");

    vibeos_tmpfs_destroy(&g_t);
    expect(g_pages_out == 0, "every page taken is given back");
    expect(g_lock_depth == 0, "the lock is released on every path");
    vibeos_fs_set_clock(0);
    return g_fail ? -1 : 0;
}
