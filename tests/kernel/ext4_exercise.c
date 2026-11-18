/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The ext4 driver, on a volume mkfs.ext4 made (docs/filesystems/ext4/design.md
 * steps 1 and 2).
 *
 *     vibeos_ext4_exercise walk                (in the directory that holds fs.img)
 *     vibeos_ext4_exercise apply <script>      (changes fs.img through the driver)
 *     vibeos_ext4_exercise refused-file <path> (a path on the volume)
 *     vibeos_ext4_exercise refused-mount
 *     vibeos_ext4_exercise powercut <n>       (the script, the device dead after write n)
 *     vibeos_ext4_exercise recover            (mount fs.img, which replays its journal)
 *     vibeos_ext4_exercise states             (the script, the tree listed after each line)
 *
 * The last three are step 3's power-cut sweep (scripts/dev/verify-ext4-powercut.sh):
 * a device that takes the first n sector writes and silently drops every one
 * after them is a machine that lost power after write n - nothing the driver
 * does afterwards reaches the disk, and it is not told. What is left must be
 * replayed into a volume e2fsck calls clean whose tree is the one before or
 * the one after the operation the cut fell in, never a mixture.
 *
 * scripts/dev/verify-ext4.sh builds a tree on the host, has mkfs.ext4 -d put
 * it on a volume, and holds what this prints against the tree itself: "walk"
 * lists every directory and file under the root, one line each - the path,
 * D or F, the size, and the crc32 of the bytes read through the driver - and
 * the script lists the host's tree the same way. Neither side wrote the
 * volume; mkfs.ext4 did.
 *
 * "apply" is step 2: it runs a script of changes (create, write at an offset,
 * truncate, mkdir, rename, link, symlink, chmod, ...) through the mount's
 * operations, flushes, and writes the volume back to fs.img. The script is made
 * by scripts/dev/ext4-tree.py, which applies the same changes to the host's
 * copy of the tree with the host's own calls; the volume is then checked by
 * e2fsck and read back by debugfs, neither of which is ours.
 *
 * The other two modes are the refusals, on a volume the script has damaged
 * with debugfs: a file whose inode checksum no longer matches must not be
 * read, and counts as a checksum failure; a volume that claims a feature this
 * driver does not read must not mount, and names the feature bits. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
/* Mapping a volume past a gigabyte (load): POSIX only. The Windows host job
 * builds this exerciser and never gives it a volume that size. */
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

#include "vibeos/abi_linux.h"
#include "vibeos/acl.h"
#include "vibeos/ext2.h"
#include "vibeos/ext4_journal.h"
#include "vibeos/storage.h"
#include "vibeos/vfs.h"

const vibeos_fs_driver_t *vibeos_ext4_fs_driver(void);

#define SLOTS 64u

static uint8_t *g_img;
static uint64_t g_sectors;
static vibeos_blockdev_t g_dev;
static vibeos_blockcache_t g_bc;
static vibeos_block_slot_t g_slots[SLOTS];
static uint8_t g_slot_data[SLOTS][512];
/* The state a driver is given is VIBEOS_FS_STATE_BYTES, as in the volume table. */
static union {
    vibeos_ext2_t fs;
    uint64_t raw[VIBEOS_FS_STATE_BYTES / 8u];
} g_state;
#define g_fs g_state.fs
static const vibeos_fs_ops_t *g_ops;
static vibeos_fsmount_t g_mnt;
static int g_writable_dev;
static uint64_t g_writes;   /* sector writes the device has been asked for */
static uint64_t g_cut;      /* after this many it takes no more; 0 is never */
static uint64_t g_last_lba = ~0ull;
static int g_dead;          /* the cut has happened */

static int img_read(void *ctx, uint64_t lba, void *buf) {
    (void)ctx;
    if (lba >= g_sectors) {
        return -1;
    }
    memcpy(buf, g_img + lba * 512u, 512);
    return 0;
}

static int img_write(void *ctx, uint64_t lba, const void *buf) {
    (void)ctx;
    if (lba >= g_sectors) {
        return -1;
    }
    /* The unit a disk writes whole is its physical sector, 4 KiB on anything
     * current. A cut half-way through one is not a state a real power failure
     * leaves, and with 512-byte atomicity the superblock's two sectors could
     * land one new and one old - which no journal, Linux's included, can
     * protect against. So a cut finishes the 4 KiB run the last write was in,
     * if the next writes continue it, and then the power is gone. */
    if (++g_writes > g_cut && g_cut != 0u) {
        if (g_dead || lba != g_last_lba + 1u || lba / 8u != g_last_lba / 8u) {
            g_dead = 1;
            return 0;   /* the write is lost, and nobody is told */
        }
    }
    g_last_lba = lba;
    memcpy(g_img + lba * 512u, buf, 512);
    return 0;
}

