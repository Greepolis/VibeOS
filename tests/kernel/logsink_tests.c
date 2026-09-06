/* The kernel's own log, on disk (I5b).
 *
 * The medium is a RAM array that survives across "reboots" - the whole point
 * of the feature is that the log outlives the machine, so the interesting
 * tests are the ones that detach, forget everything, and attach again.
 *
 * Every one of these was written with a sabotage in mind, and the case file
 * scripts/dev/cases/io-logsink.txt names which. A test whose defect cannot be
 * made to fire is not a test, and this module is one where that is easy to get
 * wrong: an in-memory medium that is recreated per test would pass every check
 * here while proving nothing about persistence.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "vibeos/logsink.h"

#define LT_SECTORS 64u

static uint8_t g_medium[LT_SECTORS][VIBEOS_LOGSINK_SECTOR];
static int g_fail_write_after;   /* -1: never */
static uint32_t g_writes;
static uint32_t g_reads;

static int lt_read(void *ctx, uint64_t lba, void *buf) {
    (void)ctx;
    if (lba >= LT_SECTORS) {
        return -1;
    }
    g_reads++;
    memcpy(buf, g_medium[lba], VIBEOS_LOGSINK_SECTOR);
    return 0;
}

static int lt_write(void *ctx, uint64_t lba, const void *buf) {
    (void)ctx;
    if (lba >= LT_SECTORS) {
        return -1;
    }
    if (g_fail_write_after >= 0 && (int)g_writes >= g_fail_write_after) {
        return -1;
    }
    g_writes++;
    memcpy(g_medium[lba], buf, VIBEOS_LOGSINK_SECTOR);
    return 0;
}

static uint32_t g_cpu;
static uint32_t lt_cpu(void) { return g_cpu; }

static vibeos_logsink_dev_t lt_dev(void) {
    vibeos_logsink_dev_t d;
    d.read = lt_read;
    d.write = lt_write;
    d.ctx = 0;
    d.sectors = LT_SECTORS;
    return d;
}

/* Attach without touching the medium: this is what a reboot looks like from
 * the module's point of view, and it is the only way to test recovery. */
static int lt_reattach(void) {
    vibeos_logsink_dev_t d = lt_dev();
    vibeos_logsink_reset();
    vibeos_logsink_set_cpu_id(lt_cpu);
    return vibeos_logsink_attach(&d);
}

static void lt_wipe(void) {
    memset(g_medium, 0, sizeof(g_medium));
    g_fail_write_after = -1;
    g_writes = 0;
    g_reads = 0;
    g_cpu = 0;
}

static int lt_say(const char *s) {
    return vibeos_logsink_write(s, (uint32_t)strlen(s));
}

static int lt_is(const vibeos_logsink_record_t *r, const char *s) {
    return r->len == (uint32_t)strlen(s) && memcmp(r->text, s, r->len) == 0;
}

/* ---- the tests ----------------------------------------------------------- */

static int test_logsink_writes_and_reads_back(void) {
    vibeos_logsink_record_t r;

    lt_wipe();
    if (lt_reattach() != 0) {
        return 0;
    }
    if (lt_say("first") != 0 || lt_say("second") != 0) {
        return 0;
    }
    /* 0 is the newest. A reader that got this backwards would still pass a
     * test that wrote one record, which is why there are two. */
    if (vibeos_logsink_read(0, &r) != 0 || !lt_is(&r, "second")) {
        return 0;
    }
    if (vibeos_logsink_read(1, &r) != 0 || !lt_is(&r, "first")) {
        return 0;
    }
    return vibeos_logsink_read(2, &r) != 0;
}

/* The one that matters most: a log that loses its last line loses the only
 * line anybody wanted. */
static int test_logsink_survives_a_reset(void) {
    vibeos_logsink_record_t r;

    lt_wipe();
    if (lt_reattach() != 0 || lt_say("before the reset") != 0) {
        return 0;
    }
    /* The machine stops. The medium does not. */
    if (lt_reattach() != 0) {
        return 0;
    }
    if (vibeos_logsink_read(0, &r) != 0 || !lt_is(&r, "before the reset")) {
        return 0;
    }
    /* And the previous boot's last sequence number was recovered, so this
     * boot's records sort after it rather than colliding with it.
     *
     * Asserted as "the next record sorts after the last one" rather than as
     * "highest_seq_seen is non-zero", which is what it used to say. That was
     * encoding a defect: an empty medium started at sequence 1, because attach
     * could not tell "nothing here" from "one record numbered zero". It starts
     * at 0 now, so the old assertion would fail on a perfectly good sink. The
     * torture found the defect; this test had been agreeing with it. */
    {
        uint64_t before = r.seq;
        if (lt_say("after the reset") != 0) {
            return 0;
        }
        if (vibeos_logsink_read(0, &r) != 0 || r.seq <= before) {
            return 0;
        }
    }
    if (!lt_is(&r, "after the reset")) {
        return 0;
    }
    return vibeos_logsink_read(1, &r) == 0 && lt_is(&r, "before the reset");
}

