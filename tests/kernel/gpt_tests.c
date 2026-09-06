/* Writing a GPT, and what an interrupted write leaves behind (I4c, I5c).
 *
 * The claim is not "the writer writes a GPT". It is that a machine losing
 * power part way through leaves a disk a reader can still make sense of - and
 * the specific invariant that buys, which is worth stating precisely because
 * it is narrower than "all or nothing":
 *
 *   **A GPT header that validates always describes an entry array that is
 *   actually on the disk.**
 *
 * That is what the write order is for. A header is a claim about bytes, and a
 * claim written before the bytes is a table that passes every check and points
 * at rubbish. The array goes down and is flushed; only then the header that
 * names it. Backup pair first, primary pair second, protective MBR last.
 *
 * The device below has a volatile cache of its own, as real drives do, and its
 * flush applies pending writes in an order the test chooses. A writer that
 * survives only because the cache happened to be helpful has not been shown to
 * survive anything - the same reason the journal harness does it.
 */

#include <stdio.h>
#include <string.h>

#include "vibeos/parttab.h"
#include "vibeos/partition.h"

#define GT_SECTORS 256u
#define GT_PENDING_MAX 128u

static uint8_t g_gt_platter[GT_SECTORS][VIBEOS_BLOCK_SIZE];
static uint64_t g_gt_pending_lba[GT_PENDING_MAX];
static uint8_t g_gt_pending_data[GT_PENDING_MAX][VIBEOS_BLOCK_SIZE];
static uint32_t g_gt_pending;
static uint32_t g_gt_landed;
static uint32_t g_gt_power_off;
static uint32_t g_gt_order;

static int gt_read(void *ctx, uint64_t lba, void *buf) {
    uint32_t i;

    (void)ctx;
    if (lba >= GT_SECTORS) {
        return -1;
    }
    for (i = g_gt_pending; i > 0u; i--) {
        if (g_gt_pending_lba[i - 1u] == lba) {
            memcpy(buf, g_gt_pending_data[i - 1u], VIBEOS_BLOCK_SIZE);
            return 0;
        }
    }
    memcpy(buf, g_gt_platter[lba], VIBEOS_BLOCK_SIZE);
    return 0;
}

static int gt_write(void *ctx, uint64_t lba, const void *buf) {
    (void)ctx;
    if (lba >= GT_SECTORS || g_gt_pending >= GT_PENDING_MAX) {
        return -1;
    }
    g_gt_pending_lba[g_gt_pending] = lba;
    memcpy(g_gt_pending_data[g_gt_pending], buf, VIBEOS_BLOCK_SIZE);
    g_gt_pending++;
    return 0;
}

static int gt_flush(void *ctx) {
    uint32_t n = g_gt_pending;
    uint32_t k;

    (void)ctx;
    for (k = 0; k < n; k++) {
        uint32_t i = (k + g_gt_order) % n;

        if (g_gt_landed >= g_gt_power_off) {
            g_gt_pending = 0;
            return -1;
        }
        memcpy(g_gt_platter[g_gt_pending_lba[i]], g_gt_pending_data[i],
               VIBEOS_BLOCK_SIZE);
        g_gt_landed++;
    }
    g_gt_pending = 0;
    return 0;
}

static uint8_t g_gt_slot_data[8][VIBEOS_BLOCK_SIZE];
static vibeos_block_slot_t g_gt_slots[8];

static void gt_attach(vibeos_blockdev_t *dev, vibeos_blockcache_t *bc) {
    uint32_t i;

    memset(dev, 0, sizeof(*dev));
    dev->read = gt_read;
    dev->write = gt_write;
    dev->flush = gt_flush;
    dev->ctx = 0;
    dev->sectors = GT_SECTORS;
    for (i = 0; i < 8u; i++) {
        g_gt_slots[i].data = g_gt_slot_data[i];
    }
    (void)vibeos_blockcache_init(bc, dev, g_gt_slots, 8u);
}

static void gt_reset(void) {
    memset(g_gt_platter, 0, sizeof(g_gt_platter));
    /* Plausible boot code in sector 0, which a repartition must not destroy. */
    {
        uint32_t i;
        for (i = 0; i < 446u; i++) {
            g_gt_platter[0][i] = (uint8_t)(i ^ 0x5Au);
        }
        g_gt_platter[0][510] = 0x55u;
        g_gt_platter[0][511] = 0xAAu;
    }
    g_gt_pending = 0;
    g_gt_landed = 0;
    g_gt_power_off = 0xFFFFFFFFu;
}

static const uint8_t g_gt_guid[16] = {
    0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
    0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x01
};

static void gt_table(vibeos_parttable_t *t) {
    memset(t, 0, sizeof(*t));
    t->count = 2u;
    t->entry[0].first_lba = 40u;
    t->entry[0].sector_count = 60u;
    t->entry[1].first_lba = 100u;
    t->entry[1].sector_count = 60u;
}

