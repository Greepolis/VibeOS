/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef VIBEOS_EXT4_JOURNAL_H
#define VIBEOS_EXT4_JOURNAL_H

/* ext4's journal (kernel/fs/ext4_journal.c, docs/filesystem-design/ext4.md
 * step 3). Everything it does happens inside the ext4 driver's operations and
 * its mount; what the rest of the kernel and the tests see is what it did. */

#include <stdint.h>

/* What the journal did, for the tests and the boot's report: transactions
 * committed, transactions replayed at mount, operations refused because the
 * log was full, and blocks a replay skipped because their tag checksum did
 * not match (must be zero on a healthy volume). */
typedef struct {
    uint64_t commits;
    uint64_t replayed;
    uint64_t log_full;
    uint64_t bad_tags;
} vibeos_ext4_journal_stats_t;
void vibeos_ext4_journal_stats(vibeos_ext4_journal_stats_t *out);

#endif
