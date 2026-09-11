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
#define NTFS_ATTR_INDEX_ALLOCATION 0xA0u
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
/* A $DATA attribute, after its own internal fields have been checked.
 *
 * `ntfs_find_attr` below bounds an attribute's *total* length inside the MFT
 * record, and nothing more. Every field inside the attribute - where the
 * resident value starts, how long it is, where the run list starts - came off
 * the volume and was used as it arrived.
 *
 * That is a hole a USB stick walks through. `kernel/fs/storage.c` tries NTFS
 * during filesystem recognition, so simply plugging in a crafted image reaches
 * this code with no privilege at all, and the resident read path copies from
 * `rec` - a four-kilobyte buffer on the *kernel stack* - straight into the
 * buffer a user `read()` supplied. A resident length the volume chose therefore
 * chose how much kernel stack to hand to a user process.
 *
 * The non-resident path had an unsigned subtraction on top: run-list offset
 * beyond the attribute length wrapped to a near-4-gigabyte run list.
 *
 * So the fields are extracted once, here, or the attribute is refused. A
 * refused attribute is a file that cannot be read; an unrefused one was a
 * kernel memory disclosure. */
typedef struct {
    int resident;
    uint64_t size;            /* the file's length as the volume states it */
    const uint8_t *value;     /* resident bytes, valid for value_len       */
    uint32_t value_len;
    const uint8_t *runs;      /* non-resident run list, valid for runs_len */
    uint32_t runs_len;
} ntfs_data_attr_t;

/* `attr` must have come from ntfs_find_attr against the same `rec`/`size`, so
 * its total length is already known to fit. Returns 0 on success. */
static int ntfs_data_attr(const uint8_t *rec, uint32_t size,
                          const uint8_t *attr, ntfs_data_attr_t *out) {
    uint32_t attr_len;
    uint32_t at;

    if (!rec || !attr || !out || attr < rec) {
        return -1;
    }
    at = (uint32_t)(attr - rec);
    /* Every bound here is `length > size - offset` once `offset <= size` is
     * known, never `offset + length > size`: the sum is 32-bit, and an
     * attribute length from the volume wraps it (H-011). A length of
     * 0xFFFFFFF0 at offset 64 summed to 48 and passed, and the resident value
     * checks below then trusted it - a value longer than the record was
     * accepted, and read_at copied the bytes after the record, which is a
     * local array on the kernel stack, to user space. */
    if (at > size || size - at < 8u) {
        return -1;
    }
    attr_len = rd32(attr + 4);
    if (attr_len < 8u || attr_len > size - at) {
        return -1;   /* find_attr promised this; not trusting it costs nothing */
    }

    out->resident = (attr[8] == 0u) ? 1 : 0;
    out->value = 0;
    out->value_len = 0;
    out->runs = 0;
    out->runs_len = 0;
    out->size = 0;

    if (out->resident) {
        uint32_t value_off, value_len;

        /* A resident attribute's header is 0x18 bytes: below that, the offset
         * and length fields being read are not part of the attribute. */
        if (attr_len < 0x18u) {
            return -1;
        }
        value_len = rd32(attr + 0x10);
        value_off = rd16(attr + 0x14);
        if (value_off < 0x18u || value_off > attr_len) {
            return -1;
        }
        if (value_len > attr_len - value_off) {
            return -1;   /* the value claims to run past its own attribute */
        }
        out->value = attr + value_off;
        out->value_len = value_len;
        out->size = value_len;
        return 0;
    }

    {
        uint32_t runs_off;

        /* A non-resident header is 0x40 bytes, and the real size lives at
         * 0x30 - so reading it at all requires the attribute to be that long.
         * It was read unconditionally before, which is an eight-byte read past
         * a short attribute and into whatever follows it in the record. */
        if (attr_len < 0x40u) {
            return -1;
        }
        runs_off = rd16(attr + 0x20);
        if (runs_off < 0x40u || runs_off >= attr_len) {
            /* The subtraction below was unsigned and unguarded: an offset past
             * the attribute produced a run list of nearly four gigabytes. */
            return -1;
        }
        out->runs = attr + runs_off;
        out->runs_len = attr_len - runs_off;
        out->size = rd64(attr + 0x30);
        return 0;
    }
}

