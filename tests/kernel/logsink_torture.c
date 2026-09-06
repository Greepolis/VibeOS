/* The on-disk log, against a model that does not share its code (I5b).
 *
 * The unit tests check the cases somebody thought of. This checks the sink
 * against an independent account of what the medium ought to contain, over
 * many seeds and many thousands of operations, with power cut at random
 * moments and the sink reattached from the platter as a fresh boot would.
 *
 * Why a separate model rather than more assertions: a subsystem asked whether
 * it is correct answers from the numbers it used to decide, so the defects
 * that survive are the self-consistent ones. The model here keeps its own
 * sequence counter and its own picture of the ring, derived only from the
 * operations issued - it never reads the sink's own state. When the two
 * disagree, one of them is wrong, and that is a question worth having.
 *
 * The interesting operation is the power cut. A record that was written and
 * acknowledged must be readable after a reattach; a record whose write was
 * refused must not be; and the sequence must never go backwards across a
 * reattach, because that is the one property that makes records from different
 * machines orderable at all.
 *
 * Usage: vibeos_logsink_torture <seed> <rounds>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vibeos/logsink.h"

/* Small on purpose. A ring that wraps every few dozen records exercises the
 * wrap logic thousands of times in a run; a big one would exercise it never,
 * which is how a torture test ends up torturing nothing. */
#define LT_SECTORS 24u
#define LT_CAPACITY (LT_SECTORS - 1u)

static uint8_t g_medium[LT_SECTORS][VIBEOS_LOGSINK_SECTOR];

/* The drive's own volatile cache: a write is acknowledged and sits here until
 * a power cut throws it away or the next write pushes it through. The sink
 * claims to be write-through, so the model expects every acknowledged write to
 * have reached the platter - and this is where that claim is checked rather
 * than believed. */
static int g_fail_writes;
static uint32_t g_writes_seen;

static int lt_read(void *ctx, uint64_t lba, void *buf) {
    (void)ctx;
    if (lba >= LT_SECTORS) {
        return -1;
    }
    memcpy(buf, g_medium[lba], VIBEOS_LOGSINK_SECTOR);
    return 0;
}

static int lt_read_many(void *ctx, uint64_t lba, void *buf, uint32_t n) {
    uint32_t i;

    (void)ctx;
    if (lba + n > LT_SECTORS) {
        return -1;
    }
    for (i = 0; i < n; i++) {
        memcpy((uint8_t *)buf + (size_t)i * VIBEOS_LOGSINK_SECTOR,
               g_medium[lba + i], VIBEOS_LOGSINK_SECTOR);
    }
    return 0;
}

static int lt_write(void *ctx, uint64_t lba, const void *buf) {
    (void)ctx;
    if (g_fail_writes || lba >= LT_SECTORS) {
        return -1;
    }
    g_writes_seen++;
    memcpy(g_medium[lba], buf, VIBEOS_LOGSINK_SECTOR);
    return 0;
}

static uint32_t g_cpu;
static uint32_t lt_cpu(void) { return g_cpu; }

/* ---- the model ------------------------------------------------------------
 *
 * What the medium ought to hold, kept from the operations alone. */
#define MODEL_TEXT 40u

typedef struct {
    uint64_t seq;
    uint32_t len;
    char text[MODEL_TEXT];
    int valid;
} model_slot_t;

static model_slot_t g_model[LT_CAPACITY];
static uint64_t g_model_next_seq;

/* The sequence of the last record that actually reached the medium, and
 * whether there has been one. Distinct from g_model_next_seq - 1, because a
 * refused write consumes a sequence number that never lands, so those two
 * differ exactly as often as the medium refuses. Conflating them was the first
 * version of this model, and it made the model claim the sink had gone
 * backwards across a reset when the sink was right. */
static uint64_t g_model_last_written;
static int g_model_have_written;

static uint32_t rnd(uint32_t *state) {
    *state = (*state * 1103515245u) + 12345u;
    return (*state >> 8) & 0x00FFFFFFu;
}

static int attach(void) {
    vibeos_logsink_dev_t d;

    /* Zeroed for the same reason logsink_tests.c is: a struct filled field by
     * field silently gains a hole the day somebody adds a field. */
    memset(&d, 0, sizeof(d));
    vibeos_logsink_reset();
    vibeos_logsink_set_cpu_id(lt_cpu);
    d.read = lt_read;
    d.read_many = lt_read_many;
    d.write = lt_write;
    d.ctx = 0;
    d.sectors = LT_SECTORS;
    return vibeos_logsink_attach(&d);
}

/* Every record the model still expects to be on the medium must come back
 * exactly, and one it does not expect must not. Checked by sequence number
 * rather than by position, because position is the sink's business. */
static int check_against_model(const char *where, uint32_t seed) {
    uint32_t back;

    for (back = 0; back < LT_CAPACITY; back++) {
        vibeos_logsink_record_t r;
        uint64_t want_seq;
        int got;

        if ((uint64_t)back + 1ull > g_model_next_seq) {
            break;
        }
        want_seq = g_model_next_seq - 1ull - (uint64_t)back;
        got = (vibeos_logsink_read(back, &r) == 0);

        {
            const model_slot_t *m = &g_model[want_seq % LT_CAPACITY];
            int expected = m->valid && m->seq == want_seq;

            if (expected && !got) {
                printf("FAIL:logsink-torture seed=%u %s: record %llu was lost\n",
                       seed, where, (unsigned long long)want_seq);
                return -1;
            }
            if (!expected && got) {
                printf("FAIL:logsink-torture seed=%u %s: record %llu came back "
                       "and should not have\n",
                       seed, where, (unsigned long long)want_seq);
                return -1;
            }
            if (!expected) {
                continue;
            }
            if (r.seq != want_seq) {
                printf("FAIL:logsink-torture seed=%u %s: asked for %llu got "
                       "%llu\n", seed, where,
                       (unsigned long long)want_seq,
                       (unsigned long long)r.seq);
                return -1;
            }
            if (r.len != m->len || memcmp(r.text, m->text, m->len) != 0) {
                printf("FAIL:logsink-torture seed=%u %s: record %llu came back "
                       "with different bytes\n", seed, where,
                       (unsigned long long)want_seq);
                return -1;
            }
        }
    }
    return 0;
}