/* A wrap must not make the reader answer with a stale record. The checksum
 * covers the sequence number precisely so that an old slot cannot impersonate
 * a new one. */
static int test_logsink_wrap_keeps_ordering(void) {
    vibeos_logsink_record_t r;
    uint64_t cap;
    uint32_t i;
    char buf[16];

    lt_wipe();
    if (lt_reattach() != 0) {
        return 0;
    }
    cap = vibeos_logsink_capacity();
    if (cap != LT_SECTORS - 1u) {
        return 0;     /* sector 0 is not a record slot */
    }
    for (i = 0; i < (uint32_t)cap + 5u; i++) {
        buf[0] = (char)('a' + (i % 26u));
        buf[1] = 0;
        if (vibeos_logsink_write(buf, 1u) != 0) {
            return 0;
        }
    }
    /* The newest is still the newest after a wrap. */
    buf[0] = (char)('a' + ((cap + 4u) % 26u));
    buf[1] = 0;
    if (vibeos_logsink_read(0, &r) != 0 || !lt_is(&r, buf)) {
        return 0;
    }
    /* And a record the wrap has overwritten is reported as gone rather than as
     * whatever now occupies its slot. Asking for one older than the capacity
     * must miss. */
    return vibeos_logsink_read((uint32_t)cap + 1u, &r) != 0;
}

/* A record whose bytes were disturbed must not be handed back as if it were
 * intact - that is the difference between a log and a rumour. */
static int test_logsink_rejects_a_torn_record(void) {
    vibeos_logsink_record_t r;
    uint64_t slot;

    lt_wipe();
    if (lt_reattach() != 0 || lt_say("intact") != 0) {
        return 0;
    }
    if (vibeos_logsink_read(0, &r) != 0) {
        return 0;
    }
    slot = (r.seq % vibeos_logsink_capacity()) + 1ull;
    g_medium[slot][VIBEOS_LOGSINK_HEADER] ^= 0xFFu;
    return vibeos_logsink_read(0, &r) != 0;
}

/* A torn sequence number must not survive an attach.
 *
 * This is the test that makes the checksum-covers-the-sequence rule mean
 * something, and it was written *because* sabotaging that rule was caught by
 * nothing. The reader is not the place it matters: a read compares the
 * sequence it got against the one it asked for, so a wrong sequence is caught
 * there whatever the checksum covers.
 *
 * Attach is the place. It calls log_valid with no sequence to compare against
 * - there is nothing to compare against yet, that is what it is recovering -
 * so a slot whose sequence bytes were disturbed is taken at face value, and a
 * single flipped high bit sets the next sequence number to something enormous
 * for the rest of the medium life. The checksum is the only thing standing
 * between a torn sector and a log that can never wrap correctly again.
 */
static int test_logsink_a_torn_sequence_does_not_survive_attach(void) {
    vibeos_logsink_record_t r;
    uint64_t slot;

    lt_wipe();
    if (lt_reattach() != 0 || lt_say("intact") != 0) {
        return 0;
    }
    if (vibeos_logsink_read(0, &r) != 0) {
        return 0;
    }
    slot = (r.seq % vibeos_logsink_capacity()) + 1ull;
    /* The top byte of the sequence field: a plausible single-bit medium
     * failure, and the most damaging place for one. */
    g_medium[slot][11] ^= 0x80u;
    if (lt_reattach() != 0) {
        return 0;
    }
    /* The record is refused, so the enormous sequence is not adopted. */
    return vibeos_logsink_stats()->highest_seq_seen == 0ull &&
           vibeos_logsink_stats()->bad_records > 0ull;
}

/* A medium that refuses a write must be reported, not absorbed. A sink that
 * silently drops records is worse than no sink, because it is believed. */
