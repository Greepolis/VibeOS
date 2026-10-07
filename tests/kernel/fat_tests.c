/* Host tests for the FAT driver's write path (kernel/fs/fat.c, docs/abi/ L1 step
 * 4), through the filesystem interface's wrappers as the kernel calls it. Each
 * check names the defect it stands for.
 *
 * The volume is an image in memory, formatted by the driver's own formatter:
 * sixteen megabytes of FAT16 with one-sector clusters, so a directory holds
 * sixteen entries a cluster and grows early, and a file crosses clusters
 * almost at once. That fixture is this project's, which proves only that the
 * writer and the reader agree; scripts/dev/verify-fat-mtools.sh is where
 * somebody else's tool reads what this driver wrote. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vibeos/fat.h"
#include "vibeos/vfs.h"
#include "vibeos/path.h"
#include "vibeos/abi_linux.h"

int test_fat(void);

#define SECTORS 32768u
#define FIRST 2048u
#define SLOTS 64u

static int g_fail;
static uint8_t *g_img;
static uint64_t g_img_sectors;
static uint64_t g_clock;

static void expect(int cond, const char *what) {
    if (!cond) {
        printf("FAIL:fat %s\n", what);
        g_fail = 1;
    }
}

static int img_read(void *ctx, uint64_t lba, void *buf) {
    (void)ctx;
    if (lba >= g_img_sectors) {
        return -1;
    }
    memcpy(buf, g_img + lba * 512u, 512);
    return 0;
}

static int img_write(void *ctx, uint64_t lba, const void *buf) {
    (void)ctx;
    if (lba >= g_img_sectors) {
        return -1;
    }
    memcpy(g_img + lba * 512u, buf, 512);
    return 0;
}

static uint64_t t_now(void) {
    return g_clock;
}

static vibeos_blockdev_t g_dev;
static vibeos_blockcache_t g_bc;
static vibeos_block_slot_t g_slots[SLOTS];
static uint8_t g_slot_data[SLOTS][512];
static vibeos_fsmount_t g_m;

/* A fresh volume of `sectors`, formatted and mounted. */
static int fresh(uint32_t sectors) {
    const vibeos_fs_driver_t *drv = vibeos_fat_fs_driver();
    uint32_t i;

    vibeos_fat_forget_volumes();
    free(g_img);
    g_img_sectors = (uint64_t)FIRST + sectors;
    g_img = calloc(1, (size_t)g_img_sectors * 512u);
    memset(g_img, 0xE7, (size_t)g_img_sectors * 512u);   /* nothing may count on zeros */
    g_dev.read = img_read;
    g_dev.write = img_write;
    g_dev.flush = 0;
    g_dev.ctx = 0;
    g_dev.sectors = g_img_sectors;
    for (i = 0; i < SLOTS; i++) {
        memset(&g_slots[i], 0, sizeof(g_slots[i]));
        g_slots[i].data = g_slot_data[i];
    }
    if (vibeos_blockcache_init(&g_bc, &g_dev, g_slots, SLOTS) != 0 ||
        drv->format(&g_bc, FIRST, sectors) != 0 ||
        drv->mount(&g_m, &g_bc, FIRST, sectors, 0) != 0) {
        return -1;
    }
    return 0;
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

static uint64_t free_clusters(void) {
    vibeos_fs_statfs_t sf;
    return vibeos_fs_statfs(&g_m, &sf) == 0 ? sf.blocks_free : ~0ull;
}

/* How many times `name` is listed in `dir`, exactly as spelled. */
static int listed(const char *dir, const char *name) {
    char buf[VIBEOS_NAME_MAX + 1u];
    uint32_t i;
    int n = 0;
    for (i = 0; vibeos_fs_list(&g_m, dir, i, buf, sizeof(buf), 0, 0) == 0; i++) {
        if (strcmp(buf, name) == 0) {
            n++;
        }
    }
    return n;
}

int test_fat(void) {
    static uint8_t big[2000], back[2000];
    vibeos_fs_node_t n, n2;
    vibeos_fs_statfs_t sf;
    vibeos_fs_attr_t a;
    uint64_t f0;
    uint32_t i;
    long r;

    g_fail = 0;
    g_clock = 0;
    vibeos_fs_set_clock(t_now);
    expect(fresh(SECTORS) == 0, "a volume formatted and mounted");
    {
        /* A boot sector that claims more than its partition is refused: its
         * last sectors would be the next partition's, and the driver writes
         * (external review, 2026-10-07). */
        vibeos_fsmount_t m2;
        uint64_t before = vibeos_fat_out_of_volume();

        expect(vibeos_fat_fs_driver()->mount(&m2, &g_bc, FIRST, SECTORS - 8u, 0) != 0,
               "a volume whose boot sector claims more than its partition is not mounted");
        expect(vibeos_fat_out_of_volume() == before, "and nothing was asked for outside a volume");
    }
    for (i = 0; i < sizeof(big); i++) {
        big[i] = (uint8_t)(i * 11u + 7u);
    }

    /* ---- names ---- */
    expect(mk("notes.txt") == 0 && listed("/", "notes.txt") == 1,
           "a lower-case 8.3 name is listed in the case it was written, without a long name");
    expect(node("NOTES.TXT", &n) == 0 && node("Notes.Txt", &n2) == 0 && n.id == n2.id,
           "and found whatever the case, as FAT finds names");
    expect(mk("notes.txt") == -VIBEOS_EEXIST && mk("NOTES.TXT") == -VIBEOS_EEXIST,
           "create on a taken name is EEXIST, in any case");
    expect(mk("sedAbCdEf") == 0 && listed("/", "sedAbCdEf") == 1 && node("sedAbCdEf", &n) == 0,
           "a nine-character name is kept whole - it used to be cut to eight");
    expect(mk("sedAbCdEg") == 0 && node("sedAbCdEf", &n) == 0 && node("sedAbCdEg", &n2) == 0 &&
           n.id != n2.id, "two long names with one eight-character prefix are two files");
    expect(node("SEDABC~1", &n) == 0 && node("SEDABC~2", &n2) == 0 && n.id != n2.id,
           "and have two short aliases - one alias for both is two files under one 8.3 name");
    expect(mk("a name with spaces and.two.dots") == 0 &&
           listed("/", "a name with spaces and.two.dots") == 1, "a name FAT's short form cannot hold");
    expect(mk("bad:name") == -VIBEOS_EINVAL && mk("trailing.") == -VIBEOS_EINVAL,
           "a name FAT cannot store at all is EINVAL, not something else's name");
    expect(vibeos_fs_link(&g_m, "notes.txt", "hard") == -VIBEOS_EPERM &&
           vibeos_fs_symlink(&g_m, "notes.txt", "soft") == -VIBEOS_EPERM,
           "FAT has neither kind of link: EPERM");

    /* ---- identity: where the entry is ---- */
    expect(mk("e1") == 0 && mk("e2") == 0 && node("e1", &n) == 0 && node("e2", &n2) == 0 &&
           n.id != n2.id && n.id != 0u,
           "two empty files are two files - they both had identity zero, the cluster they lack");
    expect(wr("e2", 0, "second", 6) == 6 && node("e1", &n2) == 0 && n2.size == 0u &&
           rd("e2", 0, back, 6) == 6 && memcmp(back, "second", 6) == 0,
           "a write to one empty file is in that one");
    expect(node("e2", &n) == 0 && wr("e2", 6, "!", 1) == 1 &&
           vibeos_fs_read_at(&g_m, &n, 0, back, 16) == 7,
           "a node looked up before a write reads the file as it is after");

    /* ---- writing in place ---- */
    f0 = free_clusters();
    expect(mk("f") == 0 && wr("f", 100, big, sizeof(big)) == (long)sizeof(big),
           "a write across four clusters, starting inside the first");
    memset(back, 0xEE, sizeof(back));
    expect(rd("f", 100, back, sizeof(back)) == (long)sizeof(back) &&
           memcmp(back, big, sizeof(big)) == 0, "reads back as written");
    expect(rd("f", 0, back, 100) == 100 && back[0] == 0 && back[99] == 0,
           "the bytes before the first write are zeros, not what the cluster held");
    expect(node("f", &n) == 0 && n.size == 2100u && free_clusters() == f0 - 5u,
           "the size is where the write ended and the clusters are the five it needs");
    expect(wr("f", 600, "MID", 3) == 3 && rd("f", 598, back, 8) == 8 &&
           back[0] == big[498] && memcmp(back + 2, "MID", 3) == 0 && back[5] == big[503],
           "a write in the middle changes three bytes and no others");
    expect(node("f", &n) == 0 && n.size == 2100u, "and not the size");

    /* A hole is not a hole on FAT: the gap is written as zeros, over whatever
     * the clusters held. Fill, cut, and write past the cut. */
    expect(node("f", &n) == 0 && vibeos_fs_truncate(&g_m, &n, 10) == 0 &&
           free_clusters() == f0 - 1u, "truncate gives back the clusters past the new end");
    expect(wr("f", 1500, "far", 3) == 3 && rd("f", 0, back, 1503) == 1503 &&
           memcmp(back, big, 0) == 0 && back[9] == 0 && back[10] == 0 && back[511] == 0 &&
           back[512] == 0 && back[1499] == 0 && memcmp(back + 1500, "far", 3) == 0,
           "a write past the end zeroes the gap: the old bytes of the kept cluster too");
    expect(node("f", &n) == 0 && vibeos_fs_truncate(&g_m, &n, 3000) == 0 &&
           rd("f", 1503, back, 1497) == 1497 && back[0] == 0 && back[1496] == 0,
           "truncate growing a file exposes zeros");
    expect(node("f", &n) == 0 && vibeos_fs_truncate(&g_m, &n, 0) == 0 && free_clusters() == f0 &&
           node("f", &n2) == 0 && n2.size == 0u && n2.id == n.id,
           "truncate to nothing frees every cluster and keeps the file");
    expect(wr("f", 0, "again", 5) == 5 && rd("f", 0, back, 5) == 5 && memcmp(back, "again", 5) == 0,
           "and it can be written again");

    /* ---- directories that grow ---- */
    expect(vibeos_fs_mkdir(&g_m, "d") == 0 && node("d", &n) == 0 && n.is_dir, "a directory");
    for (i = 0; i < 40u; i++) {
        char nm[64];
        snprintf(nm, sizeof(nm), "d/a long file name number %u", i);
        if (mk(nm) != 0) {
            break;
        }
    }
    expect(i == 40u, "forty long names in a directory whose cluster holds sixteen entries");
    for (i = 0; i < 40u; i++) {
        char nm[64], only[64];
        snprintf(nm, sizeof(nm), "d/a long file name number %u", i);
        snprintf(only, sizeof(only), "a long file name number %u", i);
        if (node(nm, &n) != 0 || listed("d", only) != 1) {
            break;
        }
    }
    expect(i == 40u, "every one of them is found and listed once, past the first cluster");
    expect(vibeos_fs_rmdir(&g_m, "d") == -VIBEOS_ENOTEMPTY, "rmdir of a directory with names in it");

    /* ---- unlink ---- */
    expect(node("sedAbCdEf", &n) == 0 && vibeos_fs_unlink(&g_m, "sedAbCdEf") == 0 &&
           node("sedAbCdEf", &n2) != 0 && listed("/", "sedAbCdEf") == 0,
           "unlink removes the name");
    expect(vibeos_fs_read_at(&g_m, &n, 0, back, 1) == -VIBEOS_ENOENT,
           "and a node left holding it is refused");
    expect(node("sedAbCdEg", &n) == 0, "the other name with its prefix is untouched");
    expect(vibeos_fs_unlink(&g_m, "d") == -VIBEOS_EISDIR && vibeos_fs_unlink(&g_m, "nope") == -VIBEOS_ENOENT &&
           vibeos_fs_rmdir(&g_m, "f") == -VIBEOS_ENOTDIR, "the wrong call for the kind of thing");

    /* ---- rename ---- */
    f0 = free_clusters();
    expect(mk("src file.txt") == 0 && wr("src file.txt", 0, big, 1200) == 1200 &&
           mk("dst.txt") == 0 && wr("dst.txt", 0, big, 600) == 600, "two files");
    expect(vibeos_fs_rename(&g_m, "src file.txt", "dst.txt", VIBEOS_RENAME_NOREPLACE) == -VIBEOS_EEXIST,
           "NOREPLACE refuses a taken name");
    expect(vibeos_fs_rename(&g_m, "src file.txt", "dst.txt", 0) == 0 && node("src file.txt", &n) != 0 &&
           node("dst.txt", &n) == 0 && n.size == 1200u && rd("dst.txt", 0, back, 1200) == 1200 &&
           memcmp(back, big, 1200) == 0, "rename over a file: the new name has the moved contents");
    expect(free_clusters() == f0 - 3u && listed("/", "dst.txt") == 1,
           "the replaced file's clusters come back, and the name is there once");
    expect(vibeos_fs_rename(&g_m, "dst.txt", "A Much Longer Name.text", 0) == 0 &&
           listed("/", "A Much Longer Name.text") == 1 && listed("/", "dst.txt") == 0 &&
           rd("A Much Longer Name.text", 0, back, 10) == 10 && memcmp(back, big, 10) == 0,
           "rename from a short name to a long one");
    expect(vibeos_fs_mkdir(&g_m, "e") == 0 && vibeos_fs_mkdir(&g_m, "e/sub") == 0 &&
           vibeos_fs_rename(&g_m, "e", "e/sub/x", 0) == -VIBEOS_EINVAL,
           "a directory cannot move inside itself");
    expect(vibeos_fs_rename(&g_m, "e", "d", 0) == -VIBEOS_ENOTEMPTY, "a directory over a non-empty one");
    expect(vibeos_fs_rename(&g_m, "f", "e", 0) == -VIBEOS_EISDIR, "a file over a directory");
    expect(vibeos_fs_rename(&g_m, "e", "f", 0) == -VIBEOS_ENOTDIR, "a directory over a file");
    expect(vibeos_fs_mkdir(&g_m, "moved") == 0 && mk("moved/inside") == 0 &&
           vibeos_fs_rename(&g_m, "moved", "e/sub/moved", 0) == 0 && node("e/sub/moved/inside", &n) == 0 &&
           node("moved", &n) != 0, "a directory moves with what is in it");
    expect(mk("e/sub/beside") == 0 && node("e/sub/moved/../beside", &n) == 0,
           "and its '..' is its new parent");
    expect(vibeos_fs_unlink(&g_m, "e/sub/moved/inside") == 0 && vibeos_fs_rmdir(&g_m, "e/sub/moved") == 0 &&
           node("e/sub/moved", &n) != 0, "rmdir of an empty directory");

    /* ---- attributes, times, statfs ---- */
    expect(node("f", &n) == 0 && (n.mode & 0777u) == 0755u && node("e", &n) == 0 && (n.mode & 0777u) == 0755u,
           "a file is executable, like a directory: FAT has no bit to say otherwise, and Linux says 0755");
    memset(&a, 0, sizeof(a));
    a.valid = VIBEOS_ATTR_MODE | VIBEOS_ATTR_MTIME;
    a.mode = 0444u;
    a.mtime_ns = 951827696ull * 1000000000ull;   /* 2000-02-29 12:34:56 UTC */
    expect(vibeos_fs_setattr(&g_m, "f", &a) == 0 && node("f", &n) == 0 &&
           (n.mode & 0222u) == 0u && n.mtime_ns == a.mtime_ns,
           "setattr keeps the read-only bit and the modification time, a leap day included");
    a.valid = VIBEOS_ATTR_UID;
    a.uid = 5;
    expect(vibeos_fs_setattr(&g_m, "f", &a) == -VIBEOS_EPERM, "an owner FAT cannot record is EPERM");
    a.uid = 0;
    expect(vibeos_fs_setattr(&g_m, "f", &a) == 0, "root's, which every file already has, is accepted");
    g_clock = 1700000000ull * 1000000000ull;
    expect(wr("e1", 0, "t", 1) == 1 && node("e1", &n) == 0 &&
           n.mtime_ns / 1000000000ull / 2u == 1700000000ull / 2u, "a write stamps the time, to FAT's two seconds");
    g_clock = 0;
    expect(vibeos_fs_statfs(&g_m, &sf) == 0 && sf.magic == 0x4d44u && sf.block_size == 512u &&
           !sf.read_only && sf.blocks_free < sf.blocks, "statfs reports MSDOS_SUPER_MAGIC and what is left");

    /* ---- running out ---- */
    expect(fresh(4200) == 0, "a small volume");
    f0 = free_clusters();
    expect(mk("fill") == 0 && mk("pad") == 0 && wr("pad", 0, "p", 1) == 1, "a file, and one cluster used beside it");
    {
        uint64_t off = 0, before = 0;
        for (;;) {
            before = free_clusters();
            r = wr("fill", off, big, sizeof(big));
            if (r != (long)sizeof(big)) {
                break;
            }
            off += sizeof(big);
        }
        /* The arrangement has to leave clusters free when the short write
         * comes, or it cannot tell a writer that takes them from one that
         * stops early. Without the pad file the last full write used the
         * volume up exactly, and a sabotage of this went NOT RED. */
        expect(before > 0u && before < 4u, "the short write arrives with clusters still free");
        expect(r >= 0 && r < (long)sizeof(big) && free_clusters() == 0u,
               "the write that runs out keeps what fitted and leaves nothing free");
        expect(wr("fill", off + (uint64_t)r, big, 10) == -VIBEOS_ENOSPC, "and the next is ENOSPC");
        expect(node("fill", &n) == 0 && n.size == off + (uint64_t)r, "the size is what was written");
        expect(rd("fill", n.size - 5u, back, 10) == 5, "and all of it reads");
    }
    expect(mk("more") == 0 && wr("more", 0, "x", 1) == -VIBEOS_ENOSPC,
           "a name still fits in the root; its first byte does not");
    expect(vibeos_fs_unlink(&g_m, "fill") == 0 && free_clusters() == f0 - 1u,
           "unlinking the file gives back every cluster it took - the pad keeps its one");
    expect(wr("more", 0, "x", 1) == 1, "and there is room again");
    {
        /* An empty file written past all the room left: the writer takes the
         * free clusters for a short write, finds the start beyond them, and
         * says ENOSPC - and used to leave them taken by nothing (external
         * review, 2026-10-07). Clusters are one sector here. */
        uint64_t left = free_clusters();

        expect(mk("late") == 0 && left > 0u && left != ~0ull &&
               wr("late", (left + 3u) * 512u, "y", 1) == -VIBEOS_ENOSPC && free_clusters() == left,
               "a write that starts past the room left is ENOSPC, and every cluster it took is free again");
        expect(node("late", &n) == 0 && n.size == 0u, "and the file is as empty as it was");
        expect(vibeos_fs_unlink(&g_m, "late") == 0, "which goes");
    }
    /* A name that goes gives back every slot it took - the long-name entries
     * as well as the short one. The root cannot grow, so three hundred names
     * of three slots each, one at a time, only fit if each is wholly returned. */
    for (i = 0; i < 300u; i++) {
        if (mk("a name that takes three slots") != 0 ||
            vibeos_fs_unlink(&g_m, "a name that takes three slots") != 0) {
            break;
        }
    }
    expect(i == 300u, "unlink returns the long-name slots too: 300 names through a 512-slot root");
    /* The FAT16 root is a fixed region: it fills, and says so. */
    for (i = 0; i < 600u; i++) {
        char nm[16];
        snprintf(nm, sizeof(nm), "r%u", i);
        if ((r = mk(nm)) != 0) {
            break;
        }
    }
    expect(i > 400u && i < 512u && r == -VIBEOS_ENOSPC,
           "the root directory of a FAT16 volume holds 512 entries and then refuses");

    free(g_img);
    g_img = 0;
    vibeos_fat_forget_volumes();
    vibeos_fs_set_clock(0);
    return g_fail ? -1 : 0;
}
