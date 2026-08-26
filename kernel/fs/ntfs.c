/* NTFS, read-only.
 *
 * Bounded on purpose. This driver reads files whose data attribute is resident
 * or described by a run list, and directories small enough to live in their
 * index root. A directory large enough to need an index allocation is refused
 * rather than half-read: returning some of a directory's entries is worse than
 * returning none, because a caller cannot tell which case it got.
 */

#include "vibeos/ntfs.h"

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

#define NTFS_ATTR_FILE_NAME 0x30u
#define NTFS_ATTR_DATA      0x80u
#define NTFS_ATTR_INDEX_ROOT 0x90u
#define NTFS_ATTR_END       0xFFFFFFFFu
#define NTFS_MFT_ROOT       5u          /* the root directory's record */
#define NTFS_FLAG_DIRECTORY 0x0002u

static int ntfs_read_sectors(vibeos_ntfs_t *fs, uint64_t sector, uint8_t *out,
                             uint32_t count) {
    uint32_t i;
    for (i = 0; i < count; i++) {
        if (vibeos_blockcache_read(fs->cache, fs->part_lba + sector + i,
                                   out + i * VIBEOS_BLOCK_SIZE) != 0) {
            return -1;
        }
    }
    return 0;
}

/* Undo the update sequence. Every sector of a record has its last two bytes
 * replaced by a sequence number; the originals live in an array at the start.
 * A reader that skips this gets two wrong bytes every 512, which corrupts
 * quietly rather than failing - so the sequence number is also verified, and a
 * record whose sectors do not all carry it is refused as torn. */
static int ntfs_apply_fixups(uint8_t *rec, uint32_t size, uint32_t sector_size) {
    uint16_t usa_off = rd16(rec + 4);
    uint16_t usa_count = rd16(rec + 6);
    uint16_t seq;
    uint32_t i;

    if (usa_count == 0u || usa_off + (uint32_t)usa_count * 2u > size) {
        return -1;
    }
    /* The count includes the sequence number itself, so there is one entry per
     * sector plus one. */
    if ((uint32_t)(usa_count - 1u) * sector_size != size) {
        return -1;
    }
    seq = rd16(rec + usa_off);
    for (i = 1; i < usa_count; i++) {
        uint8_t *tail = rec + i * sector_size - 2u;
        if (rd16(tail) != seq) {
            return -1;   /* this sector was not written with the rest */
        }
        tail[0] = rec[usa_off + i * 2u];
        tail[1] = rec[usa_off + i * 2u + 1u];
    }
    return 0;
}

/* Find an attribute of a given type in a record. Returns a pointer into `rec`
 * or NULL. */
static const uint8_t *ntfs_find_attr(const uint8_t *rec, uint32_t size, uint32_t type) {
    uint32_t off = rd16(rec + 0x14);

    while (off + 8u <= size) {
        uint32_t attr_type = rd32(rec + off);
        uint32_t attr_len;

        if (attr_type == NTFS_ATTR_END) {
            return 0;
        }
        attr_len = rd32(rec + off + 4);
        /* A zero or overlong length would loop forever or read past the
         * record; a corrupt record must end the walk. */
        if (attr_len < 8u || off + attr_len > size) {
            return 0;
        }
        if (attr_type == type) {
            return rec + off;
        }
        off += attr_len;
    }
    return 0;
}

/* Read one master file table record, fixups applied. */
static int ntfs_read_record(vibeos_ntfs_t *fs, uint64_t number, uint8_t *out) {
    uint64_t byte = fs->mft_lcn * fs->cluster_bytes + number * fs->mft_record_bytes;
    uint64_t sector = byte / fs->bytes_per_sector;

    if (fs->mft_record_bytes > VIBEOS_NTFS_MFT_RECORD_MAX) {
        return -1;
    }
    if (ntfs_read_sectors(fs, sector, out,
                          fs->mft_record_bytes / fs->bytes_per_sector) != 0) {
        return -1;
    }
    if (out[0] != 'F' || out[1] != 'I' || out[2] != 'L' || out[3] != 'E') {
        return -1;
    }
    return ntfs_apply_fixups(out, fs->mft_record_bytes, fs->bytes_per_sector);
}

