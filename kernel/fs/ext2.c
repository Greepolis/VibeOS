/* ext2, read-only.
 *
 * Portable: every byte comes through the block cache, which comes through a
 * function pair, so a fabricated image in an array exercises the same code the
 * kernel runs. Indirect block arithmetic is the part that is easy to get
 * subtly wrong and impossible to see wrong from a boot - a file just comes
 * back with the wrong contents somewhere past the twelfth block.
 */

#include "vibeos/ext2.h"

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* One filesystem block, assembled from the 512-byte sectors under it. The
 * caller supplies the buffer; this file has no allocator. */
static int ext2_read_block(vibeos_ext2_t *fs, uint32_t block, uint8_t *out) {
    uint32_t sectors = fs->block_size / VIBEOS_BLOCK_SIZE;
    uint32_t i;

    if (fs->block_size == 0u || sectors == 0u) {
        return -1;
    }
    for (i = 0; i < sectors; i++) {
        uint64_t lba = fs->part_lba + (uint64_t)block * sectors + i;
        if (vibeos_blockcache_read(fs->cache, lba, out + i * VIBEOS_BLOCK_SIZE) != 0) {
            return -1;
        }
    }
    return 0;
}

/* Blocks are up to 4 KiB here. A larger block size is legal in ext2 but rare,
 * and refusing it is honest: reading with the wrong size returns bytes from
 * the wrong place rather than failing. */
#define EXT2_MAX_BLOCK 4096u
#define EXT2_MAX_INODE_SIZE 256u

int vibeos_ext2_mount(vibeos_ext2_t *fs, vibeos_blockcache_t *cache,
                      uint64_t part_lba) {
    uint8_t sb[VIBEOS_BLOCK_SIZE * 2u];
    uint32_t log_bs, blocks_count;

    if (!fs || !cache) {
        return -1;
    }
    fs->cache = cache;
    fs->part_lba = part_lba;
    fs->mounted = 0;

    /* The superblock is at byte 1024, whatever the block size, which is why it
     * is read as sectors rather than as a block: the block size is what we are
     * about to learn. */
    if (vibeos_blockcache_read(cache, part_lba + 2u, sb) != 0 ||
        vibeos_blockcache_read(cache, part_lba + 3u, sb + VIBEOS_BLOCK_SIZE) != 0) {
        return -1;
    }
    if (rd16(sb + 56) != VIBEOS_EXT2_MAGIC) {
        return -1;
    }
    log_bs = rd32(sb + 24);
    if (log_bs > 2u) {
        return -1;   /* larger than 4 KiB; see EXT2_MAX_BLOCK */
    }
    fs->block_size = 1024u << log_bs;
    fs->blocks_per_group = rd32(sb + 32);
    fs->inodes_per_group = rd32(sb + 40);
    fs->first_data_block = rd32(sb + 20);
    blocks_count = rd32(sb + 4);

    /* s_inode_size only exists from revision 1; revision 0 always used 128. */
    fs->inode_size = (rd32(sb + 76) >= 1u) ? rd16(sb + 88) : 128u;
    if (fs->inode_size < 128u || fs->inode_size > EXT2_MAX_INODE_SIZE) {
        return -1;
    }
    if (fs->blocks_per_group == 0u || fs->inodes_per_group == 0u) {
        return -1;
    }
    fs->group_count = (blocks_count - fs->first_data_block + fs->blocks_per_group - 1u)
                      / fs->blocks_per_group;
    if (fs->group_count == 0u) {
        return -1;
    }
    /* The group descriptors live in the block after the superblock, which is
     * block 1 for a 1 KiB filesystem and block 1 as well for larger ones -
     * because the superblock still starts at byte 1024, inside block 0. */
    fs->group_desc_block = (fs->block_size == 1024u) ? 2u : 1u;
    fs->mounted = 1;
    return 0;
}

/* Fill `out` with one inode's 128 bytes of header. */
static int ext2_read_inode(vibeos_ext2_t *fs, uint32_t ino, uint8_t *out) {
    uint8_t block[EXT2_MAX_BLOCK];
    uint32_t group, index, table_block, offset, per_block;

    if (ino == 0u) {
        return -1;   /* inode zero does not exist; it means "no entry" */
    }
    group = (ino - 1u) / fs->inodes_per_group;
    index = (ino - 1u) % fs->inodes_per_group;
    if (group >= fs->group_count) {
        return -1;
    }
    /* Group descriptors are 32 bytes; the inode table pointer is at +8. */
    if (ext2_read_block(fs, fs->group_desc_block + (group * 32u) / fs->block_size,
                        block) != 0) {
        return -1;
    }
    table_block = rd32(block + ((group * 32u) % fs->block_size) + 8u);

    per_block = fs->block_size / fs->inode_size;
    if (per_block == 0u) {
        return -1;
    }
    if (ext2_read_block(fs, table_block + index / per_block, block) != 0) {
        return -1;
    }
    offset = (index % per_block) * fs->inode_size;
    {
        uint32_t i;
        for (i = 0; i < 128u; i++) {
            out[i] = block[offset + i];
        }
    }
    return 0;
}

