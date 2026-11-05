/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef VIBEOS_EXT4_WRITE_H
#define VIBEOS_EXT4_WRITE_H

/* The ext4 writer (kernel/fs/ext4_write.c, docs/filesystem-design/ext4.md
 * step 2): what the reader's driver in ext2.c asks of it when it mounts a
 * volume as "ext4". Everything else the writer does is behind the operations
 * table it returns. */

#include "vibeos/ext2.h"
#include "vibeos/vfs.h"

/* Load the superblock into the volume's state and decide whether the volume is
 * written: ext4 with extents and metadata_csum, on a device that takes writes,
 * and nothing the writer does not keep. Sets fs->writable. */
void ext4_w_mount(vibeos_ext2_t *fs);

/* The operations a mounted "ext4" volume gets: the writing set if the volume
 * is written, the reading set otherwise. */
const vibeos_fs_ops_t *ext4_ops(const vibeos_ext2_t *fs);

#endif