/* zlib's crc32, which the script's Python computes the same way. */
static uint32_t crc32_of(const uint8_t *p, uint64_t n, uint32_t crc) {
    crc = ~crc;
    while (n-- > 0u) {
        uint32_t k;

        crc ^= *p++;
        for (k = 0; k < 8u; k++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

/* A volume past a gigabyte is mapped, not read: E4-004's has more than 2^32
 * blocks, 8 TiB of sparse file, and is never in memory. Writes then land in
 * the file as they are made, so save_image has nothing to do - and a power cut
 * cannot be told from no cut, so such a volume is for apply and walk only. */
static int g_mapped;
/* Set by a line that by its own rule changes nothing - a punch that starts at
 * or past the end of the file - so the script does not expect it to commit. */
static int g_line_unchanged;
static int save_image(void);

static int load(const char *path) {
    FILE *f = fopen(path, "rb");
    long n;

    if (!f || fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) <= 0 || fseek(f, 0, SEEK_SET) != 0) {
        return -1;
    }
#ifndef _WIN32
    if ((uint64_t)n > (1ull << 30)) {
        int fd = open(path, g_writable_dev ? O_RDWR : O_RDONLY);
        void *m;

        fclose(f);
        if (fd < 0) {
            return -1;
        }
        m = mmap(NULL, (size_t)n, g_writable_dev ? PROT_READ | PROT_WRITE : PROT_READ, MAP_SHARED, fd, 0);
        close(fd);
        if (m == MAP_FAILED) {
            return -1;
        }
        g_img = (uint8_t *)m;
        g_mapped = 1;
    } else
#endif
    {
        g_img = (uint8_t *)malloc((size_t)n);
        if (!g_img || fread(g_img, 1, (size_t)n, f) != (size_t)n) {
            return -1;
        }
        fclose(f);
    }
    g_sectors = (uint64_t)n / 512u;
    for (uint32_t i = 0; i < SLOTS; i++) {
        g_slots[i].data = g_slot_data[i];
    }
    g_dev.read = img_read;
    /* A device with no write is a read-only one, and the driver's mount reads
     * that to give the volume the reading operations - the boot's ext4 image. */
    g_dev.write = g_writable_dev ? img_write : NULL;
    g_dev.sectors = g_sectors;
    return vibeos_blockcache_init(&g_bc, &g_dev, g_slots, SLOTS);
}

/* Mounted the way the storage scan mounts it: through the driver, which decides
 * between the reading operations and the writing ones. */
/* The open-file layer's part (vibeos_fs_set_open_query): one node held by a
 * description that the script opens with orphankeep and closes with
 * orphanclose (E4-019). */
static vibeos_fs_node_t g_held;
static int g_holding;

static int held_query(const void *fs, uint64_t id) {
    return fs == (const void *)&g_fs && g_holding && id == g_held.id;
}

static int mount_volume(void) {
    vibeos_fs_set_open_query(held_query);
    memset(&g_state, 0, sizeof(g_state));
    if (vibeos_ext4_fs_driver()->mount(&g_mnt, &g_bc, 0, g_sectors, &g_state) != 0) {
        return -1;
    }
    g_ops = g_mnt.ops;
    return 0;
}

/* A file's crc32, read in uneven pieces so that a read starting or ending
 * inside a block is exercised, not only whole blocks. -1 if a read failed. */
static long long file_crc(const char *path, uint64_t size) {
    static uint8_t buf[70000];
    vibeos_fs_node_t node;
    uint64_t at = 0;
    uint32_t crc = 0, piece = 4095u;

    if (g_ops->lookup(&g_fs, path, &node) != 0 || node.size != size) {
        return -1;
    }
    while (at < size) {
        long got = g_ops->read_at(&g_fs, &node, at, buf, piece);

        if (got <= 0) {
            return -1;
        }
        crc = crc32_of(buf, (uint64_t)got, crc);
        at += (uint64_t)got;
        piece = piece == 4095u ? 65537u : 4095u;
    }
    return crc;
}

/* `walk attrs`: each file and directory also carries its permission bits,
 * owner, group and mtime in seconds, for the torture to judge (E4-027). The
 * plain walk is held to a tree mkfs.ext4 -d made, whose owners are the host's. */
static int g_walk_attrs;

static int name_cmp(const void *x, const void *y) {
    return strcmp((const char *)x, (const char *)y);
}

/* Its extended attributes (E4-011), as the torture's listing has them: each
 * "user.name:length:crc32", sorted, joined by commas; "-" for none. */
static void xattrs(const vibeos_fs_node_t *node) {
    static char names[64][300];
    static uint8_t val[4096];
    static const char *const pfx[8] = {0, "user.", "system.posix_acl_access", "system.posix_acl_default",
                                       "trusted.", 0, "security.", 0};
    uint32_t k, n = 0, index, nlen;
    char nm[256];

    for (k = 0; n < 64u && vibeos_fs_listxattr(&g_mnt, node, k, &index, nm, sizeof(nm) - 1u, &nlen) == 0; k++) {
        long got;

        nm[nlen] = 0;
        got = vibeos_fs_getxattr(&g_mnt, node, index, nm, nlen, val, sizeof(val));
        snprintf(names[n++], sizeof(names[0]), "%s%s:%ld:%08x", index < 8u && pfx[index] ? pfx[index] : "?.", nm,
                 got, got >= 0 ? (unsigned)crc32_of(val, (uint64_t)got, 0) : 0u);
    }
    qsort(names, n, sizeof(names[0]), name_cmp);
    printf("\t");
    for (k = 0; k < n; k++) {
        printf("%s%s", k ? "," : "", names[k]);
    }
    printf("%s", n ? "" : "-");
}

static void attrs(const vibeos_fs_node_t *node) {
    if (g_walk_attrs) {
        printf("\t%o\t%u\t%u", (unsigned)(node->mode & 07777u), (unsigned)node->uid, (unsigned)node->gid);
        xattrs(node);
        printf("\t%llu", (unsigned long long)(node->mtime_ns / 1000000000ull));
    }
}

static int walk(const char *dir) {
    char name[256], path[1024];
    uint64_t size;
    int is_dir, bad = 0;

    for (uint32_t i = 0; g_ops->list(&g_fs, dir, i, name, sizeof(name), &size, &is_dir) == 0; i++) {
        vibeos_fs_node_t node = {0};

        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0 || strcmp(name, "lost+found") == 0) {
            continue;
        }
        snprintf(path, sizeof(path), "%s%s%s", dir, strcmp(dir, "/") == 0 ? "" : "/", name);
        if (g_ops->lookup(&g_fs, path, &node) == 0 && (node.mode & VIBEOS_S_IFMT) == VIBEOS_S_IFLNK) {
            char target[4200];
            long got = g_ops->readlink(&g_fs, path, target, sizeof(target) - 1u);

            if (got < 0) {
                fprintf(stderr, "FAIL:ext4_exercise cannot read the link %s\n", path);
                bad = 1;
                continue;
            }
            target[got] = 0;
            printf("%s\tL\t%s\n", path, target);
        } else if (is_dir) {
            printf("%s\tD", path);
            attrs(&node);
            printf("\n");
            bad |= walk(path);
        } else {
            long long crc = file_crc(path, size);

            if (crc < 0) {
                fprintf(stderr, "FAIL:ext4_exercise cannot read %s\n", path);
                bad = 1;
                continue;
            }
            printf("%s\tF\t%llu\t%08llx", path, (unsigned long long)size, crc);
            attrs(&node);
            printf("\n");
        }
    }
    return bad;
}

/* ---- apply: the script ---------------------------------------------------- */

/* A file's bytes are a function of a seed and the position, so a block that
 * lands in the wrong place or the wrong file reads as wrong bytes. The host
 * side (ext4-tree.py) computes the same function. */
static uint8_t pat(uint32_t seed, uint64_t p) {
    return (uint8_t)(seed * 31u + (uint32_t)(p >> 8) * ((seed << 1) | 1u) + (uint32_t)(p & 255u));
}

/* The clock timestamps come from: a second per call, from a fixed day. A
 * second per call also ages a transaction past group commit's five seconds
 * within a few operations (E4-016), so a measurement of commits sets
 * VIBEOS_EX_CLOCK_STEP_NS to something nearer a real machine's pace. */
static uint64_t g_now = 1700000000ull * 1000000000ull;
static uint64_t g_step = 1000000000ull;

static uint64_t clock_fn(void) {
    g_now += g_step;
    return g_now;
}

static int errno_of(const char *name) {
    static const struct { const char *name; int e; } t[] = {
        { "EPERM", VIBEOS_EPERM }, { "ENOENT", VIBEOS_ENOENT }, { "EEXIST", VIBEOS_EEXIST },
        { "ENOTDIR", VIBEOS_ENOTDIR }, { "EISDIR", VIBEOS_EISDIR }, { "EINVAL", VIBEOS_EINVAL },
        { "ENOTEMPTY", VIBEOS_ENOTEMPTY }, { "ENAMETOOLONG", VIBEOS_ENAMETOOLONG }, { "ENOSPC", VIBEOS_ENOSPC },
        { "EFBIG", VIBEOS_EFBIG }, { "ENODATA", VIBEOS_ENODATA },
        { "EACCES", VIBEOS_EACCES }, { "EOPNOTSUPP", VIBEOS_EOPNOTSUPP },
    };

    for (unsigned i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        if (strcmp(t[i].name, name) == 0) {
            return t[i].e;
        }
    }
    return 0;
}

static int write_pattern(const char *path, uint64_t off, uint64_t len, uint32_t seed) {
    static uint8_t buf[1u << 20];
    vibeos_fs_node_t node;
    uint64_t done = 0;

    if (vibeos_fs_lookup(&g_mnt, path, &node) != 0) {
        return -VIBEOS_ENOENT;
    }
    while (done < len) {
        uint64_t n = len - done < sizeof(buf) ? len - done : sizeof(buf);
        long r;

        /* Pieces of odd length too, so writes start and end inside blocks. */
        if (n > 70001u && ((done / sizeof(buf)) & 1u) != 0u) {
            n = 70001u;
        }
        for (uint64_t i = 0; i < n; i++) {
            buf[i] = pat(seed, off + done + i);
        }
        r = vibeos_fs_write_at(&g_mnt, &node, off + done, buf, (uint32_t)n);
        if (r < 0) {
            return (int)r;
        }
        if (r == 0) {
            return -VIBEOS_EIO;   /* no progress: a short write that never ends */
        }
        /* A short write is a write, as every program that loops on write()
         * takes it: the driver stops at the runs one call may take (E4-009). */
        done += (uint64_t)r;
    }
    return 0;
}

static int run(char **a, int n);

static int run_line(char *line) {
    char *a[10];
    int n = 0;
    char *p = line;

    a[n++] = p;
    for (; *p; p++) {
        if (*p == '\t') {
            *p = 0;
            if (n < 10) {
                a[n++] = p + 1;
            }
        }
    }
    return run(a, n);
}

static int run(char **a, int n) {
    vibeos_fs_node_t node;
    vibeos_fs_attr_t at;
    const char *c = a[0];

    if (strcmp(c, "fail") == 0 && n >= 3) {
        /* The next command must be refused with exactly this errno. */
        int want = errno_of(a[1]), r = run(a + 2, n - 2);

        if (want == 0) {
            fprintf(stderr, "FAIL:ext4_exercise script names an errno it does not know: %s\n", a[1]);
            return -1;
        }
        if (r != -want) {
            fprintf(stderr, "FAIL:ext4_exercise %s %s gave %d, wanted -%s\n", a[2], n > 3 ? a[3] : "", r, a[1]);
            return -1;
        }
        return 0;
    }
    if (strcmp(c, "refuse") == 0 && n >= 2) {
        /* The next command must be refused, with whatever it says. For unlink
         * and mkdir, whose operations predate L1 and answer 0 or -1 by
         * contract (vfs.h): the errno a program sees is decided by the Linux
         * handler before the filesystem is asked, so the filesystem's part is
         * only to refuse and change nothing - which the tree compared after it
         * checks. */
        int r = run(a + 1, n - 1);

        if (r >= 0) {
            fprintf(stderr, "FAIL:ext4_exercise %s %s was done, wanted a refusal\n", a[1], n > 2 ? a[2] : "");
            return -1;
        }
        return 0;
    }
    if (strcmp(c, "staleness") == 0 && n == 1) {
        /* A node held across its file's last unlink, once the inode number has
         * gone to a new file (E4-012): reading and writing through it must be
         * refused, and the new file must be untouched. The number is checked
         * to be reused, or the test proves nothing. */
        static const uint8_t mine[8] = "OLDFILE!", theirs[8] = "NEWFILE!";
        vibeos_fs_node_t held, now;
        uint8_t got[8];
        long r;

        if (vibeos_fs_create(&g_mnt, "/stale-a", 0644, &held) != 0 ||
            vibeos_fs_write_at(&g_mnt, &held, 0, mine, 8u) != 8 ||
            vibeos_fs_lookup(&g_mnt, "/stale-a", &held) != 0 ||
            vibeos_fs_unlink(&g_mnt, "/stale-a") != 0 ||
            vibeos_fs_create(&g_mnt, "/stale-b", 0644, &now) != 0 ||
            vibeos_fs_write_at(&g_mnt, &now, 0, theirs, 8u) != 8) {
            fprintf(stderr, "FAIL:ext4_exercise staleness setup\n");
            return -1;
        }
        if (now.id != held.id) {
            fprintf(stderr, "FAIL:ext4_exercise staleness: inode %llu not reused (got %llu)\n",
                    (unsigned long long)held.id, (unsigned long long)now.id);
            return -1;
        }
        r = vibeos_fs_read_at(&g_mnt, &held, 0, got, 8u);
        if (r >= 0) {
            fprintf(stderr, "FAIL:ext4_exercise a node of a removed file read %ld bytes of the file that took its inode\n", r);
            return -1;
        }
        r = vibeos_fs_write_at(&g_mnt, &held, 0, mine, 8u);
        if (r >= 0) {
            fprintf(stderr, "FAIL:ext4_exercise a node of a removed file wrote into the file that took its inode\n");
            return -1;
        }
        if (vibeos_fs_lookup(&g_mnt, "/stale-b", &now) != 0 || vibeos_fs_read_at(&g_mnt, &now, 0, got, 8u) != 8 ||
            memcmp(got, theirs, 8u) != 0) {
            fprintf(stderr, "FAIL:ext4_exercise the new file changed\n");
            return -1;
        }
        printf("staleness ok inode=%llu\n", (unsigned long long)held.id);
        return 0;
    }
    if (strcmp(c, "orphankeep") == 0 && n == 4) {
        /* A file written, held by a description, and its last name removed
         * (E4-019): the description must still read every byte, the name must
         * be gone, and the file must say it has no links. */
        static uint8_t buf[1u << 20], got[1u << 20];
        uint64_t len = strtoull(a[2], NULL, 10);
        uint32_t seed = (uint32_t)strtoul(a[3], NULL, 10);
        vibeos_fs_node_t now;

        if (len > sizeof(buf) || g_holding) {
            return -VIBEOS_EINVAL;
        }
        for (uint64_t i = 0; i < len; i++) {
            buf[i] = pat(seed, i);
        }
        if (vibeos_fs_create(&g_mnt, a[1], 0644, &g_held) != 0 ||
            vibeos_fs_write_at(&g_mnt, &g_held, 0, buf, (uint32_t)len) != (long)len ||
            vibeos_fs_lookup(&g_mnt, a[1], &g_held) != 0) {
            fprintf(stderr, "FAIL:ext4_exercise orphankeep setup\n");
            return -1;
        }
        g_holding = 1;
        if (vibeos_fs_unlink(&g_mnt, a[1]) != 0 || vibeos_fs_lookup(&g_mnt, a[1], &now) == 0) {
            fprintf(stderr, "FAIL:ext4_exercise orphankeep: the name was not removed\n");
            return -1;
        }
        if (vibeos_fs_read_at(&g_mnt, &g_held, 0, got, (uint32_t)len) != (long)len || memcmp(got, buf, len) != 0) {
            fprintf(stderr, "FAIL:ext4_exercise an open file lost its contents with its last name\n");
            return -1;
        }
        if (vibeos_fs_getattr(&g_mnt, &g_held, &now) != 0 || now.nlink != 0u || now.size != len) {
            fprintf(stderr, "FAIL:ext4_exercise an open file without names says nlink=%u size=%llu\n",
                    (unsigned)now.nlink, (unsigned long long)now.size);
            return -1;
        }
        printf("orphankeep ok inode=%llu\n", (unsigned long long)g_held.id);
        return 0;
    }
    if (strcmp(c, "orphanclose") == 0 && n == 1) {
        /* The last description goes: the file goes with it. */
        uint8_t b;

        if (!g_holding) {
            return -VIBEOS_EINVAL;
        }
        g_holding = 0;
        if (vibeos_fs_release_node(&g_mnt, g_held.id) != 0) {
            fprintf(stderr, "FAIL:ext4_exercise orphanclose: the release failed\n");
            return -1;
        }
        if (vibeos_fs_read_at(&g_mnt, &g_held, 0, &b, 1u) >= 0) {
            fprintf(stderr, "FAIL:ext4_exercise a closed orphan still reads\n");
            return -1;
        }
        printf("orphanclose ok\n");
        return 0;
    }
    if (strcmp(c, "faildata") == 0 && n == 2) {
        /* The n-th data write from here fails once (E4-009). */
        vibeos_ext4_fail_data_after((uint32_t)strtoul(a[1], NULL, 10));
        return 0;
    }
    if (strcmp(c, "writeonce") == 0 && n == 5) {
        /* One write_at, its result printed and not retried: what a write that
         * failed half-way left behind is the question, so nothing may write
         * over it. */
        static uint8_t buf[1u << 20];
        uint64_t off = strtoull(a[2], NULL, 10), len = strtoull(a[3], NULL, 10);
        uint32_t seed = (uint32_t)strtoul(a[4], NULL, 10);
        long r;

        if (len > sizeof(buf) || vibeos_fs_lookup(&g_mnt, a[1], &node) != 0) {
            return -VIBEOS_EINVAL;
        }
        for (uint64_t i = 0; i < len; i++) {
            buf[i] = pat(seed, off + i);
        }
        r = vibeos_fs_write_at(&g_mnt, &node, off, buf, (uint32_t)len);
        printf("writeonce %s returned %ld\n", a[1], r);
        return 0;
    }
    if (strcmp(c, "create") == 0 && n == 3) {
        return vibeos_fs_create(&g_mnt, a[1], (uint32_t)strtoul(a[2], NULL, 8), &node);
    }
    if (strcmp(c, "write") == 0 && n == 5) {
        return write_pattern(a[1], strtoull(a[2], NULL, 10), strtoull(a[3], NULL, 10),
                             (uint32_t)strtoul(a[4], NULL, 10));
    }
    if (strcmp(c, "bigwrite") == 0 && n == 5) {
        /* One write_at of the whole length - past 2 MiB, where `write` cuts
         * its pieces at 1 MiB (E4-026). The driver may answer short - after
         * WRITE_RUNS_MAX fresh runs, on a fragmented volume, which POSIX
         * allows and the Linux handler's 4 KiB pieces never reach - so the
         * rest is asked for as a caller would, and every answer must move. */
        uint64_t off = strtoull(a[2], NULL, 10), len = strtoull(a[3], NULL, 10);
        uint32_t seed = (uint32_t)strtoul(a[4], NULL, 10);
        uint8_t *big;
        long r;

        if (len == 0u || len > (64u << 20) || vibeos_fs_lookup(&g_mnt, a[1], &node) != 0) {
            return -VIBEOS_EINVAL;
        }
        big = malloc(len);
        if (!big) {
            return -VIBEOS_EINVAL;
        }
        for (uint64_t i = 0; i < len; i++) {
            big[i] = pat(seed, off + i);
        }
        for (uint64_t done = 0; done < len; done += (uint64_t)r) {
            r = vibeos_fs_write_at(&g_mnt, &node, off + done, big + done, (uint32_t)(len - done));
            if (r <= 0) {
                free(big);
                return r < 0 ? (int)r : -VIBEOS_EIO;   /* no progress */
            }
        }
        free(big);
        return 0;
    }
    if (strcmp(c, "read") == 0 && n == 2) {
        /* The whole file, through the mount's read: what a read changes is
         * atime (E4-013). */
        static uint8_t rbuf[65536];
        uint64_t pos = 0;   /* not "at": that is the attributes, above */
        long got;

        if (vibeos_fs_lookup(&g_mnt, a[1], &node) != 0) {
            return -VIBEOS_ENOENT;
        }
        do {
            got = vibeos_fs_read_at(&g_mnt, &node, pos, rbuf, sizeof(rbuf));
            pos += got > 0 ? (uint64_t)got : 0u;
        } while (got > 0);
        return got < 0 ? (int)got : 0;
    }
    if (strcmp(c, "truncate") == 0 && n == 3) {
        if (vibeos_fs_lookup(&g_mnt, a[1], &node) != 0) {
            return -VIBEOS_ENOENT;
        }
        return vibeos_fs_truncate(&g_mnt, &node, strtoull(a[2], NULL, 10));
    }
    if (strcmp(c, "fallocate") == 0 && n == 5) {
        /* VIBEOS_FALLOC_* mode, offset, length (E4-011). */
        uint32_t m = (uint32_t)strtoul(a[2], NULL, 10);
        uint64_t off = strtoull(a[3], NULL, 10);

        if (vibeos_fs_lookup(&g_mnt, a[1], &node) != 0) {
            return -VIBEOS_ENOENT;
        }
        if ((m & VIBEOS_FALLOC_PUNCH) != 0u && off >= node.size) {
            g_line_unchanged = 1;   /* Linux's punch changes nothing there, times included */
        }
        return vibeos_fs_fallocate(&g_mnt, &node, m, off, strtoull(a[4], NULL, 10));
    }
    if (strcmp(c, "idle") == 0 && n == 1) {
        /* The machine idle past the commit interval: the clock moves six
         * seconds and the idle loop's tick runs (E4-016). What the running
         * transaction held is then committed, as fsync would have. */
        g_line_unchanged = 1;
        g_now += 6000000000ull;
        /* This mount, which the exerciser holds and never registered at a
         * path: tick_all walks the table of mounts and would not see it -
         * the first version did exactly that, and the sweep went red on a
         * floor the tick had never raised. */
        return vibeos_fs_tick(&g_mnt);
    }
    if (strcmp(c, "fsync") == 0 && n == 1) {
        g_line_unchanged = 1;   /* it commits what others did, and changes nothing itself */
        return vibeos_fs_sync(&g_mnt);
    }
    if (strcmp(c, "chattr") == 0 && n == 3) {
        /* chattr +F / -F as chattr does it: read every flag, change one,
         * hand them all back (E4-030); "+X" asks for a flag the driver does
         * not change (immutable). */
        uint32_t fl;
        int r;

        if (vibeos_fs_lookup(&g_mnt, a[1], &node) != 0) {
            return -VIBEOS_ENOENT;
        }
        r = vibeos_fs_getflags(&g_mnt, &node, &fl);
        if (r != 0) {
            return r;
        }
        if (strcmp(a[2], "+F") == 0) {
            fl |= VIBEOS_FSFL_CASEFOLD;
        } else if (strcmp(a[2], "-F") == 0) {
            fl &= ~VIBEOS_FSFL_CASEFOLD;
        } else {
            fl |= 0x10u;   /* FS_IMMUTABLE_FL */
        }
        return vibeos_fs_setflags(&g_mnt, &node, fl);
    }
    if (strcmp(c, "setacl") == 0 && n == 4) {
        /* A POSIX ACL, access (a) or default (d), as Linux's attribute value
         * in hex - what the Linux handler hands vibeos_acl_set (E4-011). */
        static uint8_t v[4096];
        uint32_t len = 0;
        const char *h = a[3];

        if (vibeos_fs_lookup(&g_mnt, a[1], &node) != 0) {
            return -VIBEOS_ENOENT;
        }
        while (h[0] && h[1] && len < sizeof(v)) {
            unsigned b;

            if (sscanf(h, "%2x", &b) != 1) {
                return -VIBEOS_EINVAL;
            }
            v[len++] = (uint8_t)b;
            h += 2;
        }
        return vibeos_acl_set(&g_mnt, &node, a[2][0] == 'a' ? VIBEOS_XATTR_ACL_ACCESS : VIBEOS_XATTR_ACL_DEFAULT,
                              v, len);
    }
    if ((strcmp(c, "setxattr") == 0 && n == 6) || (strcmp(c, "removexattr") == 0 && n == 3)) {
        /* A user.* name; a value of `len` bytes of pat(seed) from 0, and
         * Linux's XATTR_CREATE 1 / REPLACE 2, which are VIBEOS_XATTR_*'s. */
        static uint8_t v[4096];
        const char *nm = a[2];
        uint32_t len, i;

        if (strncmp(nm, "user.", 5) != 0 || vibeos_fs_lookup(&g_mnt, a[1], &node) != 0) {
            return -VIBEOS_ENOENT;
        }
        nm += 5;
        if (c[0] == 'r') {
            return vibeos_fs_removexattr(&g_mnt, &node, VIBEOS_XATTR_USER, nm, (uint32_t)strlen(nm));
        }
        len = (uint32_t)strtoul(a[3], NULL, 10);
        if (len > sizeof(v)) {
            return -VIBEOS_EINVAL;
        }
        for (i = 0; i < len; i++) {
            v[i] = pat((uint32_t)strtoul(a[4], NULL, 10), i);
        }
        return vibeos_fs_setxattr(&g_mnt, &node, VIBEOS_XATTR_USER, nm, (uint32_t)strlen(nm), v, len,
                                  (uint32_t)strtoul(a[5], NULL, 10));
    }
    if (strcmp(c, "mknod") == 0 && n == 3) {
        /* A node by its whole mode, type included, in octal (E4-011). */
        return vibeos_fs_mknod(&g_mnt, a[1], (uint32_t)strtoul(a[2], NULL, 8), &node);
    }
    if (strcmp(c, "mkdir") == 0 && n == 2) {
        return vibeos_fs_mkdir(&g_mnt, a[1]);
    }
    if (strcmp(c, "rmdir") == 0 && n == 2) {
        return vibeos_fs_rmdir(&g_mnt, a[1]);
    }
    if (strcmp(c, "unlink") == 0 && n == 2) {
        return vibeos_fs_unlink(&g_mnt, a[1]);
    }
    if (strcmp(c, "rename") == 0 && n == 3) {
        return vibeos_fs_rename(&g_mnt, a[1], a[2], 0);
    }
    if (strcmp(c, "renamenx") == 0 && n == 3) {
        return vibeos_fs_rename(&g_mnt, a[1], a[2], VIBEOS_RENAME_NOREPLACE);
    }
    if (strcmp(c, "link") == 0 && n == 3) {
        return vibeos_fs_link(&g_mnt, a[1], a[2]);
    }
    if (strcmp(c, "symlink") == 0 && n == 3) {
        return vibeos_fs_symlink(&g_mnt, a[1], a[2]);
    }
    memset(&at, 0, sizeof(at));
    if (strcmp(c, "chmod") == 0 && n == 3) {
        at.valid = VIBEOS_ATTR_MODE;
        at.mode = (uint32_t)strtoul(a[2], NULL, 8);
        return vibeos_fs_setattr(&g_mnt, a[1], &at);
    }
    if (strcmp(c, "chown") == 0 && n == 4) {
        at.valid = VIBEOS_ATTR_UID | VIBEOS_ATTR_GID;
        at.uid = (uint32_t)strtoul(a[2], NULL, 10);
        at.gid = (uint32_t)strtoul(a[3], NULL, 10);
        return vibeos_fs_setattr(&g_mnt, a[1], &at);
    }
    if (strcmp(c, "utimes") == 0 && n == 4) {
        at.valid = VIBEOS_ATTR_ATIME | VIBEOS_ATTR_MTIME;
        at.atime_ns = strtoull(a[2], NULL, 10) * 1000000000ull;
        at.mtime_ns = strtoull(a[3], NULL, 10) * 1000000000ull;
        return vibeos_fs_setattr(&g_mnt, a[1], &at);
    }
    if (strcmp(c, "fillup") == 0 && n == 3) {
        /* Write until the volume says no: the refusal must be ENOSPC, the file
         * must hold what was written, and what e2fsck makes of the rest is the
         * script's business. */
        static uint8_t buf[1u << 20];
        uint32_t seed = (uint32_t)strtoul(a[2], NULL, 10);
        uint64_t off = 0;

        if (vibeos_fs_lookup(&g_mnt, a[1], &node) != 0) {
            return -VIBEOS_ENOENT;
        }
        for (;;) {
            long r;

            for (uint64_t i = 0; i < sizeof(buf); i++) {
                buf[i] = pat(seed, off + i);
            }
            r = vibeos_fs_write_at(&g_mnt, &node, off, buf, sizeof(buf));
            if (r > 0) {
                off += (uint64_t)r;
            }
            if (r != (long)sizeof(buf)) {
                break;
            }
        }
        if (vibeos_fs_write_at(&g_mnt, &node, off, buf, 1u) != -VIBEOS_ENOSPC) {
            fprintf(stderr, "FAIL:ext4_exercise a full volume did not say ENOSPC\n");
            return -1;
        }
        printf("fillup size=%llu\n", (unsigned long long)off);
        return 0;
    }
    if (strcmp(c, "stride") == 0 && n == 6) {
        /* count pieces of len bytes, step apart: a file of many extents. */
        unsigned count = (unsigned)strtoul(a[2], NULL, 10);
        uint64_t step = strtoull(a[3], NULL, 10), len = strtoull(a[4], NULL, 10);

        for (unsigned k = 0; k < count; k++) {
            int r = write_pattern(a[1], k * step, len, (uint32_t)strtoul(a[5], NULL, 10));

            if (r != 0) {
                return r;
            }
        }
        return 0;
    }
    if (strcmp(c, "writefile") == 0 && n == 4) {
        /* The whole-file shape: create or replace, then the bytes. */
        static uint8_t buf[1u << 20];
        uint64_t len = strtoull(a[2], NULL, 10);
        uint32_t seed = (uint32_t)strtoul(a[3], NULL, 10);
        long r;

        if (len > sizeof(buf)) {
            return -1;
        }
        for (uint64_t i = 0; i < len; i++) {
            buf[i] = pat(seed, i);
        }
        r = vibeos_fs_write_file(&g_mnt, a[1], buf, (uint32_t)len);
        return r == (long)len ? 0 : -1;
    }
    if (strcmp(c, "fill") == 0 && n == 4) {
        unsigned count = (unsigned)strtoul(a[2], NULL, 10);
        uint64_t len = strtoull(a[3], NULL, 10);

        for (unsigned k = 0; k < count; k++) {
            char path[1100];
            int r;

            snprintf(path, sizeof(path), "%s/f%05u", a[1], k);
            r = vibeos_fs_create(&g_mnt, path, 0644, &node);
            if (r == 0 && len > 0u) {
                r = write_pattern(path, 0, len, k + 1u);
            }
            if (r != 0) {
                return r;
            }
        }
        return 0;
    }
    if (strcmp(c, "stat") == 0 && n == 4) {
        /* A file's size and link count as the driver reports them, held to
         * what the script says (a check the host cannot make for us: it is
         * the driver's own answer, so it is only for things e2fsck cannot
         * see, like what a lookup returns for a name just made). */
        if (vibeos_fs_lookup(&g_mnt, a[1], &node) != 0) {
            return -VIBEOS_ENOENT;
        }
        if (node.size != strtoull(a[2], NULL, 10) || node.nlink != (uint32_t)strtoul(a[3], NULL, 10)) {
            fprintf(stderr, "FAIL:ext4_exercise %s has size %llu nlink %u\n", a[1], (unsigned long long)node.size,
                    node.nlink);
            return -1;
        }
        return 0;
    }
    fprintf(stderr, "FAIL:ext4_exercise cannot read the script line: %s\n", c);
    return -1;
}

/* The script is script.txt beside fs.img, as the image is: a program that opens
 * a path from its arguments is a path-injection finding for code scanning
 * (CodeQL #322, again as #358), and a test has no reason to take one. */
static int apply(void) {
    FILE *f = fopen("script.txt", "r");
    char line[4400];
    unsigned lines = 0;
    vibeos_fs_statfs_t sf;

    if (!f) {
        fprintf(stderr, "FAIL:ext4_exercise cannot open the script\n");
        return 2;
    }
    vibeos_fs_set_clock(clock_fn);
    if (!g_fs.writable) {
        fprintf(stderr, "FAIL:ext4_exercise the driver did not take the volume as writable\n");
        return 1;
    }
    while (fgets(line, sizeof(line), f)) {
        size_t l = strlen(line);
        int r;

        while (l > 0u && (line[l - 1u] == '\n' || line[l - 1u] == '\r')) {
            line[--l] = 0;
        }
        if (l == 0u || line[0] == '#') {
            continue;
        }
        lines++;
        r = run_line(line);
        if (r != 0) {
            fprintf(stderr, "FAIL:ext4_exercise line %u (%s) returned %d\n", lines, line, r);
            return 1;
        }
    }
    fclose(f);
    if (vibeos_fs_settle(&g_mnt) != 0) {
        fprintf(stderr, "FAIL:ext4_exercise sync failed\n");
        return 1;
    }
    if (vibeos_ext2_csum_failures() != 0u) {
        fprintf(stderr, "FAIL:ext4_exercise %llu checksum failures while writing\n",
                (unsigned long long)vibeos_ext2_csum_failures());
        return 1;
    }
    /* Through save_image, as every other mode: this wrote the image back on
     * its own, and for a mapped volume (E4-004's, 8 TiB of sparse file) that
     * was every byte of it written - zeros over every hole - and 333 GB of a
     * disk gone before anything noticed. */
    if (save_image() != 0) {
        return 1;
    }
    (void)vibeos_fs_statfs(&g_mnt, &sf);
    printf("applied lines=%u blocks_free=%llu files_free=%llu\n", lines, (unsigned long long)sf.blocks_free,
           (unsigned long long)sf.files_free);
    /* The lookups an htree answered, as walk says them: on stderr (E4-030). */
    fflush(stdout);
    fprintf(stderr, "dx_lookups=%llu\n", (unsigned long long)vibeos_ext2_dx_lookups());
    {
        /* What the journal cost (E4-016): flushes per commit is the number. */
        vibeos_ext4_journal_stats_t js;

        vibeos_ext4_journal_stats(&js);
        fprintf(stderr, "journal commits=%llu flushes=%llu checkpoints=%llu\n", (unsigned long long)js.commits,
                (unsigned long long)js.flushes, (unsigned long long)js.checkpoints);
    }
    return 0;
}

static int save_image(void) {
    FILE *f;

    if (g_mapped) {
        /* Shared, so already the file's: what the driver wrote is in the
         * file's pages, and a reader after this process sees it. Writing the
         * mapping back as the other modes do writes every byte of 8 TiB -
         * zeros over every hole - which is how one run filled a disk. */
        return 0;
    }
    f = fopen("fs.img", "r+b");

    if (!f || fwrite(g_img, 1, (size_t)(g_sectors * 512u), f) != (size_t)(g_sectors * 512u)) {
        fprintf(stderr, "FAIL:ext4_exercise cannot write fs.img back\n");
        if (f) {
            fclose(f);
        }
        return 1;
    }
    fclose(f);
    return 0;
}

/* The script line by line, each line one operation and so one transaction:
 * with `states`, the tree is listed before the first line and after each, as
 * "STATE k" followed by what walk prints. A cut run goes on past the cut, as
 * the machine would have until it stopped; what the driver believed it did is
 * not the question, and the sweep judges the disk instead. */
static int script(int states) {
    FILE *f = fopen("script.txt", "r");
    char line[4400];
    unsigned lines = 0, changing = 0;

    if (!f) {
        fprintf(stderr, "FAIL:ext4_exercise cannot open the script\n");
        return 2;
    }
    vibeos_fs_set_clock(clock_fn);
    if (!g_fs.writable) {
        fprintf(stderr, "FAIL:ext4_exercise the driver did not take the volume as writable\n");
        fclose(f);
        return 1;
    }
    if (states) {
        printf("STATE 0\n");
        (void)walk("/");
    }
    while (fgets(line, sizeof(line), f)) {
        size_t l = strlen(line);
        int r;

        while (l > 0u && (line[l - 1u] == '\n' || line[l - 1u] == '\r')) {
            line[--l] = 0;
        }
        if (l == 0u || line[0] == '#') {
            continue;
        }
        lines++;
        /* A refusal changes nothing and so commits nothing (step 5's random
         * scripts are full of them); every other line must commit. */
        g_line_unchanged = 0;
        {
            int was_dead = g_dead;
            /* Read before run_line, which cuts the line at its tabs. */
            int may_change = strncmp(line, "fail\t", 5) != 0 && strncmp(line, "refuse\t", 7) != 0;

            r = run_line(line);
            changing += may_change && !g_line_unchanged;
            /* Which line the power went in: what a cut's tree is judged
             * against, said outright rather than worked out from the counts. */
            if (!was_dead && g_dead) {
                printf("cut_at_line=%u %s\n", lines, line);
            }
            /* A sync that returned with the device still alive: everything
             * before it is on the disk, so no cut after it may undo it
             * (E4-016). */
            if (r == 0 && !g_dead && (strcmp(line, "fsync") == 0 || strcmp(line, "idle") == 0)) {
                printf("synced_at_line=%u\n", lines);
            }
        }
        if (r != 0 && g_cut == 0u) {
            fprintf(stderr, "FAIL:ext4_exercise line %u (%s) returned %d\n", lines, line, r);
            fclose(f);
            return 1;
        }
        if (states) {
            printf("STATE %u\n", lines);
            (void)walk("/");
        }
    }
    fclose(f);
    if (!states) {
        vibeos_ext4_journal_stats_t js;

        /* A settle last, as a shutdown does, counted with the rest: with
         * group commit (E4-016) the last operations are still in the running
         * transaction, so the cuts must fall inside it too, and the uncut
         * volume is a clean one. On a device already dead it changes nothing. */
        (void)vibeos_fs_settle(&g_mnt);

        vibeos_ext4_journal_stats(&js);
        printf("writes=%llu lines=%u commits=%llu\n", (unsigned long long)g_writes, lines,
               (unsigned long long)js.commits);
        /* Every changing line through the journal - several to a commit
         * since group commit (E4-016), so counted as operations. */
        if (g_cut == 0u && js.ops < changing) {
            fprintf(stderr, "FAIL:ext4_exercise %u operations went %llu times through the journal: it is not in the path\n",
                    changing, (unsigned long long)js.ops);
            return 1;
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    /* fs.img in the working directory, as fat_exercise does: a program
     * that opened a path from its arguments is a path-injection finding for
     * code scanning, and a test has no reason to take one. */
    g_writable_dev = argc >= 2 && (strcmp(argv[1], "apply") == 0 || strcmp(argv[1], "powercut") == 0 ||
                                   strcmp(argv[1], "recover") == 0 || strcmp(argv[1], "states") == 0);
    if (argc == 3 && strcmp(argv[1], "powercut") == 0) {
        g_cut = strtoull(argv[2], NULL, 10);
    }
    if (argc < 2 || load("fs.img") != 0) {
        fprintf(stderr, "usage: vibeos_ext4_exercise walk|apply|refused-file <path>|refused-mount, beside fs.img (and script.txt for apply)\n");
        return 2;
    }
    if (strcmp(argv[1], "refused-mount") == 0) {
        if (vibeos_ext2_mount(&g_fs, &g_bc, 0) == 0) {
            fprintf(stderr, "FAIL:ext4_exercise mounted a volume with a feature it does not read\n");
            return 1;
        }
        printf("refused features=0x%x\n", vibeos_ext2_last_refused_features());
        return vibeos_ext2_last_refused_features() != 0u ? 0 : 1;
    }
    /* Before the mount, as on the machine (hw_rtc_bringup): a mount dates
     * itself (E4-013). */
    if (getenv("VIBEOS_EX_CLOCK_STEP_NS")) {
        g_step = strtoull(getenv("VIBEOS_EX_CLOCK_STEP_NS"), NULL, 10);
    }
    vibeos_fs_set_clock(clock_fn);
    if (mount_volume() != 0) {
        fprintf(stderr, "FAIL:ext4_exercise cannot mount what mkfs.ext4 made (refused=0x%x)\n",
                vibeos_ext2_last_refused_features());
        return 1;
    }
    if (!g_fs.csum) {
        fprintf(stderr, "FAIL:ext4_exercise the volume has no metadata_csum: nothing would be checked\n");
        return 1;
    }
    if (strcmp(argv[1], "apply") == 0 && argc == 2) {
        return apply();
    }
    if (strcmp(argv[1], "states") == 0 && argc == 2) {
        return script(1);
    }
    if (strcmp(argv[1], "powercut") == 0 && argc == 3) {
        int r = script(0);   /* which settles the volume last, counted */
        return r != 0 ? r : save_image();
    }
    if (strcmp(argv[1], "recover") == 0 && argc == 2) {
        vibeos_ext4_journal_stats_t js;

        if (vibeos_fs_settle(&g_mnt) != 0 || save_image() != 0) {
            return 1;
        }
        vibeos_ext4_journal_stats(&js);
        printf("replayed=%llu bad_tags=%llu\n", (unsigned long long)js.replayed, (unsigned long long)js.bad_tags);
        return js.bad_tags == 0u ? 0 : 1;
    }
    if (g_fs.writable) {
        fprintf(stderr, "FAIL:ext4_exercise a device that takes no writes was given the writing operations\n");
        return 1;
    }
    if (strcmp(argv[1], "walk") == 0) {
        g_walk_attrs = argc == 3 && strcmp(argv[2], "attrs") == 0;
        int bad = walk("/");

        if (vibeos_ext2_csum_failures() != 0u) {
            fprintf(stderr, "FAIL:ext4_exercise %llu checksum failures on a healthy volume\n",
                    (unsigned long long)vibeos_ext2_csum_failures());
            bad = 1;
        }
        /* How many of the walk's lookups an htree answered (E4-005): on
         * stderr, so the listing on stdout stays what it always was. */
        fflush(stdout);   /* the listing first, whole: a caller may read both streams as one */
        fprintf(stderr, "dx_lookups=%llu\n", (unsigned long long)vibeos_ext2_dx_lookups());
        return bad;
    }
    if (strcmp(argv[1], "refused-file") == 0 && argc == 3) {
        vibeos_fs_node_t node;

        if (g_ops->lookup(&g_fs, argv[2], &node) == 0) {
            fprintf(stderr, "FAIL:ext4_exercise read %s through a checksum that does not match\n", argv[2]);
            return 1;
        }
        printf("refused csum_failures=%llu\n", (unsigned long long)vibeos_ext2_csum_failures());
        return vibeos_ext2_csum_failures() != 0u ? 0 : 1;
    }
    return 2;
}
