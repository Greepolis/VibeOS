/* Write-ahead journal. See include/vibeos/journal.h for why the phases are
 * arranged the way they are. */

#include "vibeos/journal.h"

#include <string.h>

#define JOURNAL_DESC_MAGIC 0x314A4256u   /* "VBJ1" */
#define JOURNAL_COMMIT_MAGIC 0x434A4256u /* "VBJC" */

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr64(uint8_t *p, uint64_t v)
{
    wr32(p, (uint32_t)(v & 0xFFFFFFFFu));
    wr32(p + 4, (uint32_t)(v >> 32));
}

static uint64_t rd64(const uint8_t *p)
{
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

/* Fletcher-32, carried across blocks so the sum covers the whole data region
 * rather than each block separately. This does not defend against a hostile
 * volume and is not meant to: it catches a commit record that survived while
 * the data it describes did not, which is the one corruption the ordering
 * rules alone cannot rule out on a device that reorders inside a flush. */
static uint32_t journal_sum(uint32_t state, const uint8_t *data, uint32_t len)
{
    uint32_t a = state & 0xFFFFu;
    uint32_t b = state >> 16;
    uint32_t i;

    for (i = 0; i < len; i++) {
        a = (a + data[i]) % 65535u;
        b = (b + a) % 65535u;
    }
    return (b << 16) | a;
}

static int journal_flush(vibeos_journal_t *j)
{
    return vibeos_blockcache_flush(j->bc);
}

/* Copy every block a committed descriptor names to where it belongs. Doing
 * this twice is the normal case after a crash, not an error path. */
static int journal_checkpoint(vibeos_journal_t *j, const uint8_t *desc,
                              uint32_t count)
{
    uint8_t block[VIBEOS_BLOCK_SIZE];
    uint32_t i;

    for (i = 0; i < count; i++) {
        if (vibeos_blockcache_read(j->bc, j->base + 1u + i, block) != 0) {
            return -1;
        }
        if (vibeos_blockcache_write(j->bc, rd64(desc + 16u + i * 8u), block) != 0) {
            return -1;
        }
    }
    /* The targets must be durable before the descriptor is retired, or a crash
     * here would leave a volume that is half updated and no longer has a
     * record saying so. */
    return journal_flush(j);
}

/* Retire the region. Only the descriptor's magic is cleared: that is the one
 * word recovery looks at first, so a single block write ends the transaction. */
static int journal_retire(vibeos_journal_t *j)
{
    uint8_t block[VIBEOS_BLOCK_SIZE];

    memset(block, 0, sizeof(block));
    if (vibeos_blockcache_write(j->bc, j->base, block) != 0) {
        return -1;
    }
    return journal_flush(j);
}

static int journal_recover(vibeos_journal_t *j)
{
    uint8_t desc[VIBEOS_BLOCK_SIZE];
    uint8_t commit[VIBEOS_BLOCK_SIZE];
    uint8_t block[VIBEOS_BLOCK_SIZE];
    uint32_t count;
    uint32_t sum = 0xFFFFFFFFu;
    uint32_t i;

    if (vibeos_blockcache_read(j->bc, j->base, desc) != 0) {
        return -1;
    }
    if (rd32(desc) != JOURNAL_DESC_MAGIC) {
        return 0;   /* nothing was in flight */
    }

    count = rd32(desc + 4);
    if (count == 0u || count > j->max_targets) {
        /* A descriptor that cannot be believed. It was never committed - a
         * commit record is only written after this block is durable - so the
         * volume still holds the old contents and the region can be dropped. */
        return journal_retire(j);
    }

    if (vibeos_blockcache_read(j->bc, j->base + 1u + count, commit) != 0) {
        return -1;
    }
    /* The commit record must belong to *this* descriptor, and a sequence
     * number is not enough to say so: the counter restarts at every mount, so
     * a record left behind by a finished transaction can carry the same one.
     * Binding it to the descriptor's contents is what makes a leftover record
     * recognisable as somebody else's. */
    if (rd32(commit) != JOURNAL_COMMIT_MAGIC ||
        rd64(commit + 8) != rd64(desc + 8) ||
        rd32(commit + 20) != journal_sum(0xFFFFFFFFu, desc, VIBEOS_BLOCK_SIZE)) {
        /* Descriptor without a matching commit: the crash landed before the
         * transaction became real. Every target still holds its old contents,
         * which is a state the caller asked for. */
        return journal_retire(j);
    }

    /* Recompute over what is actually in the region rather than trusting that
     * it is what the writer staged. */
    for (i = 0; i < count; i++) {
        if (vibeos_blockcache_read(j->bc, j->base + 1u + i, block) != 0) {
            return -1;
        }
        sum = journal_sum(sum, block, VIBEOS_BLOCK_SIZE);
    }
    if (sum != rd32(commit + 16)) {
        return journal_retire(j);
    }

    if (journal_checkpoint(j, desc, count) != 0) {
        return -1;
    }
    j->seq = rd64(desc + 8);
    j->replays++;
    return journal_retire(j);
}

int vibeos_journal_init(vibeos_journal_t *j, vibeos_blockcache_t *bc,
                        uint64_t base, uint32_t capacity,
                        uint64_t *targets, uint8_t *staging)
{
    if (j == 0 || bc == 0 || targets == 0 || staging == 0) {
        return -1;
    }
    if (capacity <= VIBEOS_JOURNAL_OVERHEAD) {
        return -1;   /* no room for even one target */
    }

    memset(j, 0, sizeof(*j));
    j->bc = bc;
    j->base = base;
    j->capacity = capacity;
    j->max_targets = capacity - VIBEOS_JOURNAL_OVERHEAD;
    if (j->max_targets > VIBEOS_JOURNAL_MAX_TARGETS) {
        j->max_targets = VIBEOS_JOURNAL_MAX_TARGETS;
    }
    j->targets = targets;
    j->staging = staging;

    return journal_recover(j);
}

int vibeos_journal_begin(vibeos_journal_t *j)
{
    if (j == 0 || j->open) {
        return -1;
    }
    j->open = 1;
    j->staged = 0;
    return 0;
}

int vibeos_journal_stage(vibeos_journal_t *j, uint64_t lba, const void *buf)
{
    uint32_t i;

    if (j == 0 || !j->open || buf == 0) {
        return -1;
    }

    /* A caller that rewrites one allocation bitmap ten times in a transaction
     * would otherwise fill the region with nine stale copies of it. */
    for (i = 0; i < j->staged; i++) {
        if (j->targets[i] == lba) {
            memcpy(j->staging + (size_t)i * VIBEOS_BLOCK_SIZE, buf,
                   VIBEOS_BLOCK_SIZE);
            return 0;
        }
    }

    if (j->staged >= j->max_targets) {
        return -1;
    }
    j->targets[j->staged] = lba;
    memcpy(j->staging + (size_t)j->staged * VIBEOS_BLOCK_SIZE, buf,
           VIBEOS_BLOCK_SIZE);
    j->staged++;
    return 0;
}

void vibeos_journal_abort(vibeos_journal_t *j)
{
    if (j != 0) {
        j->open = 0;
        j->staged = 0;
    }
}

int vibeos_journal_commit(vibeos_journal_t *j)
{
    uint8_t desc[VIBEOS_BLOCK_SIZE];
    uint8_t commit[VIBEOS_BLOCK_SIZE];
    uint32_t sum = 0xFFFFFFFFu;
    uint32_t i;

    if (j == 0 || !j->open) {
        return -1;
    }
    if (j->staged == 0u) {
        j->open = 0;
        return 0;   /* an empty transaction is not a failure */
    }

    memset(desc, 0, sizeof(desc));
    wr32(desc, JOURNAL_DESC_MAGIC);
    wr32(desc + 4, j->staged);
    wr64(desc + 8, j->seq + 1u);
    for (i = 0; i < j->staged; i++) {
        wr64(desc + 16u + i * 8u, j->targets[i]);
    }

    /* Phase one: the descriptor and the new contents, into the region. Nothing
     * outside the region is touched, so a crash anywhere in here is invisible
     * to the volume. */
    if (vibeos_blockcache_write(j->bc, j->base, desc) != 0) {
        j->open = 0;
        return -1;
    }
    for (i = 0; i < j->staged; i++) {
        const uint8_t *src = j->staging + (size_t)i * VIBEOS_BLOCK_SIZE;

        if (vibeos_blockcache_write(j->bc, j->base + 1u + i, src) != 0) {
            j->open = 0;
            return -1;
        }
        sum = journal_sum(sum, src, VIBEOS_BLOCK_SIZE);
    }
    if (journal_flush(j) != 0) {
        j->open = 0;
        return -1;
    }

    /* Phase two: the commit record, alone, after that flush. This is the
     * instant the transaction becomes real; before it, recovery discards the
     * region, and after it, recovery replays the region. There is no third
     * outcome, which is the entire property being bought here. */
    memset(commit, 0, sizeof(commit));
    wr32(commit, JOURNAL_COMMIT_MAGIC);
    wr64(commit + 8, j->seq + 1u);
    wr32(commit + 16, sum);
    wr32(commit + 20, journal_sum(0xFFFFFFFFu, desc, VIBEOS_BLOCK_SIZE));
    if (vibeos_blockcache_write(j->bc, j->base + 1u + j->staged, commit) != 0) {
        j->open = 0;
        return -1;
    }
    if (journal_flush(j) != 0) {
        j->open = 0;
        return -1;
    }

    j->seq++;

    /* Phase three: put the blocks where they belong. A crash in here is what
     * the region exists for - the next mount finds a committed descriptor and
     * does this again. */
    if (journal_checkpoint(j, desc, j->staged) != 0) {
        j->open = 0;
        return -1;
    }

    j->commits++;
    j->open = 0;
    j->staged = 0;
    return journal_retire(j);
}
