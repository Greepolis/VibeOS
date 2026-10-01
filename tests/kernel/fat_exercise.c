/* The FAT writer, on a volume somebody else made, for somebody else to read
 * (docs/abi/ L1 step 4).
 *
 *     vibeos_fat_exercise <filesystem image> <output directory>
 *
 * fat_tests.c formats its own volume and reads back what it wrote, which proves
 * the writer and the reader agree - and ISO9660 is the record of how little that
 * can mean (CLAUDE.md). This is the other half: scripts/dev/verify-fat-mtools.sh
 * formats a volume with mformat and puts a file on it with mcopy, this program
 * mounts it and creates, writes, truncates, renames and removes through the
 * driver, and the script then reads everything back with mdir and mcopy.
 *
 * What it expects mtools to find is written beside the image: `manifest` lists
 * every file that should exist, one "<number> <path>" a line, and <number>.bin
 * holds the bytes it should have; `dirs` lists the directories; `free` holds
 * the free bytes the driver's statfs reports. Anything else on the volume, or
 * anything missing, is the script's failure to report. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vibeos/fat.h"
#include "vibeos/vfs.h"
#include "vibeos/abi_linux.h"

#define FIRST 2048u   /* the driver reads a volume at sector 0 as the boot volume */
#define SLOTS 64u
#define MAX_FILES 128

static uint8_t *g_img;
static uint64_t g_sectors;
static vibeos_blockdev_t g_dev;
static vibeos_blockcache_t g_bc;
static vibeos_block_slot_t g_slots[SLOTS];
static uint8_t g_slot_data[SLOTS][512];
static vibeos_fsmount_t g_m;

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

/* ---- what the volume should hold when this is done ---------------------------------- */

typedef struct {
    char path[200];
    uint8_t *data;
    size_t size;
    int live;
} want_t;

static want_t g_want[MAX_FILES];
static int g_nwant;
static char g_dirs[32][200];
static int g_ndirs;

static want_t *want(const char *path) {
    int i;
    for (i = 0; i < g_nwant; i++) {
        if (g_want[i].live && strcmp(g_want[i].path, path) == 0) {
            return &g_want[i];
        }
    }
    if (g_nwant >= MAX_FILES) {
        fprintf(stderr, "too many files\n");
        exit(2);
    }
    memset(&g_want[g_nwant], 0, sizeof(want_t));
    snprintf(g_want[g_nwant].path, sizeof(g_want[g_nwant].path), "%s", path);
    g_want[g_nwant].live = 1;
    return &g_want[g_nwant++];
}

static void die(const char *what, const char *path, long r) {
    fprintf(stderr, "FAIL:fat_exercise %s %s (%ld)\n", what, path, r);
    exit(1);
}

static void x_create(const char *path) {
    vibeos_fs_node_t n;
    int r = vibeos_fs_create(&g_m, path, 0644u, &n);
    if (r != 0) {
        die("create", path, r);
    }
    (void)want(path);
}

static void x_write(const char *path, uint64_t off, uint32_t len, uint8_t seed) {
    vibeos_fs_node_t n;
    uint8_t *buf = malloc(len);
    want_t *w = want(path);
    uint32_t i;
    long r;

    for (i = 0; i < len; i++) {
        buf[i] = (uint8_t)(seed + i * 31u);
    }
    if (vibeos_fs_lookup(&g_m, path, &n) != 0) {
        die("lookup", path, -1);
    }
    r = vibeos_fs_write_at(&g_m, &n, off, buf, len);
    if (r != (long)len) {
        die("write", path, r);
    }
    if (off + len > w->size) {
        w->data = realloc(w->data, off + len);
        memset(w->data + w->size, 0, off + len - w->size);
        w->size = off + len;
    }
    memcpy(w->data + off, buf, len);
    free(buf);
}

static void x_truncate(const char *path, uint64_t size) {
    vibeos_fs_node_t n;
    want_t *w = want(path);
    int r;

    if (vibeos_fs_lookup(&g_m, path, &n) != 0 || (r = vibeos_fs_truncate(&g_m, &n, size)) != 0) {
        die("truncate", path, -1);
    }
    w->data = realloc(w->data, size ? size : 1);
    if (size > w->size) {
        memset(w->data + w->size, 0, size - w->size);
    }
    w->size = size;
}

static void x_unlink(const char *path) {
    int r = vibeos_fs_unlink(&g_m, path);
    if (r != 0) {
        die("unlink", path, r);
    }
    want(path)->live = 0;
}

static void x_mkdir(const char *path) {
    int r = vibeos_fs_mkdir(&g_m, path);
    if (r != 0) {
        die("mkdir", path, r);
    }
    snprintf(g_dirs[g_ndirs++], sizeof(g_dirs[0]), "%s", path);
}

static void x_rmdir(const char *path) {
    int i, r = vibeos_fs_rmdir(&g_m, path);
    if (r != 0) {
        die("rmdir", path, r);
    }
    for (i = 0; i < g_ndirs; i++) {
        if (strcmp(g_dirs[i], path) == 0) {
            g_dirs[i][0] = 0;
        }
    }
}