int vibeos_ntfs_mount(vibeos_ntfs_t *fs, vibeos_blockcache_t *cache,
                      uint64_t part_lba) {
    uint8_t sec[VIBEOS_BLOCK_SIZE];
    int8_t clusters_per_record;

    if (!fs || !cache) {
        return -1;
    }
    fs->cache = cache;
    fs->part_lba = part_lba;
    fs->mounted = 0;

    if (vibeos_blockcache_read(cache, part_lba, sec) != 0) {
        return -1;
    }
    if (sec[3] != 'N' || sec[4] != 'T' || sec[5] != 'F' || sec[6] != 'S' ||
        sec[7] != ' ' || sec[8] != ' ' || sec[9] != ' ' || sec[10] != ' ') {
        return -1;
    }
    fs->bytes_per_sector = rd16(sec + 11);
    fs->sectors_per_cluster = sec[13];
    if (fs->bytes_per_sector != VIBEOS_BLOCK_SIZE) {
        return -1;   /* the block cache serves 512-byte sectors */
    }
    if (fs->sectors_per_cluster == 0u || fs->sectors_per_cluster > 8u) {
        return -1;
    }
    fs->cluster_bytes = fs->bytes_per_sector * fs->sectors_per_cluster;
    fs->mft_lcn = rd64(sec + 0x30);

    /* A positive value counts clusters per record; a negative one is a power
     * of two in bytes. Reading it as unsigned gives 246 clusters per record
     * for the ordinary 1 KiB case, which is not a subtle error but does not
     * announce itself either. */
    clusters_per_record = (int8_t)sec[0x40];
    if (clusters_per_record > 0) {
        fs->mft_record_bytes = (uint32_t)clusters_per_record * fs->cluster_bytes;
    } else if (clusters_per_record >= -31 && clusters_per_record <= -9) {
        fs->mft_record_bytes = 1u << (uint32_t)(-clusters_per_record);
    } else {
        return -1;
    }
    if (fs->mft_record_bytes < fs->bytes_per_sector ||
        fs->mft_record_bytes > VIBEOS_NTFS_MFT_RECORD_MAX ||
        (fs->mft_record_bytes % fs->bytes_per_sector) != 0u) {
        return -1;
    }
    fs->mounted = 1;
    return 0;
}

/* A run list entry: a length and a starting cluster, both variable width, with
 * the start stored as a signed delta from the previous run. Sparse runs have
 * no start at all. */
typedef struct {
    uint64_t length;      /* in clusters */
    int64_t start;        /* absolute cluster, or 0 when sparse */
    int sparse;
} ntfs_run_t;

/* Decode the run at `*off`, advancing it. Returns 0 at the end of the list. */
static int ntfs_next_run(const uint8_t *runs, uint32_t len, uint32_t *off,
                         int64_t *prev_start, ntfs_run_t *out) {
    uint8_t header;
    uint32_t len_size, off_size, i;
    uint64_t run_len = 0;
    int64_t run_off = 0;

    if (*off >= len) {
        return 0;
    }
    header = runs[*off];
    if (header == 0u) {
        return 0;   /* the list ends with a zero byte */
    }
    len_size = header & 0x0Fu;
    off_size = (header >> 4) & 0x0Fu;
    if (len_size == 0u || *off + 1u + len_size + off_size > len) {
        return 0;
    }
    for (i = 0; i < len_size; i++) {
        run_len |= (uint64_t)runs[*off + 1u + i] << (8u * i);
    }
    if (off_size == 0u) {
        /* No offset field: the run is sparse, a hole in the file. Treating it
         * as "starts at cluster zero" reads the boot sector into the middle of
         * the file. */
        out->sparse = 1;
        out->start = 0;
    } else {
        for (i = 0; i < off_size; i++) {
            run_off |= (int64_t)runs[*off + 1u + len_size + i] << (8u * i);
        }
        /* The offset is signed and stored in as few bytes as it fits, so the
         * sign bit is the top bit of the last byte, not of a 64-bit word. */
        if (runs[*off + 1u + len_size + off_size - 1u] & 0x80u) {
            run_off |= -((int64_t)1 << (8 * off_size));
        }
        *prev_start += run_off;
        out->sparse = 0;
        out->start = *prev_start;
    }
    out->length = run_len;
    *off += 1u + len_size + off_size;
    return 1;
}

/* Where in the volume a file's cluster `vcn` lives. Returns 0 when the file has
 * no such cluster, and sets `*sparse` when it is a hole. */
static int64_t ntfs_map_vcn(const uint8_t *runs, uint32_t runs_len, uint64_t vcn,
                            int *sparse) {
    uint32_t off = 0;
    int64_t prev = 0;
    uint64_t seen = 0;
    ntfs_run_t run;

    while (ntfs_next_run(runs, runs_len, &off, &prev, &run)) {
        if (vcn < seen + run.length) {
            *sparse = run.sparse;
            return run.sparse ? 0 : (run.start + (int64_t)(vcn - seen));
        }
        seen += run.length;
    }
    return -1;
}

