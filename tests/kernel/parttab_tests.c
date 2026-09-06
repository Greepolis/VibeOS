/* Writing a partition table: the refusals, and the one write that is allowed.
 *
 * Every test here is about a *refusal*, which is unusual and is the point.
 * Reading a table wrongly gives a machine that will not boot; writing one
 * wrongly destroys data that was never this machine's to lose, silently, and
 * somebody else finds out later. The interesting behaviour of this layer is
 * almost entirely the things it declines to do.
 */

#include <stdio.h>
#include <string.h>

#include "vibeos/parttab.h"

#define PT_SECTORS 2048u

static uint8_t g_disk[PT_SECTORS][VIBEOS_BLOCK_SIZE];
static uint8_t g_slot_data[4][VIBEOS_BLOCK_SIZE];
static vibeos_block_slot_t g_slots[4];
static vibeos_blockdev_t g_dev;
static vibeos_blockcache_t g_bc;

static int dev_read(void *ctx, uint64_t lba, void *buf) {
    (void)ctx;
    if (lba >= PT_SECTORS) { return -1; }
    memcpy(buf, g_disk[lba], VIBEOS_BLOCK_SIZE);
    return 0;
}
static int dev_write(void *ctx, uint64_t lba, const void *buf) {
    (void)ctx;
    if (lba >= PT_SECTORS) { return -1; }
    memcpy(g_disk[lba], buf, VIBEOS_BLOCK_SIZE);
    return 0;
}
static int dev_flush(void *ctx) { (void)ctx; return 0; }

static int fail(const char *what, vibeos_parttab_result_t got) {
    printf("FAIL:parttab %s (got %s)\n", what, vibeos_parttab_result_name(got));
    return -1;
}

static void setup(void) {
    uint32_t i;
    memset(g_disk, 0, sizeof(g_disk));
    /* A plausible existing sector 0: boot code that must survive, and a
     * signature. */
    for (i = 0; i < 446u; i++) {
        g_disk[0][i] = (uint8_t)(i ^ 0x5Au);
    }
    g_disk[0][510] = 0x55u;
    g_disk[0][511] = 0xAAu;
    for (i = 0; i < 4u; i++) {
        g_slots[i].data = g_slot_data[i];
    }
    g_dev.read = dev_read;
    g_dev.write = dev_write;
    g_dev.flush = dev_flush;
    g_dev.ctx = 0;
    g_dev.sectors = PT_SECTORS;
    (void)vibeos_blockcache_init(&g_bc, &g_dev, g_slots, 4u);
}

static void one_entry(vibeos_parttable_t *t, uint64_t first, uint64_t count) {
    memset(t, 0, sizeof(*t));
    t->count = 1;
    t->entry[0].first_lba = first;
    t->entry[0].sector_count = count;
    t->entry[0].mbr_type = 0x0Cu;
}