static vibeos_parttab_result_t gt_write_table(void) {
    vibeos_blockdev_t dev;
    vibeos_blockcache_t bc;
    vibeos_parttab_guard_t guard;
    vibeos_parttable_t t;
    uint32_t sum = 0;

    gt_attach(&dev, &bc);
    memset(&guard, 0, sizeof(guard));
    gt_table(&t);
    if (vibeos_parttab_checksum(&bc, GT_SECTORS, &sum) != 0) {
        return VIBEOS_PARTTAB_IO;
    }
    return vibeos_parttab_write_gpt(&bc, GT_SECTORS, &t, &guard, g_gt_guid,
                                    sum);
}

/* Read one header straight off the platter and ask whether it validates
 * against the array it names. This is the invariant, so it is checked by
 * reading what a fresh reader would read - never through the cache that just
 * wrote it. */
static int gt_header_is_honest(uint64_t header_lba) {
    static uint8_t entries[VIBEOS_PARTTAB_GPT_ARRAY_SECTORS *
                           VIBEOS_BLOCK_SIZE];
    const uint8_t *h = g_gt_platter[header_lba];
    vibeos_parttable_t parsed;
    uint64_t array_lba;
    uint32_t i;

    if (memcmp(h, "EFI PART", 8) != 0) {
        return 0;      /* no header here at all: not a claim, so not a lie */
    }
    array_lba = (uint64_t)h[72] | ((uint64_t)h[73] << 8) |
                ((uint64_t)h[74] << 16) | ((uint64_t)h[75] << 24);
    if (array_lba + VIBEOS_PARTTAB_GPT_ARRAY_SECTORS > GT_SECTORS) {
        return -1;
    }
    for (i = 0; i < VIBEOS_PARTTAB_GPT_ARRAY_SECTORS; i++) {
        memcpy(entries + (size_t)i * VIBEOS_BLOCK_SIZE,
               g_gt_platter[array_lba + i], VIBEOS_BLOCK_SIZE);
    }
    if (vibeos_partition_parse_gpt(h, entries, sizeof(entries), GT_SECTORS,
                                   &parsed) != 0) {
        /* The header did not validate. That is a perfectly good outcome for an
         * interrupted write - it means a reader will fall back to the other
         * copy - so it is not a failure here. What would be a failure is a
         * header that validates while its array does not, and parse_gpt checks
         * both CRCs, so reaching here means one of them did not hold. */
        return 0;
    }
    return 1;
}

/* ---- the tests ----------------------------------------------------------- */

static int test_gpt_writes_a_table_a_reader_accepts(void) {
    gt_reset();
    if (gt_write_table() != VIBEOS_PARTTAB_OK) {
        printf("FAIL:gpt the write was refused\n");
        return -1;
    }
    if (gt_header_is_honest(1u) != 1) {
        printf("FAIL:gpt the primary header does not validate\n");
        return -1;
    }
    if (gt_header_is_honest(GT_SECTORS - 1u) != 1) {
        printf("FAIL:gpt the backup header does not validate\n");
        return -1;
    }
    /* The boot code survived, which is rule "edit, do not rebuild". */
    {
        uint32_t i;
        for (i = 0; i < 446u; i++) {
            if (g_gt_platter[0][i] != (uint8_t)(i ^ 0x5Au)) {
                printf("FAIL:gpt the write destroyed the boot code\n");
                return -1;
            }
        }
    }
    if (g_gt_platter[0][446 + 4] != 0xEEu) {
        printf("FAIL:gpt no protective MBR entry\n");
        return -1;
    }
    return 0;
}

/* The sweep. Power is cut after every possible number of landed writes, in two
 * different flush orders, and after each one the disk must hold no header that
 * lies about its array. */
static int test_gpt_interrupted_write_leaves_no_lying_header(void) {
    uint32_t cut;
    uint32_t order;

    for (order = 0; order < 3u; order++) {
        for (cut = 0; cut < 80u; cut++) {
            gt_reset();
            g_gt_order = order;
            g_gt_power_off = cut;
            (void)gt_write_table();   /* it is expected to fail part way */

            if (gt_header_is_honest(1u) < 0 ||
                gt_header_is_honest(GT_SECTORS - 1u) < 0) {
                printf("FAIL:gpt a header pointed off the disk "
                       "(cut=%u order=%u)\n", cut, order);
                return -1;
            }
            /* And the boot code is intact however far the write got: the
             * protective MBR is written last and is an edit, so no prefix of
             * this sequence can have destroyed it. */
            {
                uint32_t i;
                for (i = 0; i < 446u; i++) {
                    if (g_gt_platter[0][i] != (uint8_t)(i ^ 0x5Au)) {
                        printf("FAIL:gpt boot code lost at cut=%u order=%u\n",
                               cut, order);
                        return -1;
                    }
                }
            }
        }
    }
    g_gt_power_off = 0xFFFFFFFFu;
    g_gt_order = 0;
    return 0;
}

