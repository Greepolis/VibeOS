/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The ext4 driver, on a volume mkfs.ext4 made (docs/filesystem-design/ext4.md
 * steps 1 and 2).
 *
 *     vibeos_ext4_exercise walk                (in the directory that holds fs.img)
 *     vibeos_ext4_exercise apply <script>      (changes fs.img through the driver)
 *     vibeos_ext4_exercise refused-file <path> (a path on the volume)
 *     vibeos_ext4_exercise refused-mount
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

#include "vibeos/abi_linux.h"
#include "vibeos/ext2.h"
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

static int load(const char *path) {
    FILE *f = fopen(path, "rb");
    long n;

    if (!f || fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) <= 0 || fseek(f, 0, SEEK_SET) != 0) {
        return -1;
    }
    g_img = (uint8_t *)malloc((size_t)n);
    if (!g_img || fread(g_img, 1, (size_t)n, f) != (size_t)n) {
        return -1;
    }
    fclose(f);
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
static int mount_volume(void) {
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

static int walk(const char *dir) {
    char name[256], path[1024];
    uint64_t size;
    int is_dir, bad = 0;

    for (uint32_t i = 0; g_ops->list(&g_fs, dir, i, name, sizeof(name), &size, &is_dir) == 0; i++) {
        vibeos_fs_node_t node;

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
            printf("%s\tD\n", path);
            bad |= walk(path);
        } else {
            long long crc = file_crc(path, size);

            if (crc < 0) {
                fprintf(stderr, "FAIL:ext4_exercise cannot read %s\n", path);
                bad = 1;
                continue;
            }
            printf("%s\tF\t%llu\t%08llx\n", path, (unsigned long long)size, crc);
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

/* The clock timestamps come from: a second per call, from a fixed day. */
static uint64_t g_now = 1700000000ull * 1000000000ull;

static uint64_t clock_fn(void) {
    g_now += 1000000000ull;
    return g_now;
}

static int errno_of(const char *name) {
    static const struct { const char *name; int e; } t[] = {
        { "EPERM", VIBEOS_EPERM }, { "ENOENT", VIBEOS_ENOENT }, { "EEXIST", VIBEOS_EEXIST },
        { "ENOTDIR", VIBEOS_ENOTDIR }, { "EISDIR", VIBEOS_EISDIR }, { "EINVAL", VIBEOS_EINVAL },
        { "ENOTEMPTY", VIBEOS_ENOTEMPTY }, { "ENAMETOOLONG", VIBEOS_ENAMETOOLONG }, { "ENOSPC", VIBEOS_ENOSPC },
        { "EFBIG", VIBEOS_EFBIG },
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
        if ((uint64_t)r != n) {
            return -VIBEOS_EIO;
        }
        done += n;
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
    if (strcmp(c, "create") == 0 && n == 3) {
        return vibeos_fs_create(&g_mnt, a[1], (uint32_t)strtoul(a[2], NULL, 8), &node);
    }
    if (strcmp(c, "write") == 0 && n == 5) {
        return write_pattern(a[1], strtoull(a[2], NULL, 10), strtoull(a[3], NULL, 10),
                             (uint32_t)strtoul(a[4], NULL, 10));
    }
    if (strcmp(c, "truncate") == 0 && n == 3) {
        if (vibeos_fs_lookup(&g_mnt, a[1], &node) != 0) {
            return -VIBEOS_ENOENT;
        }
        return vibeos_fs_truncate(&g_mnt, &node, strtoull(a[2], NULL, 10));
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
    if (vibeos_fs_sync(&g_mnt) != 0) {
        fprintf(stderr, "FAIL:ext4_exercise sync failed\n");
        return 1;
    }
    if (vibeos_ext2_csum_failures() != 0u) {
        fprintf(stderr, "FAIL:ext4_exercise %llu checksum failures while writing\n",
                (unsigned long long)vibeos_ext2_csum_failures());
        return 1;
    }
    /* Written over the image that is there, never created: "w" would make one
     * with mode 0666 if it were gone, which code scanning reports (#357). */
    f = fopen("fs.img", "r+b");
    if (!f || fwrite(g_img, 1, (size_t)(g_sectors * 512u), f) != (size_t)(g_sectors * 512u)) {
        fprintf(stderr, "FAIL:ext4_exercise cannot write fs.img back\n");
        return 1;
    }
    fclose(f);
    (void)vibeos_fs_statfs(&g_mnt, &sf);
    printf("applied lines=%u blocks_free=%llu files_free=%llu\n", lines, (unsigned long long)sf.blocks_free,
           (unsigned long long)sf.files_free);
    return 0;
}

int main(int argc, char **argv) {
    /* fs.img in the working directory, as fat_exercise does: a program
     * that opened a path from its arguments is a path-injection finding for
     * code scanning, and a test has no reason to take one. */
    g_writable_dev = argc >= 2 && strcmp(argv[1], "apply") == 0;
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
    if (g_fs.writable) {
        fprintf(stderr, "FAIL:ext4_exercise a device that takes no writes was given the writing operations\n");
        return 1;
    }
    if (strcmp(argv[1], "walk") == 0) {
        int bad = walk("/");

        if (vibeos_ext2_csum_failures() != 0u) {
            fprintf(stderr, "FAIL:ext4_exercise %llu checksum failures on a healthy volume\n",
                    (unsigned long long)vibeos_ext2_csum_failures());
            bad = 1;
        }
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