/* ---- name handling --------------------------------------------------------*/

static char ntfs_upper(char c) {
    return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

static int ntfs_name_eq(const uint8_t *utf16, uint32_t chars,
                        const char *want, uint32_t want_len) {
    uint32_t i;
    if (chars != want_len) {
        return 0;
    }
    for (i = 0; i < chars; i++) {
        uint16_t wc = rd16(utf16 + i * 2u);
        if (wc >= 0x80u) {
            return 0;
        }
        if (ntfs_upper((char)wc) != ntfs_upper(want[i])) {
            return 0;
        }
    }
    return 1;
}

/* ---- directories ----------------------------------------------------------*/

/* Walk the index entries in a directory's resident index root. Returns 1 when
 * the wanted name or index was found. */
static int ntfs_index_scan(vibeos_ntfs_t *fs, const uint8_t *rec,
                           const char *want, uint32_t want_len, uint32_t want_index,
                           uint64_t *out_ref, char *out_name, uint32_t name_cap,
                           uint64_t *out_size, int *out_is_dir) {
    const uint8_t *attr = ntfs_find_attr(rec, fs->mft_record_bytes,
                                         NTFS_ATTR_INDEX_ROOT);
    const uint8_t *root, *entry;
    uint32_t value_len, off, seen = 0;

    if (!attr || attr[8] != 0u) {
        return -1;   /* absent, or non-resident: an index allocation */
    }
    value_len = rd32(attr + 0x10);
    root = attr + rd16(attr + 0x14);

    /* The index header sits at offset 16 of the root; entries start at its
     * first-entry offset, relative to the header. */
    off = 16u + rd32(root + 16);
    while (off + 16u <= value_len) {
        uint16_t entry_len;
        uint8_t name_chars;
        uint32_t flags;

        entry = root + off;
        entry_len = rd16(entry + 8);
        flags = rd32(entry + 12);
        if (entry_len < 16u || off + entry_len > value_len) {
            return -1;
        }
        /* Bit 1 marks the end entry, which has no name. */
        if (flags & 0x02u) {
            break;
        }
        name_chars = entry[80];
        if (off + 82u + (uint32_t)name_chars * 2u > value_len) {
            return -1;
        }
        {
            uint64_t ref = rd64(entry) & 0x0000FFFFFFFFFFFFull;
            const uint8_t *name = entry + 82;
            uint64_t fsize = rd64(entry + 64);
            int is_dir = (rd32(entry + 72) & 0x10000000u) ? 1 : 0;

            /* The record for the root directory appears inside itself; every
             * directory also lists a short-name duplicate of each entry, and
             * reporting both would show every file twice. Names in DOS-only
             * namespace are marked in the byte after the length. */
            if (entry[81] == 2u) {
                off += entry_len;
                continue;
            }
            if (want) {
                if (ntfs_name_eq(name, name_chars, want, want_len)) {
                    *out_ref = ref;
                    return 1;
                }
            } else if (seen == want_index) {
                uint32_t i;
                for (i = 0; i + 1u < name_cap && i < name_chars; i++) {
                    uint16_t wc = rd16(name + i * 2u);
                    out_name[i] = (wc < 0x80u) ? (char)wc : '?';
                }
                out_name[i] = 0;
                if (out_size) {
                    *out_size = fsize;
                }
                if (out_is_dir) {
                    *out_is_dir = is_dir;
                }
                *out_ref = ref;
                return 1;
            } else {
                seen++;
            }
        }
        off += entry_len;
    }
    return 0;
}

/* ---- the filesystem interface --------------------------------------------*/

static int ntfs_resolve(vibeos_ntfs_t *fs, const char *path, uint64_t *out_ref,
                        uint8_t *rec) {
    uint64_t ref = NTFS_MFT_ROOT;
    uint32_t i = 0;

    if (ntfs_read_record(fs, ref, rec) != 0) {
        return -1;
    }
    while (path[i] == '/') {
        i++;
    }
    while (path[i]) {
        uint32_t start = i, seg;
        uint64_t next = 0;
        while (path[i] && path[i] != '/') {
            i++;
        }
        seg = i - start;
        if (seg > 0u) {
            if (ntfs_index_scan(fs, rec, path + start, seg, 0, &next, 0, 0, 0, 0) != 1) {
                return -1;
            }
            ref = next;
            if (ntfs_read_record(fs, ref, rec) != 0) {
                return -1;
            }
        }
        while (path[i] == '/') {
            i++;
        }
    }
    *out_ref = ref;
    return 0;
}

static int ntfs_op_lookup(void *fsv, const char *path, vibeos_fs_node_t *out) {
    vibeos_ntfs_t *fs = (vibeos_ntfs_t *)fsv;
    uint8_t rec[VIBEOS_NTFS_MFT_RECORD_MAX];
    uint64_t ref = 0;
    const uint8_t *data;

    if (!fs || !fs->mounted || ntfs_resolve(fs, path, &ref, rec) != 0) {
        return -1;
    }
    out->id = ref;
    out->is_dir = (rd16(rec + 22) & NTFS_FLAG_DIRECTORY) ? 1 : 0;
    out->size = 0;
    data = ntfs_find_attr(rec, fs->mft_record_bytes, NTFS_ATTR_DATA);
    if (data) {
        /* Resident data has its length in the record; non-resident data has a
         * real size field. A directory has no data attribute at all. */
        out->size = (data[8] == 0u) ? rd32(data + 0x10) : rd64(data + 0x30);
    }
    return 0;
}

static long ntfs_op_read_at(void *fsv, const vibeos_fs_node_t *node,
                            uint64_t offset, void *buf, uint32_t len) {
    vibeos_ntfs_t *fs = (vibeos_ntfs_t *)fsv;
    uint8_t rec[VIBEOS_NTFS_MFT_RECORD_MAX];
    uint8_t cluster[VIBEOS_NTFS_MFT_RECORD_MAX];
    uint8_t *dst = (uint8_t *)buf;
    const uint8_t *data;
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
    if (ntfs_read_record(fs, node->id, rec) != 0) {
        return -1;
    }
    data = ntfs_find_attr(rec, fs->mft_record_bytes, NTFS_ATTR_DATA);
    if (!data) {
        return -1;
    }
    if (data[8] == 0u) {
        /* Resident: the file is inside its own record, and there are no
         * clusters to map. A small file on NTFS occupies no space on the
         * volume at all. */
        const uint8_t *value = data + rd16(data + 0x14);
        uint32_t i;
        for (i = 0; i < len; i++) {
            dst[i] = value[offset + i];
        }
        return (long)len;
    }
    {
        const uint8_t *runs = data + rd16(data + 0x20);
        uint32_t runs_len = rd32(data + 4) - rd16(data + 0x20);

        while (done < len) {
            uint64_t vcn = (offset + done) / fs->cluster_bytes;
            uint32_t within = (uint32_t)((offset + done) % fs->cluster_bytes);
            uint32_t chunk = fs->cluster_bytes - within;
            int sparse = 0;
            int64_t lcn;
            uint32_t i;

            if (chunk > len - done) {
                chunk = len - done;
            }
            lcn = ntfs_map_vcn(runs, runs_len, vcn, &sparse);
            if (lcn < 0) {
                return (done > 0u) ? (long)done : -1;
            }
            if (sparse) {
                for (i = 0; i < chunk; i++) {
                    dst[done + i] = 0;
                }
            } else {
                if (ntfs_read_sectors(fs, (uint64_t)lcn * fs->sectors_per_cluster,
                                      cluster, fs->sectors_per_cluster) != 0) {
                    return (done > 0u) ? (long)done : -1;
                }
                for (i = 0; i < chunk; i++) {
                    dst[done + i] = cluster[within + i];
                }
            }
            done += chunk;
        }
    }
    return (long)done;
}

static int ntfs_op_list(void *fsv, const char *path, uint32_t index, char *name,
                        uint32_t name_cap, uint64_t *out_size, int *out_is_dir) {
    vibeos_ntfs_t *fs = (vibeos_ntfs_t *)fsv;
    uint8_t rec[VIBEOS_NTFS_MFT_RECORD_MAX];
    uint64_t ref = 0, entry_ref = 0;

    if (!fs || !fs->mounted || name_cap == 0u) {
        return -1;
    }
    if (ntfs_resolve(fs, path, &ref, rec) != 0) {
        return -1;
    }
    if (!(rd16(rec + 22) & NTFS_FLAG_DIRECTORY)) {
        return -1;
    }
    if (ntfs_index_scan(fs, rec, 0, 0, index, &entry_ref, name, name_cap,
                        out_size, out_is_dir) != 1) {
        return -1;
    }
    return 0;
}

static const vibeos_fs_ops_t g_ntfs_ops = {
    ntfs_op_lookup,
    ntfs_op_read_at,
    0,
    ntfs_op_list,
    0,
    0
};

const vibeos_fs_ops_t *vibeos_ntfs_ops(void) {
    return &g_ntfs_ops;
}