/* Which filesystem block holds a file's block number `n`.
 *
 * Twelve direct entries, then one, two and three levels of indirection. The
 * boundaries are where this goes wrong, so they are computed rather than
 * unrolled: an off-by-one here returns a real block from the wrong offset, and
 * the file reads back plausible and wrong. */
static int ext2_map_block(vibeos_ext2_t *fs, const uint8_t *inode, uint32_t n,
                          uint32_t *out) {
    uint32_t per = fs->block_size / 4u;
    uint8_t block[EXT2_MAX_BLOCK];

    if (n < 12u) {
        *out = rd32(inode + 40u + n * 4u);
        return 0;
    }
    n -= 12u;
    if (n < per) {
        uint32_t ind = rd32(inode + 40u + 12u * 4u);
        if (ind == 0u || ext2_read_block(fs, ind, block) != 0) {
            return -1;
        }
        *out = rd32(block + n * 4u);
        return 0;
    }
    n -= per;
    if (n < per * per) {
        uint32_t dind = rd32(inode + 40u + 13u * 4u);
        uint32_t mid;
        if (dind == 0u || ext2_read_block(fs, dind, block) != 0) {
            return -1;
        }
        mid = rd32(block + (n / per) * 4u);
        if (mid == 0u || ext2_read_block(fs, mid, block) != 0) {
            return -1;
        }
        *out = rd32(block + (n % per) * 4u);
        return 0;
    }
    n -= per * per;
    {
        uint32_t tind = rd32(inode + 40u + 14u * 4u);
        uint32_t a, b;
        if (tind == 0u || ext2_read_block(fs, tind, block) != 0) {
            return -1;
        }
        a = rd32(block + (n / (per * per)) * 4u);
        if (a == 0u || ext2_read_block(fs, a, block) != 0) {
            return -1;
        }
        b = rd32(block + ((n / per) % per) * 4u);
        if (b == 0u || ext2_read_block(fs, b, block) != 0) {
            return -1;
        }
        *out = rd32(block + (n % per) * 4u);
    }
    return 0;
}

static uint64_t inode_size(const uint8_t *inode) {
    /* The high half of a regular file's size lives in what was a directory ACL
     * field. Ignoring it silently truncates every file over 4 GiB to its low
     * 32 bits, which reads as a shorter file rather than as an error. */
    uint64_t low = rd32(inode + 4);
    uint64_t high = rd32(inode + 108);
    return low | (high << 32);
}

static int inode_is_dir(const uint8_t *inode) {
    return (rd16(inode) & 0xF000u) == 0x4000u;
}

/* Find `name` in a directory inode. Returns the inode number, or 0. */
static uint32_t ext2_dir_find(vibeos_ext2_t *fs, const uint8_t *dir,
                              const char *name, uint32_t name_len) {
    uint8_t block[EXT2_MAX_BLOCK];
    uint64_t size = inode_size(dir);
    uint32_t nblocks = (uint32_t)((size + fs->block_size - 1u) / fs->block_size);
    uint32_t bi;

    for (bi = 0; bi < nblocks; bi++) {
        uint32_t phys = 0, off = 0;
        if (ext2_map_block(fs, dir, bi, &phys) != 0 || phys == 0u) {
            continue;
        }
        if (ext2_read_block(fs, phys, block) != 0) {
            return 0;
        }
        while (off + 8u <= fs->block_size) {
            uint32_t ino = rd32(block + off);
            uint16_t reclen = rd16(block + off + 4u);
            uint8_t nlen = block[off + 6u];

            /* A zero or unaligned record length would loop forever or walk
             * out of the block; a corrupt directory must end the scan, not
             * hang the kernel. Removing this guard does not make a test fail,
             * it makes the test suite hang - which is the whole reason it
             * lives here rather than being left to the caller. */
            if (reclen < 8u || (reclen & 3u) != 0u || off + reclen > fs->block_size) {
                break;
            }
            if (ino != 0u && nlen == name_len && off + 8u + nlen <= fs->block_size) {
                uint32_t i;
                for (i = 0; i < nlen; i++) {
                    if (block[off + 8u + i] != (uint8_t)name[i]) {
                        break;
                    }
                }
                if (i == nlen) {
                    return ino;
                }
            }
            off += reclen;
        }
    }
    return 0;
}

/* Walk a '/'-separated path from the root. */
static uint32_t ext2_resolve(vibeos_ext2_t *fs, const char *path, uint8_t *inode) {
    uint32_t ino = VIBEOS_EXT2_ROOT_INO;
    uint32_t i = 0;

    if (ext2_read_inode(fs, ino, inode) != 0) {
        return 0;
    }
    while (path[i] == '/') {
        i++;
    }
    while (path[i]) {
        uint32_t start = i;
        uint32_t len;
        while (path[i] && path[i] != '/') {
            i++;
        }
        len = i - start;
        if (len > 0u) {
            if (!inode_is_dir(inode)) {
                return 0;   /* a path component that is not a directory */
            }
            ino = ext2_dir_find(fs, inode, path + start, len);
            if (ino == 0u || ext2_read_inode(fs, ino, inode) != 0) {
                return 0;
            }
        }
        while (path[i] == '/') {
            i++;
        }
    }
    return ino;
}