static const uint8_t *ntfs_find_attr(const uint8_t *rec, uint32_t size, uint32_t type) {
    uint32_t off = rd16(rec + 0x14);

    while (off <= size && size - off >= 8u) {
        uint32_t attr_type = rd32(rec + off);
        uint32_t attr_len;

        if (attr_type == NTFS_ATTR_END) {
            return 0;
        }
        attr_len = rd32(rec + off + 4);
        /* A zero or overlong length would loop forever or read past the
         * record; a corrupt record must end the walk. */
        if (attr_len < 8u || attr_len > size - off) {   /* not off + attr_len: it wraps (H-011) */
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
    uint64_t run_off = 0;

    if (*off >= len) {
        return 0;
    }
    header = runs[*off];
    if (header == 0u) {
        return 0;   /* the list ends with a zero byte */
    }
    len_size = header & 0x0Fu;
    off_size = (header >> 4) & 0x0Fu;
    /* Each field is at most eight bytes: a 64-bit value holds no more, and the
     * header nibbles allow fifteen. Shifting a ninth byte by 8 * i is past the
     * width of the type, which is undefined (M-010). */
    if (len_size == 0u || len_size > 8u || off_size > 8u ||
        *off + 1u + len_size + off_size > len) {
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
            run_off |= (uint64_t)runs[*off + 1u + len_size + i] << (8u * i);
        }
        /* The offset is signed and stored in as few bytes as it fits, so the
         * sign bit is the top bit of the last byte, not of a 64-bit word.
         * Extended in unsigned arithmetic: `(int64_t)1 << 64` at the largest
         * legal size was undefined, and so was shifting a byte into the sign
         * bit of an int64_t (M-010). An eight-byte field needs no extension. */
        if (off_size < 8u && (runs[*off + 1u + len_size + off_size - 1u] & 0x80u)) {
            run_off |= ~0ull << (8u * off_size);
        }
        /* The running start, also unsigned: a signed sum of volume-supplied
         * deltas can overflow, and a signed overflow is undefined. */
        *prev_start = (int64_t)((uint64_t)*prev_start + run_off);
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
        /* vcn >= seen here, so the difference is exact; `vcn < seen + length`
         * wrapped on a crafted length (the review's second NTFS candidate). */
        if (vcn - seen < run.length) {
            *sparse = run.sparse;
            return run.sparse ? 0 : (int64_t)((uint64_t)run.start + (vcn - seen));
        }
        if (run.length > ~0ull - seen) {
            return -1;   /* a run list longer than any volume */
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

/* Walk one node of a directory index.
 *
 * `base` is the index header; entries start at its first-entry offset,
 * measured from the header. That is true of the resident index root and of an
 * INDX block alike, which is why this is a function rather than two copies of
 * a loop.
 *
 * Returns 1 when found, 0 when this node did not contain it, -1 when the node
 * is malformed. `seen` is carried across nodes so that listing by index runs
 * over a whole directory rather than restarting in each node.
 */
static int ntfs_index_walk(const uint8_t *base, uint32_t value_len,
                           const char *want, uint32_t want_len,
                           uint32_t want_index, uint32_t *seen,
                           uint64_t *out_ref, char *out_name, uint32_t name_cap,
                           uint64_t *out_size, int *out_is_dir) {
    uint32_t off;

    if (value_len < 16u) {
        return -1;
    }
    /* From the header, not from the attribute value. The first version of this
     * refactor kept the `16u +` that belonged to the old root-relative form
     * and then read the first-entry offset from base + 16 as well - wrong
     * twice, and the existing host test said so on the first run. Which is the
     * argument for having had it. */
    off = rd32(base);
    {
        uint32_t used = rd32(base + 4);
        if (used >= 16u && used <= value_len) {
            value_len = used;   /* the header knows better than the caller */
        }
    }
    while (off + 16u <= value_len) {
        const uint8_t *entry = base + off;
        uint16_t entry_len = rd16(entry + 8);
        uint32_t flags = rd32(entry + 12);
        uint8_t name_chars;

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

            /* Every directory also lists a short-name duplicate of each entry,
             * and reporting both would show every file twice. Names in the
             * DOS-only namespace are marked in the byte after the length. */
            if (entry[81] == 2u) {
                off += entry_len;
                continue;
            }
            if (want) {
                if (ntfs_name_eq(name, name_chars, want, want_len)) {
                    *out_ref = ref;
                    return 1;
                }
            } else if (*seen == want_index) {
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
                (*seen)++;
            }
        }
        off += entry_len;
    }
    return 0;
}

/* How many index blocks one directory may have.
 *
 * A bound rather than trust: a run list that loops, or one this reader has
 * misparsed, would otherwise walk forever inside a filesystem call. 4096
 * blocks of 4 KiB is a 16 MiB directory, far past anything this driver is
 * asked to open.
 */
#define NTFS_MAX_INDEX_BLOCKS 4096u

/* Walk a directory index: the root first, then its allocation.
 *
 * The allocation half is why this driver could not read a single real NTFS
 * volume. It refused any directory with an $INDEX_ALLOCATION, which sounds
 * like a large-directory limitation and is not: mkntfs gives the *root*
 * directory one, because a fresh volume already holds sixteen metadata files.
 * So every volume made by anything other than this project failed at the first
 * lookup, and the synthetic image in the host test was the only shape it could
 * read. Fifth time this phase has found code that worked only in the one
 * arrangement it was written against.
 *
 * The allocation is scanned linearly rather than descended as the B-tree it
 * is. That is correct - every entry appears in exactly one node - and costs a
 * read per block on a directory this driver opens once. Descending needs the
 * collation rules, which is a lot of code to make a bounded scan faster.
 */
static int ntfs_index_scan(vibeos_ntfs_t *fs, const uint8_t *rec,
                           const char *want, uint32_t want_len,
                           uint32_t want_index,
                           uint64_t *out_ref, char *out_name, uint32_t name_cap,
                           uint64_t *out_size, int *out_is_dir) {
    const uint8_t *attr = ntfs_find_attr(rec, fs->mft_record_bytes,
                                         NTFS_ATTR_INDEX_ROOT);
    const uint8_t *root;
    const uint8_t *alloc;
    uint32_t value_len;
    uint32_t seen = 0;
    uint32_t block_bytes;
    int r;

    if (!attr || attr[8] != 0u) {
        return -1;   /* absent, or non-resident: not a directory this reads */
    }
    value_len = rd32(attr + 0x10);
    root = attr + rd16(attr + 0x14);
    if (value_len < 16u) {
        return -1;
    }

    /* The index block size is in the root value, ahead of the header. */
    block_bytes = rd32(root + 8);

    r = ntfs_index_walk(root + 16, value_len - 16u, want, want_len, want_index,
                        &seen, out_ref, out_name, name_cap, out_size,
                        out_is_dir);
    if (r != 0) {
        return r;
    }

    alloc = ntfs_find_attr(rec, fs->mft_record_bytes,
                           NTFS_ATTR_INDEX_ALLOCATION);
    if (!alloc || alloc[8] == 0u) {
        return 0;    /* absent, or resident, which an allocation never is */
    }
    if (block_bytes == 0u || block_bytes > VIBEOS_NTFS_INDEX_BLOCK_MAX ||
        (block_bytes % fs->bytes_per_sector) != 0u) {
        return -1;
    }

    {
        uint16_t runs_off = rd16(alloc + 0x20);
        const uint8_t *runs = alloc + runs_off;
        uint32_t attr_len = rd32(alloc + 4);
        uint32_t runs_len;
        uint64_t clusters_per_block;
        uint32_t blk;

        if (attr_len <= runs_off) {
            return -1;
        }
        runs_len = attr_len - runs_off;
        clusters_per_block = block_bytes / fs->cluster_bytes;
        if (clusters_per_block == 0ull) {
            clusters_per_block = 1ull;   /* a block smaller than a cluster */
        }
        for (blk = 0; blk < NTFS_MAX_INDEX_BLOCKS; blk++) {
            uint64_t vcn = (uint64_t)blk * clusters_per_block;
            int sparse = 0;
            int64_t lcn = ntfs_map_vcn(runs, runs_len, vcn, &sparse);

            if (lcn < 0) {
                break;              /* past the end of the allocation */
            }
            if (sparse) {
                continue;
            }
            if (ntfs_read_sectors(fs,
                                  (uint64_t)lcn * fs->sectors_per_cluster,
                                  fs->index_block,
                                  block_bytes / fs->bytes_per_sector) != 0) {
                return -1;
            }
            if (fs->index_block[0] != 'I' || fs->index_block[1] != 'N' ||
                fs->index_block[2] != 'D' || fs->index_block[3] != 'X') {
                continue;           /* an unused block in the allocation */
            }
            /* An INDX block carries fixups exactly as an MFT record does, and
             * a reader that skips them gets two wrong bytes per sector - which
             * here land in the middle of entry lengths. */
            if (ntfs_apply_fixups(fs->index_block, block_bytes,
                                  fs->bytes_per_sector) != 0) {
                return -1;
            }
            /* The header sits at offset 24 of the block, and the walker
             * measures everything from the header. */
            r = ntfs_index_walk(fs->index_block + 24, block_bytes - 24u,
                                want, want_len, want_index, &seen, out_ref,
                                out_name, name_cap, out_size, out_is_dir);
            if (r != 0) {
                return r;
            }
        }
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
        ntfs_data_attr_t d;

        /* A directory has no data attribute at all, and an attribute whose own
         * fields do not fit inside it is a file with no readable length rather
         * than a length to believe. */
        if (ntfs_data_attr(rec, fs->mft_record_bytes, data, &d) == 0) {
            out->size = d.size;
        }
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
    ntfs_data_attr_t attr;
    uint32_t done = 0;

    if (!fs || !fs->mounted || node->is_dir) {
        return -1;
    }
    if (offset >= node->size) {
        return 0;
    }
    /* Not offset + len > size: offset is below size here, but a size near
     * 2^64 from the volume makes the sum wrap and the length stay untrimmed
     * (found by the H-011 audit). The difference cannot wrap. */
    if (len > node->size - offset) {
        len = (uint32_t)(node->size - offset);
    }
    if (ntfs_read_record(fs, node->id, rec) != 0) {
        return -1;
    }
    data = ntfs_find_attr(rec, fs->mft_record_bytes, NTFS_ATTR_DATA);
    if (!data) {
        return -1;
    }
    if (ntfs_data_attr(rec, fs->mft_record_bytes, data, &attr) != 0) {
        return -1;
    }
    if (attr.resident) {
        /* Resident: the file is inside its own record, and there are no
         * clusters to map. A small file on NTFS occupies no space on the
         * volume at all.
         *
         * Bounded against the *validated* value length rather than against the
         * size the volume claimed. The two disagree exactly when somebody is
         * trying to read the kernel stack this record sits on. */
        uint32_t i;

        if (offset >= attr.value_len) {
            return 0;
        }
        if (len > attr.value_len - (uint32_t)offset) {
            len = attr.value_len - (uint32_t)offset;
        }
        for (i = 0; i < len; i++) {
            dst[i] = attr.value[offset + i];
        }
        return (long)len;
    }
    {
        const uint8_t *runs = attr.runs;
        uint32_t runs_len = attr.runs_len;

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
