/* exFAT, read-only.
 *
 * The node identity here is the first cluster plus a contiguity flag, and both
 * are needed to read the file - so unlike ext2 a lookup result is enough on its
 * own, and unlike FAT it is not just a cluster number. That is the third
 * different answer three drivers have given to the same question, which is
 * worth noticing: the interface's node is carrying more meaning each time.
 */

#include "vibeos/exfat.h"

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const uint8_t *p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

#define EXFAT_MAX_CLUSTER_BYTES 4096u
#define EXFAT_ENTRY_FILE   0x85u
#define EXFAT_ENTRY_STREAM 0xC0u
#define EXFAT_ENTRY_NAME   0xC1u
#define EXFAT_ATTR_DIRECTORY 0x0010u
/* Bit 1 of the stream entry's flags: the file occupies consecutive clusters
 * and the allocation table says nothing about it. */
#define EXFAT_FLAG_NO_FAT_CHAIN 0x02u

/* The node id packs the first cluster and the contiguity flag, because reading
 * needs both and the interface gives a node one number. */
#define EXFAT_ID(cluster, contig) (((uint64_t)(cluster)) | ((uint64_t)((contig) ? 1u : 0u) << 32))
#define EXFAT_ID_CLUSTER(id) ((uint32_t)((id) & 0xFFFFFFFFull))
#define EXFAT_ID_CONTIG(id) (((id) >> 32) & 1ull)

static int exfat_read_sector(vibeos_exfat_t *fs, uint64_t sector, uint8_t *out) {
    return vibeos_blockcache_read(fs->cache, fs->part_lba + sector, out);
}

static uint64_t exfat_cluster_sector(vibeos_exfat_t *fs, uint32_t cluster) {
    /* Cluster numbering starts at 2, and the heap starts where the first one
     * would be - so the arithmetic is offset by two, not by zero. */
    return (uint64_t)fs->cluster_heap +
           (uint64_t)(cluster - 2u) * fs->sectors_per_cluster;
}

static int exfat_read_cluster(vibeos_exfat_t *fs, uint32_t cluster, uint8_t *out) {
    uint32_t i;
    if (cluster < 2u || cluster - 2u >= fs->cluster_count) {
        return -1;
    }
    for (i = 0; i < fs->sectors_per_cluster; i++) {
        if (exfat_read_sector(fs, exfat_cluster_sector(fs, cluster) + i,
                              out + i * fs->bytes_per_sector) != 0) {
            return -1;
        }
    }
    return 0;
}

/* The next cluster in a chain, or 0 to stop. */
static uint32_t exfat_next_cluster(vibeos_exfat_t *fs, uint32_t cluster) {
    uint8_t sec[VIBEOS_BLOCK_SIZE];
    uint64_t byte = (uint64_t)cluster * 4u;
    uint32_t next;

    if (exfat_read_sector(fs, fs->fat_offset + byte / fs->bytes_per_sector, sec) != 0) {
        return 0;
    }
    next = rd32(sec + (byte % fs->bytes_per_sector));
    /* Anything at or above 0xFFFFFFF7 is a marker, not a cluster, and anything
     * below 2 is not one either.
     *
     * No test turns red when this is removed, and that is worth saying rather
     * than leaving to be rediscovered: exfat_read_cluster already rejects both
     * ranges, so the effect is identical either way. It is kept because the two
     * checks answer different questions - this one says "the chain ended", that
     * one says "that is not a readable cluster" - and a reader who deletes this
     * has to work out that the other one exists. Defence in depth, labelled as
     * such instead of masquerading as load-bearing. */
    if (next >= 0xFFFFFFF7u || next < 2u) {
        return 0;
    }
    return next;
}

/* The cluster holding a file's block `index`. */
static uint32_t exfat_nth_cluster(vibeos_exfat_t *fs, uint32_t first, int contiguous,
                                  uint32_t index) {
    uint32_t c = first;
    uint32_t i;

    if (contiguous) {
        return first + index;
    }
    for (i = 0; i < index; i++) {
        c = exfat_next_cluster(fs, c);
        if (c == 0u) {
            return 0;
        }
    }
    return c;
}