/* ---- the filesystem interface --------------------------------------------*/

static int ext2_op_lookup(void *fsv, const char *path, vibeos_fs_node_t *out) {
    vibeos_ext2_t *fs = (vibeos_ext2_t *)fsv;
    uint8_t inode[128];
    uint32_t ino;

    if (!fs || !fs->mounted) {
        return -1;
    }
    ino = ext2_resolve(fs, path, inode);
    if (ino == 0u) {
        return -1;
    }
    out->id = ino;
    out->size = inode_size(inode);
    out->is_dir = inode_is_dir(inode);
    return 0;
}

static long ext2_op_read_at(void *fsv, const vibeos_fs_node_t *node,
                            uint64_t offset, void *buf, uint32_t len) {
    vibeos_ext2_t *fs = (vibeos_ext2_t *)fsv;
    uint8_t inode[128];
    uint8_t block[EXT2_MAX_BLOCK];
    uint8_t *dst = (uint8_t *)buf;
    uint64_t size;
    uint32_t done = 0;

    if (!fs || !fs->mounted || node->is_dir) {
        return -1;
    }
    /* The node carries an inode number, not the block pointers, so the inode
     * is fetched again here. See the header: this is the cost of a node
     * identity that cannot hold driver-private data. */
    if (ext2_read_inode(fs, (uint32_t)node->id, inode) != 0) {
        return -1;
    }
    size = inode_size(inode);
    if (offset >= size) {
        return 0;   /* end of file, which is not a failure */
    }
    if (offset + len > size) {
        len = (uint32_t)(size - offset);
    }
    while (done < len) {
        uint32_t bi = (uint32_t)((offset + done) / fs->block_size);
        uint32_t within = (uint32_t)((offset + done) % fs->block_size);
        uint32_t chunk = fs->block_size - within;
        uint32_t phys = 0;
        uint32_t i;

        if (chunk > len - done) {
            chunk = len - done;
        }
        if (ext2_map_block(fs, inode, bi, &phys) != 0) {
            return (done > 0u) ? (long)done : -1;
        }
        if (phys == 0u) {
            /* A hole: ext2 stores nothing for a run of zeroes, and a reader
             * that treated this as an error would fail on a sparse file that
             * is perfectly valid. */
            for (i = 0; i < chunk; i++) {
                dst[done + i] = 0;
            }
        } else {
            if (ext2_read_block(fs, phys, block) != 0) {
                return (done > 0u) ? (long)done : -1;
            }
            for (i = 0; i < chunk; i++) {
                dst[done + i] = block[within + i];
            }
        }
        done += chunk;
    }
    return (long)done;
}

static int ext2_op_list(void *fsv, const char *path, uint32_t index, char *name,
                        uint32_t name_cap, uint64_t *out_size, int *out_is_dir) {
    vibeos_ext2_t *fs = (vibeos_ext2_t *)fsv;
    uint8_t dir[128];
    uint8_t block[EXT2_MAX_BLOCK];
    uint64_t size;
    uint32_t nblocks, bi, seen = 0;

    if (!fs || !fs->mounted || ext2_resolve(fs, path, dir) == 0u) {
        return -1;
    }
    if (!inode_is_dir(dir)) {
        return -1;
    }
    size = inode_size(dir);
    nblocks = (uint32_t)((size + fs->block_size - 1u) / fs->block_size);
    for (bi = 0; bi < nblocks; bi++) {
        uint32_t phys = 0, off = 0;
        if (ext2_map_block(fs, dir, bi, &phys) != 0 || phys == 0u) {
            continue;
        }
        if (ext2_read_block(fs, phys, block) != 0) {
            return -1;
        }
        while (off + 8u <= fs->block_size) {
            uint32_t ino = rd32(block + off);
            uint16_t reclen = rd16(block + off + 4u);
            uint8_t nlen = block[off + 6u];

            if (reclen < 8u || (reclen & 3u) != 0u || off + reclen > fs->block_size) {
                break;
            }
            if (ino != 0u && nlen > 0u) {
                if (seen == index) {
                    uint32_t i;
                    for (i = 0; i + 1u < name_cap && i < nlen; i++) {
                        name[i] = (char)block[off + 8u + i];
                    }
                    name[i] = 0;
                    if (out_size || out_is_dir) {
                        uint8_t child[128];
                        if (ext2_read_inode(fs, ino, child) == 0) {
                            if (out_size) {
                                *out_size = inode_size(child);
                            }
                            if (out_is_dir) {
                                *out_is_dir = inode_is_dir(child);
                            }
                        }
                    }
                    return 0;
                }
                seen++;
            }
            off += reclen;
        }
    }
    return -1;   /* past the last entry */
}

static const vibeos_fs_ops_t g_ext2_ops = {
    ext2_op_lookup,
    ext2_op_read_at,
    0,              /* write_file: read-only, and the wrapper reports that */
    ext2_op_list,
    0,              /* unlink  */
    0               /* mkdir   */
};

const vibeos_fs_ops_t *vibeos_ext2_ops(void) {
    return &g_ext2_ops;
}