int test_parttab(void) {
    vibeos_parttable_t t;
    vibeos_parttab_guard_t guard;
    uint32_t sum = 0, sum2 = 0;
    vibeos_parttab_result_t r;
    uint32_t i;

    setup();
    memset(&guard, 0, sizeof(guard));

    if (vibeos_parttab_checksum(&g_bc, PT_SECTORS, &sum) != 0) {
        printf("FAIL:parttab checksum failed\n");
        return -1;
    }

    /* ---- what is refused --------------------------------------------- */

    one_entry(&t, 64, 128);
    r = vibeos_parttab_write_mbr(&g_bc, PT_SECTORS, &t, &guard, sum ^ 1u);
    if (r != VIBEOS_PARTTAB_STALE) {
        /* Two things that both read the table and one of which writes: the
         * writer silently discards the other's work, and on a partition table
         * that work is where somebody's filesystem begins. */
        return fail("a stale view was allowed to write", r);
    }

    one_entry(&t, 0, 128);
    r = vibeos_parttab_write_mbr(&g_bc, PT_SECTORS, &t, &guard, sum);
    if (r != VIBEOS_PARTTAB_OVERLAP) {
        /* Sector 0 is the table. A partition starting there is overwritten by
         * the write that creates it. */
        return fail("a partition was allowed to start at sector 0", r);
    }

    one_entry(&t, PT_SECTORS - 4u, 128);
    r = vibeos_parttab_write_mbr(&g_bc, PT_SECTORS, &t, &guard, sum);
    if (r != VIBEOS_PARTTAB_PAST_END) {
        return fail("a partition past the end of the disk was accepted", r);
    }

    memset(&t, 0, sizeof(t));
    t.count = 2;
    t.entry[0].first_lba = 64;  t.entry[0].sector_count = 128;
    t.entry[1].first_lba = 128; t.entry[1].sector_count = 128;
    r = vibeos_parttab_write_mbr(&g_bc, PT_SECTORS, &t, &guard, sum);
    if (r != VIBEOS_PARTTAB_OVERLAP) {
        /* Not "slightly wrong": two filesystems each believing they own the
         * same blocks, discovered as corruption in one of them much later. */
        return fail("overlapping entries were accepted", r);
    }

    /* A range somebody is standing on, covered by the new table. */
    guard.count = 1;
    guard.range[0].first_lba = 64;
    guard.range[0].sectors = 128;
    guard.range[0].why = "mounted";
    one_entry(&t, 32, 512);
    r = vibeos_parttab_write_mbr(&g_bc, PT_SECTORS, &t, &guard, sum);
    if (r != VIBEOS_PARTTAB_IN_USE) {
        return fail("a mounted range was repartitioned", r);
    }

    /* And the other mistake with the same consequence: the new table simply
     * does not have it any more. */
    one_entry(&t, 1024, 512);
    r = vibeos_parttab_write_mbr(&g_bc, PT_SECTORS, &t, &guard, sum);
    if (r != VIBEOS_PARTTAB_IN_USE) {
        return fail("a mounted partition was dropped from the table", r);
    }

    /* Growing a mounted partition in place.
     *
     * This case exists because removing the overlap check did not turn this
     * test red: the two cases above both fall through to the "no new entry
     * covers this range" check and are refused by *that* one instead, so the
     * overlap check could be deleted and nothing noticed. One check was
     * masking the other.
     *
     * Here the new entry starts exactly where the guard does and is larger, so
     * the coverage rule is satisfied and only the overlap rule can refuse it.
     * Refusing is right: a filesystem that is mounted has an opinion about
     * where it ends, and changing that underneath it is how a mounted volume
     * comes to read past its own data. */
    one_entry(&t, 64, 256);
    r = vibeos_parttab_write_mbr(&g_bc, PT_SECTORS, &t, &guard, sum);
    if (r != VIBEOS_PARTTAB_IN_USE) {
        return fail("a mounted partition was grown underneath its filesystem", r);
    }

    /* ---- and the write that is allowed -------------------------------- */

    guard.count = 0;
    one_entry(&t, 64, 128);
    r = vibeos_parttab_write_mbr(&g_bc, PT_SECTORS, &t, &guard, sum);
    if (r != VIBEOS_PARTTAB_OK) {
        return fail("a valid table was refused", r);
    }

    /* The boot code survived. A writer that rebuilt sector 0 from nothing
     * would make a disk unbootable while doing exactly what it was asked. */
    for (i = 0; i < 446u; i++) {
        if (g_disk[0][i] != (uint8_t)(i ^ 0x5Au)) {
            printf("FAIL:parttab the boot code was destroyed at byte %u\n", i);
            return -1;
        }
    }

    /* It reads back as what was asked for. */
    {
        vibeos_parttable_t back;
        int protective = 0;
        if (vibeos_partition_parse_mbr(g_disk[0], &back, &protective) != 0) {
            printf("FAIL:parttab the table it wrote does not parse\n");
            return -1;
        }
        if (back.count != 1u || back.entry[0].first_lba != 64ull ||
            back.entry[0].sector_count != 128ull) {
            printf("FAIL:parttab the table read back differs from the one written\n");
            return -1;
        }
    }

    /* And the checksum moved, so the next writer's stale view is caught. */
    if (vibeos_parttab_checksum(&g_bc, PT_SECTORS, &sum2) != 0 || sum2 == sum) {
        printf("FAIL:parttab the checksum did not change after a write\n");
        return -1;
    }
    r = vibeos_parttab_write_mbr(&g_bc, PT_SECTORS, &t, &guard, sum);
    if (r != VIBEOS_PARTTAB_STALE) {
        return fail("the second write with the old checksum was allowed", r);
    }

    return 0;
}