int vibeos_exfat_mount(vibeos_exfat_t *fs, vibeos_blockcache_t *cache,
                       uint64_t part_lba) {
    uint8_t sec[VIBEOS_BLOCK_SIZE];
    uint8_t bps_shift, spc_shift;

    if (!fs || !cache) {
        return -1;
    }
    fs->cache = cache;
    fs->part_lba = part_lba;
    fs->mounted = 0;

    if (vibeos_blockcache_read(cache, part_lba, sec) != 0) {
        return -1;
    }
    if (sec[3] != 'E' || sec[4] != 'X' || sec[5] != 'F' || sec[6] != 'A' ||
        sec[7] != 'T' || sec[8] != ' ' || sec[9] != ' ' || sec[10] != ' ') {
        return -1;
    }
    bps_shift = sec[108];
    spc_shift = sec[109];
    /* This driver reads through a 512-byte block cache, so a volume with a
     * different sector size would need the cache to agree. Refusing is honest;
     * pretending would read at the wrong offsets from the first cluster on. */
    if (bps_shift != 9u) {
        return -1;
    }
    if (spc_shift > 3u) {
        return -1;   /* clusters above EXFAT_MAX_CLUSTER_BYTES */
    }
    fs->bytes_per_sector = 1u << bps_shift;
    fs->sectors_per_cluster = 1u << spc_shift;
    fs->cluster_bytes = fs->bytes_per_sector * fs->sectors_per_cluster;
    fs->fat_offset = rd32(sec + 80);
    fs->cluster_heap = rd32(sec + 88);
    fs->cluster_count = rd32(sec + 92);
    fs->root_cluster = rd32(sec + 96);
    if (fs->cluster_count == 0u || fs->root_cluster < 2u) {
        return -1;
    }
    fs->mounted = 1;
    return 0;
}