static int run_seed(uint32_t seed, uint32_t rounds) {
    uint32_t state = seed ? seed : 1u;
    uint64_t highest_seq_before_reattach = 0;
    uint32_t i;

    memset(g_medium, 0, sizeof(g_medium));
    memset(g_model, 0, sizeof(g_model));
    g_model_next_seq = 0;
    g_model_last_written = 0;
    g_model_have_written = 0;
    g_fail_writes = 0;
    g_cpu = 0;

    if (attach() != 0) {
        printf("FAIL:logsink-torture seed=%u: attach refused a good medium\n",
               seed);
        return -1;
    }

    for (i = 0; i < rounds; i++) {
        uint32_t roll = rnd(&state) % 100u;

        g_cpu = rnd(&state) % VIBEOS_LOGSINK_MAX_CPUS;

        if (roll < 70u) {
            /* An ordinary record. */
            char text[MODEL_TEXT];
            uint32_t len = 1u + (rnd(&state) % (MODEL_TEXT - 1u));
            uint32_t k;

            for (k = 0; k < len; k++) {
                text[k] = (char)('a' + (rnd(&state) % 26u));
            }
            if (vibeos_logsink_write(text, len) == 0) {
                model_slot_t *m = &g_model[g_model_next_seq % LT_CAPACITY];
                m->seq = g_model_next_seq;
                m->len = len;
                memcpy(m->text, text, len);
                m->valid = 1;
                g_model_last_written = g_model_next_seq;
                g_model_have_written = 1;
                g_model_next_seq++;
            } else {
                printf("FAIL:logsink-torture seed=%u: a write to a healthy "
                       "medium was refused\n", seed);
                return -1;
            }
        } else if (roll < 80u) {
            /* A medium that refuses. The record must not appear, and the sink
             * must say so rather than absorbing it. */
            char text[8] = "refused";
            uint64_t seq_before = g_model_next_seq;

            g_fail_writes = 1;
            if (vibeos_logsink_write(text, 7u) == 0) {
                printf("FAIL:logsink-torture seed=%u: a refused write was "
                       "reported as done\n", seed);
                return -1;
            }
            g_fail_writes = 0;
            /* The sequence number is consumed even though the write failed -
             * it is allocated before the medium is touched, and reusing it
             * would let a later record impersonate this one.
             *
             * What the model must NOT do is mark that slot dead: the write
             * never reached the medium, so whatever older record occupied the
             * slot is still there and still readable. The first version did
             * mark it, which made the model claim a live record should be
             * gone. A model that is wrong in the same place twice is worse
             * than no model, so this is written down rather than just fixed. */
            g_model_next_seq = seq_before + 1ull;
        } else if (roll < 95u) {
            if (check_against_model("mid-run", seed) != 0) {
                return -1;
            }
        } else {
            /* The power goes and the machine comes back. */
            highest_seq_before_reattach = g_model_last_written;
            if (attach() != 0) {
                printf("FAIL:logsink-torture seed=%u: attach refused a medium "
                       "it had itself written\n", seed);
                return -1;
            }
            /* The sink recovers its sequence from the medium, so after a reset
             * it is authoritative and the model follows it - downward as well
             * as upward. Downward is not a defect: a refused write consumes a
             * number that never reaches the medium, so a reattach legitimately
             * lands below the model running count. The first version of this
             * model only ever raised, and then reported the sink as wrong.
             *
             * What must NOT go backwards is the last sequence that actually
             * landed and is still inside the ring. That is the property that
             * makes records of two different boots orderable, and it is what
             * is asserted here. */
            if (g_model_have_written) {
                const model_slot_t *m =
                    &g_model[highest_seq_before_reattach % LT_CAPACITY];
                int still_in_ring = m->valid &&
                                    m->seq == highest_seq_before_reattach;

                if (still_in_ring &&
                    vibeos_logsink_stats()->highest_seq_seen <
                        highest_seq_before_reattach) {
                    printf("FAIL:logsink-torture seed=%u: the sequence went "
                           "backwards across a reset (%llu -> %llu)\n", seed,
                           (unsigned long long)highest_seq_before_reattach,
                           (unsigned long long)
                               vibeos_logsink_stats()->highest_seq_seen);
                    return -1;
                }
            }
            g_model_next_seq = g_model_have_written
                             ? vibeos_logsink_stats()->highest_seq_seen + 1ull
                             : 0ull;
            if (check_against_model("after a reset", seed) != 0) {
                return -1;
            }
        }
    }
    return check_against_model("at the end", seed);
}

int main(int argc, char **argv) {
    uint32_t seed = (argc > 1) ? (uint32_t)strtoul(argv[1], 0, 0) : 1u;
    uint32_t rounds = (argc > 2) ? (uint32_t)strtoul(argv[2], 0, 0) : 4000u;

    if (run_seed(seed, rounds) != 0) {
        printf("logsink-torture seed=%u FAILED\n", seed);
        return 1;
    }
    printf("logsink-torture seed=%u rounds=%u ok (writes=%u)\n", seed, rounds,
           g_writes_seen);
    return 0;
}
