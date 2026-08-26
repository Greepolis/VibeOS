/* ISO9660, read-only.
 *
 * Files are contiguous, so the interesting code here is name handling rather
 * than block mapping - which is a pleasant change after ext2, and is why this
 * driver is a fraction of the size.
 */

#include "vibeos/iso9660.h"

static uint32_t rd32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* One 2048-byte logical sector, assembled from the 512-byte sectors under it. */
static int iso_read_sector(vibeos_iso9660_t *fs, uint32_t lb, uint8_t *out) {
    uint32_t i;
    for (i = 0; i < VIBEOS_ISO_SECTOR / VIBEOS_BLOCK_SIZE; i++) {
        uint64_t lba = fs->part_lba + (uint64_t)lb * (VIBEOS_ISO_SECTOR / VIBEOS_BLOCK_SIZE) + i;
        if (vibeos_blockcache_read(fs->cache, lba, out + i * VIBEOS_BLOCK_SIZE) != 0) {
            return -1;
        }
    }
    return 0;
}

int vibeos_iso9660_mount(vibeos_iso9660_t *fs, vibeos_blockcache_t *cache,
                         uint64_t part_lba) {
    uint8_t sec[VIBEOS_ISO_SECTOR];

    if (!fs || !cache) {
        return -1;
    }
    fs->cache = cache;
    fs->part_lba = part_lba;
    fs->mounted = 0;

    if (iso_read_sector(fs, VIBEOS_ISO_PVD_SECTOR, sec) != 0) {
        return -1;
    }
    /* Type 1 is the primary volume descriptor; "CD001" is what makes this an
     * ISO9660 volume rather than a disc that happens to have bytes there. */
    if (sec[0] != 1u || sec[1] != 'C' || sec[2] != 'D' || sec[3] != '0' ||
        sec[4] != '0' || sec[5] != '1') {
        return -1;
    }
    /* Numbers are stored twice, little-endian then big-endian. Reading the
     * little-endian half is correct and reading the pair as one number is the
     * classic way to get an enormous wrong answer. */
    if (rd32le(sec + 128) != VIBEOS_ISO_SECTOR) {
        return -1;   /* a logical block size this driver does not handle */
    }
    /* The root directory record sits inside the descriptor at offset 156. */
    fs->root_extent = rd32le(sec + 156 + 2);
    fs->root_length = rd32le(sec + 156 + 10);
    if (fs->root_length == 0u) {
        return -1;
    }
    fs->mounted = 1;
    return 0;
}