/* A file moves; a directory moves with everything whose path begins with it. */
static void x_rename(const char *from, const char *to) {
    size_t fl = strlen(from);
    int i, r = vibeos_fs_rename(&g_m, from, to, 0);

    if (r != 0) {
        die("rename", from, r);
    }
    for (i = 0; i < g_nwant; i++) {
        if (g_want[i].live && strcmp(g_want[i].path, to) == 0) {
            g_want[i].live = 0;   /* replaced */
        }
    }
    for (i = 0; i < g_nwant; i++) {
        char moved[200];
        if (!g_want[i].live) {
            continue;
        }
        if (strcmp(g_want[i].path, from) == 0) {
            snprintf(g_want[i].path, sizeof(g_want[i].path), "%s", to);
        } else if (strncmp(g_want[i].path, from, fl) == 0 && g_want[i].path[fl] == '/') {
            snprintf(moved, sizeof(moved), "%s%s", to, g_want[i].path + fl);
            snprintf(g_want[i].path, sizeof(g_want[i].path), "%s", moved);
        }
    }
    for (i = 0; i < g_ndirs; i++) {
        char moved[200];
        if (strcmp(g_dirs[i], from) == 0) {
            snprintf(g_dirs[i], sizeof(g_dirs[i]), "%s", to);
        } else if (strncmp(g_dirs[i], from, fl) == 0 && g_dirs[i][fl] == '/') {
            snprintf(moved, sizeof(moved), "%s%s", to, g_dirs[i] + fl);
            snprintf(g_dirs[i], sizeof(g_dirs[i]), "%s", moved);
        }
    }
}

/* A file mtools put there: read through the driver, so the expectation starts
 * from what is really on the volume. */
static void x_adopt(const char *path) {
    vibeos_fs_node_t n;
    want_t *w = want(path);
    long r;

    if (vibeos_fs_lookup(&g_m, path, &n) != 0) {
        die("a file mtools wrote is not found", path, -1);
    }
    w->data = malloc(n.size ? n.size : 1);
    w->size = n.size;
    r = vibeos_fs_read_at(&g_m, &n, 0, w->data, (uint32_t)n.size);
    if (r != (long)n.size) {
        die("a file mtools wrote does not read", path, r);
    }
}