/* Rule 4 in parttab.h, as a test rather than as a comment: the backup pair is
 * complete before the primary pair is begun. Cutting power at the moment the
 * primary array starts must leave a backup a reader can use. */
static int test_gpt_backup_lands_before_the_primary(void) {
    uint32_t cut;
    int saw_backup_only = 0;

    for (cut = 0; cut < 80u; cut++) {
        int primary, backup;

        gt_reset();
        g_gt_order = 0;
        g_gt_power_off = cut;
        (void)gt_write_table();

        primary = gt_header_is_honest(1u);
        backup = gt_header_is_honest(GT_SECTORS - 1u);
        if (primary == 1 && backup != 1) {
            printf("FAIL:gpt a primary landed without its backup (cut=%u)\n",
                   cut);
            return -1;
        }
        if (backup == 1 && primary != 1) {
            saw_backup_only = 1;
        }
    }
    g_gt_power_off = 0xFFFFFFFFu;
    /* And the interesting window was actually visited. Without this the test
     * passes on a writer that never gets far enough to have a backup at all,
     * which is the shape of a check that cannot fail. */
    if (!saw_backup_only) {
        printf("FAIL:gpt the sweep never reached the backup-only window\n");
        return -1;
    }
    return 0;
}

/* The refusals a GPT adds to the ones an MBR already has: a GPT reserves both
 * ends of the disk, so an entry that would be legal on an MBR can still land
 * on the table itself. */
static int test_gpt_refuses_an_entry_over_its_own_table(void) {
    vibeos_blockdev_t dev;
    vibeos_blockcache_t bc;
    vibeos_parttab_guard_t guard;
    vibeos_parttable_t t;
    uint32_t sum = 0;

    gt_reset();
    gt_attach(&dev, &bc);
    memset(&guard, 0, sizeof(guard));
    (void)vibeos_parttab_checksum(&bc, GT_SECTORS, &sum);

    /* Starting at 2: inside the primary entry array. */
    memset(&t, 0, sizeof(t));
    t.count = 1u;
    t.entry[0].first_lba = 2u;
    t.entry[0].sector_count = 10u;
    if (vibeos_parttab_write_gpt(&bc, GT_SECTORS, &t, &guard, g_gt_guid,
                                 sum) == VIBEOS_PARTTAB_OK) {
        printf("FAIL:gpt an entry over the primary array was accepted\n");
        return -1;
    }
    /* Running into the backup array at the far end. */
    t.entry[0].first_lba = 200u;
    t.entry[0].sector_count = 50u;
    if (vibeos_parttab_write_gpt(&bc, GT_SECTORS, &t, &guard, g_gt_guid,
                                 sum) == VIBEOS_PARTTAB_OK) {
        printf("FAIL:gpt an entry over the backup array was accepted\n");
        return -1;
    }
    /* A disk too small to hold two copies of its own table. */
    if (vibeos_parttab_write_gpt(&bc, 8ull, &t, &guard, g_gt_guid,
                                 sum) == VIBEOS_PARTTAB_OK) {
        printf("FAIL:gpt a disk too small for a GPT was accepted\n");
        return -1;
    }
    return 0;
}

/* The stale check applies here as it does to the MBR writer: a caller acting
 * on a view of the disk that has since changed is refused. */
static int test_gpt_refuses_a_stale_view(void) {
    vibeos_blockdev_t dev;
    vibeos_blockcache_t bc;
    vibeos_parttab_guard_t guard;
    vibeos_parttable_t t;
    uint32_t sum = 0;

    gt_reset();
    if (gt_write_table() != VIBEOS_PARTTAB_OK) {
        return -1;
    }
    gt_attach(&dev, &bc);
    memset(&guard, 0, sizeof(guard));
    gt_table(&t);
    /* The checksum from before the write, which is now stale. */
    if (vibeos_parttab_write_gpt(&bc, GT_SECTORS, &t, &guard, g_gt_guid,
                                 sum) != VIBEOS_PARTTAB_STALE) {
        printf("FAIL:gpt a stale view was accepted\n");
        return -1;
    }
    return 0;
}

int test_gpt(void) {
    if (test_gpt_writes_a_table_a_reader_accepts() != 0) {
        return -1;
    }
    if (test_gpt_interrupted_write_leaves_no_lying_header() != 0) {
        return -1;
    }
    if (test_gpt_backup_lands_before_the_primary() != 0) {
        return -1;
    }
    if (test_gpt_refuses_an_entry_over_its_own_table() != 0) {
        return -1;
    }
    if (test_gpt_refuses_a_stale_view() != 0) {
        return -1;
    }
    return 0;
}