static char iso_upper(char c) {
    return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

/* The stored length of a name once the version suffix is dropped. ";1" is
 * present on files and absent on directories, and a caller that saw it would
 * have to strip it before passing the name to open(). */
static uint32_t iso_visible_len(const uint8_t *name, uint32_t len) {
    uint32_t i;
    for (i = 0; i < len; i++) {
        if (name[i] == ';') {
            return i;
        }
    }
    /* A trailing dot is how a name with no extension is stored. */
    if (len > 0u && name[len - 1u] == '.') {
        return len - 1u;
    }
    return len;
}

static int iso_name_eq(const uint8_t *stored, uint32_t stored_len,
                       const char *want, uint32_t want_len) {
    uint32_t vis = iso_visible_len(stored, stored_len);
    uint32_t i;

    if (vis != want_len) {
        return 0;
    }
    for (i = 0; i < vis; i++) {
        if (iso_upper((char)stored[i]) != iso_upper(want[i])) {
            return 0;
        }
    }
    return 1;
}

/* Walk one directory extent, handing each record to the caller's decision.
 * Returns 1 when `want` was found and fills the outputs. */
static int iso_dir_find(vibeos_iso9660_t *fs, uint32_t extent, uint32_t length,
                        const char *want, uint32_t want_len,
                        uint32_t *out_extent, uint32_t *out_len, int *out_is_dir) {
    uint8_t sec[VIBEOS_ISO_SECTOR];
    uint32_t sectors = (length + VIBEOS_ISO_SECTOR - 1u) / VIBEOS_ISO_SECTOR;
    uint32_t s;

    for (s = 0; s < sectors; s++) {
        uint32_t off = 0;
        if (iso_read_sector(fs, extent + s, sec) != 0) {
            return -1;
        }
        while (off < VIBEOS_ISO_SECTOR) {
            uint8_t rec_len = sec[off];
            uint8_t name_len;

            /* A zero length means "no more records in this sector"; records
             * never straddle a sector boundary, which is what makes that
             * padding legal rather than corrupt. */
            if (rec_len == 0u) {
                break;
            }
            if (off + rec_len > VIBEOS_ISO_SECTOR || rec_len < 33u) {
                break;   /* corrupt: end the scan rather than walk out */
            }
            name_len = sec[off + 32];
            if (off + 33u + name_len > VIBEOS_ISO_SECTOR) {
                break;
            }
            /* The first two records of every directory are "." and "..",
             * stored as single bytes 0x00 and 0x01 rather than as text. */
            if (name_len == 1u && (sec[off + 33] == 0u || sec[off + 33] == 1u)) {
                off += rec_len;
                continue;
            }
            if (iso_name_eq(sec + off + 33, name_len, want, want_len)) {
                *out_extent = rd32le(sec + off + 2);
                *out_len = rd32le(sec + off + 10);
                *out_is_dir = (sec[off + 25] & 0x02u) ? 1 : 0;
                return 1;
            }
            off += rec_len;
        }
    }
    return 0;
}

static int iso_resolve(vibeos_iso9660_t *fs, const char *path,
                       uint32_t *out_extent, uint32_t *out_len, int *out_is_dir) {
    uint32_t extent = fs->root_extent;
    uint32_t length = fs->root_length;
    int is_dir = 1;
    uint32_t i = 0;

    while (path[i] == '/') {
        i++;
    }
    while (path[i]) {
        uint32_t start = i;
        uint32_t seg;
        while (path[i] && path[i] != '/') {
            i++;
        }
        seg = i - start;
        if (seg > 0u) {
            uint32_t ne = 0, nl = 0;
            int nd = 0;
            if (!is_dir) {
                return -1;   /* a component below something that is not a directory */
            }
            if (iso_dir_find(fs, extent, length, path + start, seg, &ne, &nl, &nd) != 1) {
                return -1;
            }
            extent = ne;
            length = nl;
            is_dir = nd;
        }
        while (path[i] == '/') {
            i++;
        }
    }
    *out_extent = extent;
    *out_len = length;
    *out_is_dir = is_dir;
    return 0;
}

/* ---- the filesystem interface --------------------------------------------*/

static int iso_op_lookup(void *fsv, const char *path, vibeos_fs_node_t *out) {
    vibeos_iso9660_t *fs = (vibeos_iso9660_t *)fsv;
    uint32_t extent = 0, length = 0;
    int is_dir = 0;

    if (!fs || !fs->mounted) {
        return -1;
    }
    if (iso_resolve(fs, path, &extent, &length, &is_dir) != 0) {
        return -1;
    }
    /* The extent is enough to read the file, so unlike ext2 no second lookup
     * is needed later - the node identity carries everything. */
    out->id = extent;
    out->size = length;
    out->is_dir = is_dir;
    return 0;
}

static long iso_op_read_at(void *fsv, const vibeos_fs_node_t *node,
                           uint64_t offset, void *buf, uint32_t len) {
    vibeos_iso9660_t *fs = (vibeos_iso9660_t *)fsv;
    uint8_t sec[VIBEOS_ISO_SECTOR];
    uint8_t *dst = (uint8_t *)buf;
    uint32_t done = 0;

    if (!fs || !fs->mounted || node->is_dir) {
        return -1;
    }
    if (offset >= node->size) {
        return 0;
    }
    if (offset + len > node->size) {
        len = (uint32_t)(node->size - offset);
    }
    while (done < len) {
        uint32_t lb = (uint32_t)(node->id + (offset + done) / VIBEOS_ISO_SECTOR);
        uint32_t within = (uint32_t)((offset + done) % VIBEOS_ISO_SECTOR);
        uint32_t chunk = VIBEOS_ISO_SECTOR - within;
        uint32_t i;

        if (chunk > len - done) {
            chunk = len - done;
        }
        if (iso_read_sector(fs, lb, sec) != 0) {
            return (done > 0u) ? (long)done : -1;
        }
        for (i = 0; i < chunk; i++) {
            dst[done + i] = sec[within + i];
        }
        done += chunk;
    }
    return (long)done;
}

static int iso_op_list(void *fsv, const char *path, uint32_t index, char *name,
                       uint32_t name_cap, uint64_t *out_size, int *out_is_dir) {
    vibeos_iso9660_t *fs = (vibeos_iso9660_t *)fsv;
    uint8_t sec[VIBEOS_ISO_SECTOR];
    uint32_t extent = 0, length = 0, sectors, s, seen = 0;
    int is_dir = 0;

    if (!fs || !fs->mounted || name_cap == 0u) {
        return -1;
    }
    if (iso_resolve(fs, path, &extent, &length, &is_dir) != 0 || !is_dir) {
        return -1;
    }
    sectors = (length + VIBEOS_ISO_SECTOR - 1u) / VIBEOS_ISO_SECTOR;
    for (s = 0; s < sectors; s++) {
        uint32_t off = 0;
        if (iso_read_sector(fs, extent + s, sec) != 0) {
            return -1;
        }
        while (off < VIBEOS_ISO_SECTOR) {
            uint8_t rec_len = sec[off];
            uint8_t name_len;

            if (rec_len == 0u) {
                break;
            }
            if (off + rec_len > VIBEOS_ISO_SECTOR || rec_len < 33u) {
                break;
            }
            name_len = sec[off + 32];
            if (off + 33u + name_len > VIBEOS_ISO_SECTOR) {
                break;
            }
            if (name_len == 1u && (sec[off + 33] == 0u || sec[off + 33] == 1u)) {
                off += rec_len;
                continue;
            }
            if (seen == index) {
                uint32_t vis = iso_visible_len(sec + off + 33, name_len);
                uint32_t i;
                for (i = 0; i + 1u < name_cap && i < vis; i++) {
                    name[i] = (char)sec[off + 33 + i];
                }
                name[i] = 0;
                if (out_size) {
                    *out_size = rd32le(sec + off + 10);
                }
                if (out_is_dir) {
                    *out_is_dir = (sec[off + 25] & 0x02u) ? 1 : 0;
                }
                return 0;
            }
            seen++;
            off += rec_len;
        }
    }
    return -1;
}

static const vibeos_fs_ops_t g_iso_ops = {
    iso_op_lookup,
    iso_op_read_at,
    0,   /* write_file: a disc is read-only, and the wrapper says so */
    iso_op_list,
    0,
    0
};

const vibeos_fs_ops_t *vibeos_iso9660_ops(void) {
    return &g_iso_ops;
}