static char exfat_upper(char c) {
    return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

/* One directory entry set, decoded. */
typedef struct {
    uint32_t first_cluster;
    uint64_t size;
    int is_dir;
    int contiguous;
    char name[VIBEOS_FS_NAME_MAX + 1];
} exfat_dirent_t;

/* Walk a directory's clusters, decoding entry sets. Returns 1 when the entry
 * at `want_index` was decoded, or when `want_name` matched. */
static int exfat_dir_scan(vibeos_exfat_t *fs, uint32_t dir_cluster, int dir_contig,
                          const char *want_name, uint32_t want_name_len,
                          uint32_t want_index, exfat_dirent_t *out) {
    uint8_t cluster[EXFAT_MAX_CLUSTER_BYTES];
    uint32_t ci = 0, seen = 0;

    for (;;) {
        uint32_t c = exfat_nth_cluster(fs, dir_cluster, dir_contig, ci);
        uint32_t off;

        if (c == 0u || exfat_read_cluster(fs, c, cluster) != 0) {
            return 0;
        }
        for (off = 0; off + 32u <= fs->cluster_bytes; off += 32u) {
            const uint8_t *e = cluster + off;
            uint8_t type = e[0];
            uint8_t secondary;
            uint32_t name_pos = 0;
            uint32_t k;

            if (type == 0x00u) {
                return 0;   /* end of the directory */
            }
            if (type != EXFAT_ENTRY_FILE) {
                continue;   /* deleted, or a bitmap/upcase entry */
            }
            /* A file is a set: this entry, a stream entry, and name entries.
             * They must be read together or the file has no name and no size. */
            secondary = e[1];
            if (secondary < 1u || off + 32u * (secondary + 1u) > fs->cluster_bytes) {
                /* A set straddling the end of a cluster is legal exFAT; this
                 * driver does not handle it, and stopping is better than
                 * decoding half a set into a plausible wrong file. */
                return 0;
            }
            {
                const uint8_t *stream = cluster + off + 32u;
                uint16_t attrs = rd16(e + 4);
                uint8_t name_len;

                if (stream[0] != EXFAT_ENTRY_STREAM) {
                    continue;
                }
                name_len = stream[3];
                out->is_dir = (attrs & EXFAT_ATTR_DIRECTORY) ? 1 : 0;
                out->contiguous = (stream[1] & EXFAT_FLAG_NO_FAT_CHAIN) ? 1 : 0;
                out->first_cluster = rd32(stream + 20);
                out->size = rd64(stream + 24);

                for (k = 1; k <= secondary && name_pos < VIBEOS_FS_NAME_MAX; k++) {
                    const uint8_t *ne = cluster + off + 32u * (k + 1u);
                    uint32_t j;
                    if (ne[0] != EXFAT_ENTRY_NAME) {
                        continue;
                    }
                    for (j = 0; j < 15u && name_pos < name_len &&
                                name_pos < VIBEOS_FS_NAME_MAX; j++) {
                        uint16_t wc = rd16(ne + 2u + j * 2u);
                        /* Names are UTF-16. Anything outside ASCII becomes a
                         * marker rather than being truncated at the first wide
                         * character: half a name is worse than a marked one. */
                        out->name[name_pos++] = (wc < 0x80u) ? (char)wc : '?';
                    }
                }
                out->name[name_pos] = 0;
            }
            off += 32u * secondary;   /* skip the rest of the set */

            if (want_name) {
                uint32_t i;
                if (name_pos != want_name_len) {
                    goto next;
                }
                for (i = 0; i < name_pos; i++) {
                    if (exfat_upper(out->name[i]) != exfat_upper(want_name[i])) {
                        goto next;
                    }
                }
                return 1;
            } else if (seen == want_index) {
                return 1;
            }
            seen++;
        next:
            continue;
        }
        ci++;
    }
}

static int exfat_resolve(vibeos_exfat_t *fs, const char *path, exfat_dirent_t *out) {
    uint32_t cluster = fs->root_cluster;
    int contig = 0;   /* the root's chain is in the table unless said otherwise */
    uint32_t i = 0;
    int is_dir = 1;

    out->first_cluster = cluster;
    out->size = 0;
    out->is_dir = 1;
    out->contiguous = 0;
    out->name[0] = 0;

    while (path[i] == '/') {
        i++;
    }
    while (path[i]) {
        uint32_t start = i, seg;
        while (path[i] && path[i] != '/') {
            i++;
        }
        seg = i - start;
        if (seg > 0u) {
            if (!is_dir) {
                return -1;
            }
            if (exfat_dir_scan(fs, cluster, contig, path + start, seg, 0, out) != 1) {
                return -1;
            }
            cluster = out->first_cluster;
            contig = out->contiguous;
            is_dir = out->is_dir;
        }
        while (path[i] == '/') {
            i++;
        }
    }
    out->first_cluster = cluster;
    out->contiguous = contig;
    out->is_dir = is_dir;
    return 0;
}

/* ---- the filesystem interface --------------------------------------------*/

static int exfat_op_lookup(void *fsv, const char *path, vibeos_fs_node_t *out) {
    vibeos_exfat_t *fs = (vibeos_exfat_t *)fsv;
    exfat_dirent_t ent;

    if (!fs || !fs->mounted || exfat_resolve(fs, path, &ent) != 0) {
        return -1;
    }
    out->id = EXFAT_ID(ent.first_cluster, ent.contiguous);
    out->size = ent.size;
    out->is_dir = ent.is_dir;
    return 0;
}

static long exfat_op_read_at(void *fsv, const vibeos_fs_node_t *node,
                             uint64_t offset, void *buf, uint32_t len) {
    vibeos_exfat_t *fs = (vibeos_exfat_t *)fsv;
    uint8_t cluster[EXFAT_MAX_CLUSTER_BYTES];
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
        uint32_t index = (uint32_t)((offset + done) / fs->cluster_bytes);
        uint32_t within = (uint32_t)((offset + done) % fs->cluster_bytes);
        uint32_t chunk = fs->cluster_bytes - within;
        uint32_t c, i;

        if (chunk > len - done) {
            chunk = len - done;
        }
        c = exfat_nth_cluster(fs, EXFAT_ID_CLUSTER(node->id),
                              (int)EXFAT_ID_CONTIG(node->id), index);
        if (c == 0u || exfat_read_cluster(fs, c, cluster) != 0) {
            return (done > 0u) ? (long)done : -1;
        }
        for (i = 0; i < chunk; i++) {
            dst[done + i] = cluster[within + i];
        }
        done += chunk;
    }
    return (long)done;
}

static int exfat_op_list(void *fsv, const char *path, uint32_t index, char *name,
                         uint32_t name_cap, uint64_t *out_size, int *out_is_dir) {
    vibeos_exfat_t *fs = (vibeos_exfat_t *)fsv;
    exfat_dirent_t dir, ent;
    uint32_t i;

    if (!fs || !fs->mounted || name_cap == 0u) {
        return -1;
    }
    if (exfat_resolve(fs, path, &dir) != 0 || !dir.is_dir) {
        return -1;
    }
    if (exfat_dir_scan(fs, dir.first_cluster, dir.contiguous, 0, 0, index, &ent) != 1) {
        return -1;
    }
    for (i = 0; i + 1u < name_cap && ent.name[i]; i++) {
        name[i] = ent.name[i];
    }
    name[i] = 0;
    if (out_size) {
        *out_size = ent.size;
    }
    if (out_is_dir) {
        *out_is_dir = ent.is_dir;
    }
    return 0;
}

static const vibeos_fs_ops_t g_exfat_ops = {
    exfat_op_lookup,
    exfat_op_read_at,
    0,
    exfat_op_list,
    0,
    0
};

const vibeos_fs_ops_t *vibeos_exfat_ops(void) {
    return &g_exfat_ops;
}