int main(int argc, char **argv) {
    const vibeos_fs_driver_t *drv = vibeos_fat_fs_driver();
    vibeos_fs_statfs_t sf;
    char name[300];
    FILE *f;
    long size;
    uint32_t i;
    int k;

    if (argc < 3) {
        fprintf(stderr, "usage: %s <image> <outdir>\n", argv[0]);
        return 2;
    }
    f = fopen(argv[1], "rb");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", argv[1]);
        return 2;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    g_sectors = FIRST + (uint64_t)size / 512u;
    g_img = calloc(1, (size_t)g_sectors * 512u);
    if (fread(g_img + FIRST * 512u, 1, (size_t)size, f) != (size_t)size) {
        return 2;
    }
    fclose(f);

    g_dev.read = img_read;
    g_dev.write = img_write;
    g_dev.sectors = g_sectors;
    for (i = 0; i < SLOTS; i++) {
        g_slots[i].data = g_slot_data[i];
    }
    if (vibeos_blockcache_init(&g_bc, &g_dev, g_slots, SLOTS) != 0 ||
        drv->mount(&g_m, &g_bc, FIRST, (uint64_t)size / 512u, 0) != 0) {
        fprintf(stderr, "FAIL:fat_exercise cannot mount what mformat made\n");
        return 1;
    }

    /* What mtools put there: a directory and a file, both with long names. */
    snprintf(g_dirs[g_ndirs++], sizeof(g_dirs[0]), "%s", "made by mtools");
    x_adopt("made by mtools/a file mtools wrote.txt");

    /* Into mtools's own file: the middle of it, and past its end. */
    x_write("made by mtools/a file mtools wrote.txt", 7, 20, 0x11);
    x_write("made by mtools/a file mtools wrote.txt", 9000, 300, 0x22);

    /* Names of every kind, beside it and in the root. */
    x_create("lower.txt");
    x_create("UPPER.TXT");
    x_create("Mixed.Txt");
    x_create("sedAbCdEf");
    x_create("made by mtools/a second long name, by vibeos.data");
    x_write("lower.txt", 0, 5000, 0x33);
    x_write("UPPER.TXT", 100, 10, 0x44);
    x_write("sedAbCdEf", 0, 1, 0x55);
    x_write("made by mtools/a second long name, by vibeos.data", 4090, 12, 0x66);

    /* A directory that grows, and then loses every third name. */
    x_mkdir("Made By VibeOS");
    for (k = 0; k < 30; k++) {
        snprintf(name, sizeof(name), "Made By VibeOS/file number %d in a growing directory", k);
        x_create(name);
        x_write(name, 0, (uint32_t)(k * 97), (uint8_t)k);
    }
    for (k = 0; k < 30; k += 3) {
        snprintf(name, sizeof(name), "Made By VibeOS/file number %d in a growing directory", k);
        x_unlink(name);
    }

    /* A directory filled exactly to the end of its cluster, and then one name
     * more. With room left in the cluster the end-of-directory marker is still
     * there and everything after it is free, so a new cluster's contents do
     * not matter; filled exactly, there is no marker, and the new cluster is
     * the directory. Sixty-two one-slot names beside "." and ".." are the
     * sixty-four a four-sector cluster holds. */
    x_mkdir("exact");
    for (k = 0; k < 62; k++) {
        snprintf(name, sizeof(name), "exact/p%d", k);
        x_create(name);
    }
    x_create("exact/the name that needs another cluster");
    x_write("exact/the name that needs another cluster", 0, 10, 0x12);

    /* Sizes. */
    x_truncate("lower.txt", 1234);
    x_truncate("UPPER.TXT", 7000);
    x_truncate("sedAbCdEf", 5000);   /* grown: one byte, then zeros it never wrote */

    /* Moves: to a long name in another directory, over an existing file, and
     * a directory with what is in it. */
    x_rename("lower.txt", "Made By VibeOS/moved here from the root.txt");
    x_rename("Mixed.Txt", "UPPER.TXT");
    x_mkdir("empty one");
    x_mkdir("Made By VibeOS/nested");
    x_create("Made By VibeOS/nested/deep.txt");
    x_write("Made By VibeOS/nested/deep.txt", 0, 3000, 0x77);
    x_rename("Made By VibeOS/nested", "made by mtools/nested, moved");
    x_rmdir("empty one");

    /* A path through "..": the moved directory's parent is where it is now. */
    {
        vibeos_fs_node_t n;
        if (vibeos_fs_lookup(&g_m, "made by mtools/nested, moved/../a file mtools wrote.txt", &n) != 0) {
            die("a path through '..' of a moved directory", "made by mtools/nested, moved/..", -1);
        }
    }

    /* Run the volume out, and give it back. The write that does not fit must
     * leave nothing free - a writer that stops a few clusters early is one
     * that says "full" with room left - and unlinking the file must return
     * every cluster, which the free count below and the checker both see. */
    {
        static uint8_t chunk[5000];
        vibeos_fs_node_t n;
        uint64_t off = 0;
        long r;

        if (vibeos_fs_create(&g_m, "fill the volume", 0644u, &n) != 0) {
            die("create", "fill the volume", -1);
        }
        memset(chunk, 0x5F, sizeof(chunk));
        for (;;) {
            r = vibeos_fs_write_at(&g_m, &n, off, chunk, sizeof(chunk));
            if (r != (long)sizeof(chunk)) {
                break;
            }
            off += sizeof(chunk);
        }
        if (r < 0 && r != -VIBEOS_ENOSPC) {
            die("filling the volume", "fill the volume", r);
        }
        if (vibeos_fs_statfs(&g_m, &sf) != 0 || sf.blocks_free != 0u) {
            die("the write that ran out left clusters free", "fill the volume", (long)sf.blocks_free);
        }
        if (vibeos_fs_unlink(&g_m, "fill the volume") != 0) {
            die("unlink", "fill the volume", -1);
        }
    }

    if (vibeos_fs_statfs(&g_m, &sf) != 0) {
        die("statfs", "/", -1);
    }

    /* The image back where mtools will read it, and what it should find. */
    f = fopen(argv[1], "wb");
    if (!f || fwrite(g_img + FIRST * 512u, 1, (size_t)size, f) != (size_t)size) {
        return 2;
    }
    fclose(f);
    snprintf(name, sizeof(name), "%s/manifest", argv[2]);
    f = fopen(name, "w");
    for (k = 0; k < g_nwant; k++) {
        char bin[300];
        FILE *b;
        if (!g_want[k].live) {
            continue;
        }
        fprintf(f, "%d %s\n", k, g_want[k].path);
        snprintf(bin, sizeof(bin), "%s/%d.bin", argv[2], k);
        b = fopen(bin, "wb");
        fwrite(g_want[k].data, 1, g_want[k].size, b);
        fclose(b);
    }
    fclose(f);
    snprintf(name, sizeof(name), "%s/dirs", argv[2]);
    f = fopen(name, "w");
    for (k = 0; k < g_ndirs; k++) {
        if (g_dirs[k][0]) {
            fprintf(f, "%s\n", g_dirs[k]);
        }
    }
    fclose(f);
    snprintf(name, sizeof(name), "%s/free", argv[2]);
    f = fopen(name, "w");
    fprintf(f, "%llu\n", (unsigned long long)(sf.blocks_free * sf.block_size));
    fclose(f);
    printf("fat_exercise ok files=%d\n", g_nwant);
    return 0;
}