static int test_logsink_counts_a_failed_write(void) {
    uint64_t failed_before;

    lt_wipe();
    if (lt_reattach() != 0) {
        return 0;
    }
    failed_before = vibeos_logsink_stats()->write_failed;
    g_fail_write_after = 0;
    if (lt_say("this cannot land") == 0) {
        return 0;     /* it must say so */
    }
    return vibeos_logsink_stats()->write_failed == failed_before + 1ull;
}

/* Two cores at once take different sequence numbers, so they take different
 * slots and cannot overwrite each other. Driven by switching the reported cpu
 * id between calls: the property under test is that the slot follows the
 * sequence number and nothing else. */
static int test_logsink_two_cores_do_not_share_a_slot(void) {
    vibeos_logsink_record_t a;
    vibeos_logsink_record_t b;

    lt_wipe();
    if (lt_reattach() != 0) {
        return 0;
    }
    g_cpu = 0;
    if (lt_say("from cpu 0") != 0) {
        return 0;
    }
    g_cpu = 3;
    if (lt_say("from cpu 3") != 0) {
        return 0;
    }
    if (vibeos_logsink_read(0, &b) != 0 || vibeos_logsink_read(1, &a) != 0) {
        return 0;
    }
    return b.seq != a.seq && lt_is(&b, "from cpu 3") && lt_is(&a, "from cpu 0");
}

/* A line longer than a record is truncated and said to be, rather than
 * refused. Half a line is evidence; no line is not. */
static int test_logsink_truncates_and_says_so(void) {
    static char big[VIBEOS_LOGSINK_PAYLOAD + 64u];
    vibeos_logsink_record_t r;
    uint64_t before;

    lt_wipe();
    if (lt_reattach() != 0) {
        return 0;
    }
    memset(big, 'x', sizeof(big));
    before = vibeos_logsink_stats()->truncated;
    if (vibeos_logsink_write(big, (uint32_t)sizeof(big)) != 0) {
        return 0;
    }
    if (vibeos_logsink_stats()->truncated != before + 1ull) {
        return 0;
    }
    return vibeos_logsink_read(0, &r) == 0 &&
           r.len == VIBEOS_LOGSINK_PAYLOAD;
}

/* A medium too small to hold anything is refused, rather than accepted and
 * discovered at the first write - when the caller is a panic handler. */
static int test_logsink_refuses_an_unusable_medium(void) {
    vibeos_logsink_dev_t d = lt_dev();

    lt_wipe();
    vibeos_logsink_reset();
    d.sectors = 1ull;             /* sector 0 only: no room for a record */
    if (vibeos_logsink_attach(&d) == 0) {
        return 0;
    }
    d.sectors = LT_SECTORS;
    d.write = 0;
    return vibeos_logsink_attach(&d) != 0;
}

/* Sector 0 is not ours. A module that wrote there would one day eat somebody's
 * partition table. */
static int test_logsink_leaves_sector_zero_alone(void) {
    lt_wipe();
    memset(g_medium[0], 0xA5, VIBEOS_LOGSINK_SECTOR);
    if (lt_reattach() != 0 || lt_say("hello") != 0) {
        return 0;
    }
    {
        uint32_t i;
        for (i = 0; i < VIBEOS_LOGSINK_SECTOR; i++) {
            if (g_medium[0][i] != 0xA5u) {
                return 0;
            }
        }
    }
    return 1;
}

int test_logsink(void) {
    struct { const char *name; int (*fn)(void); } cases[] = {
        { "writes and reads back", test_logsink_writes_and_reads_back },
        { "survives a reset", test_logsink_survives_a_reset },
        { "a wrap keeps ordering", test_logsink_wrap_keeps_ordering },
        { "rejects a torn record", test_logsink_rejects_a_torn_record },
        { "a torn sequence does not survive attach",
          test_logsink_a_torn_sequence_does_not_survive_attach },
        { "counts a failed write", test_logsink_counts_a_failed_write },
        { "two cores do not share a slot",
          test_logsink_two_cores_do_not_share_a_slot },
        { "truncates and says so", test_logsink_truncates_and_says_so },
        { "refuses an unusable medium",
          test_logsink_refuses_an_unusable_medium },
        { "leaves sector zero alone", test_logsink_leaves_sector_zero_alone },
    };
    unsigned i;
    int failures = 0;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        if (!cases[i].fn()) {
            printf("FAIL:logsink %s\n", cases[i].name);
            failures++;
        }
    }
    return failures == 0 ? 0 : -1;
}
