/* The kernel's own log, on disk (I5b). See include/vibeos/logsink.h for why.
 *
 * ---- the three decisions in here that are not obvious ----------------------
 *
 * **No lock on the write path.** The one context that most needs to write is a
 * panic handler, where another core may hold any lock and the scheduler is
 * parked - so a lock here is a way for the last line before a crash to be the
 * line that never arrives. Instead: the sequence number is a single atomic
 * fetch-add, the slot is derived from it, so two cores writing at the same
 * instant get different sequence numbers and therefore different sectors and
 * never touch each other's. The only other shared state is the staging buffer,
 * and there is one per core.
 *
 * **No header update per record.** The obvious design keeps a head pointer in
 * sector 0 and writes it after each record. That is a second thing to lose at
 * exactly the wrong moment, and it doubles the I/O on the path whose latency
 * matters most. Every record carries its own sequence number instead, and the
 * head is *recovered* by scanning at attach. Scanning costs one pass over the
 * medium once per boot; a lost head pointer costs the log.
 *
 * **Write-through, never write-back, and never through the block cache.** The
 * last few lines before a crash are the entire point. This is the same
 * ordering contract the barrier in I4 provides, used for the opposite reason:
 * the journal wants ordering so it can batch, this wants it to refuse to.
 *
 * ---- and one that is a trap ------------------------------------------------
 *
 * A record checksum covers its own header including the sequence number, and
 * the reason is narrower than it first looks. It is not what catches a stale
 * slot after a wrap - the reader compares the sequence it got against the one
 * it asked for, and that catches it whatever the checksum covers. The first
 * version of this comment claimed otherwise, and sabotaging the rule was
 * caught by nothing, which is how the claim was found to be wrong.
 *
 * Attach is where it matters. Recovering the head means calling log_valid with
 * no sequence to compare against - there is nothing to compare against yet -
 * so a slot whose sequence bytes were disturbed would be taken at face value,
 * and one flipped high bit would set the next sequence to something enormous
 * for the rest of the life of the medium. The checksum is the only thing
 * between a torn sector and a log that can never wrap correctly again.
 */

#include "vibeos/logsink.h"

#define LOG_MAGIC 0x564C4F47u   /* "VLOG" */

static vibeos_logsink_dev_t g_dev;
static int g_ready;
static uint64_t g_next_seq;
static uint64_t g_capacity;
static vibeos_logsink_stats_t g_stats;
static uint32_t (*g_cpu_id)(void);

/* One per core, so the write path needs no lock. See the note above. */
static uint8_t g_stage[VIBEOS_LOGSINK_MAX_CPUS][VIBEOS_LOGSINK_SECTOR];

static uint32_t log_cpu(void) {
    uint32_t id = g_cpu_id ? g_cpu_id() : 0u;
    return (id < VIBEOS_LOGSINK_MAX_CPUS) ? id : 0u;
}

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put64(uint8_t *p, uint64_t v) {
    put32(p, (uint32_t)v);
    put32(p + 4, (uint32_t)(v >> 32));
}

static uint64_t get64(const uint8_t *p) {
    return (uint64_t)get32(p) | ((uint64_t)get32(p + 4) << 32);
}

/* Not a cryptographic checksum and not trying to be. What it has to catch is a
 * torn sector and a stale slot, and for those a sum over every byte is enough.
 * It covers the sequence number deliberately - see the trap above. */
static uint32_t log_sum(const uint8_t *sec) {
    uint32_t sum = 0x811C9DC5u;
    uint32_t i;
    for (i = 0; i < VIBEOS_LOGSINK_SECTOR; i++) {
        if (i >= 20u && i < 24u) {
            continue;         /* the checksum field itself */
        }
        sum = (sum ^ sec[i]) * 16777619u;
    }
    return sum;
}

/*  0..3   magic
 *  4..11  seq
 * 12..15  len
 * 16..19  reserved
 * 20..23  checksum
 * 24..511 text
 */
static int log_valid(const uint8_t *sec, uint64_t *out_seq, uint32_t *out_len) {
    uint32_t len;

    if (get32(sec) != LOG_MAGIC) {
        return 0;
    }
    len = get32(sec + 12);
    if (len > VIBEOS_LOGSINK_PAYLOAD) {
        return 0;
    }
    if (get32(sec + 20) != log_sum(sec)) {
        return 0;
    }
    if (out_seq) {
        *out_seq = get64(sec + 4);
    }
    if (out_len) {
        *out_len = len;
    }
    return 1;
}

/* How many sectors the scan asks for at once when the medium offers it.
 *
 * Not a tuning knob: it is the difference between a bring-up that reads a 4 MiB
 * medium in a few dozen transfers and one that issues eight thousand. The
 * first version did the latter and the boot wedged - a slow thing during
 * bring-up does not look slow, it looks stopped. */
#define LOG_SCAN_BATCH 32u

int vibeos_logsink_attach(const vibeos_logsink_dev_t *dev) {
    static uint8_t batch[LOG_SCAN_BATCH][VIBEOS_LOGSINK_SECTOR];
    uint8_t sec[VIBEOS_LOGSINK_SECTOR];
    uint64_t highest = 0;
    int found_any = 0;
    uint64_t i;

    g_ready = 0;
    g_next_seq = 0;
    g_capacity = 0;
    if (!dev || !dev->read || !dev->write || dev->sectors < 2ull) {
        return -1;
    }
    g_dev = *dev;
    /* Sector 0 is left alone. There is no header to keep, and a medium whose
     * first sector this module owns is a medium somebody will one day put a
     * partition table on by accident. Records start at 1. */
    g_capacity = dev->sectors - 1ull;

    /* Recover the head by scanning. A boot pays one pass over the medium; the
     * alternative pays a second write per record and loses everything when
     * that write is the one that does not land. */
    i = 0;
    while (i < g_capacity) {
        uint32_t n = (uint32_t)((g_capacity - i < (uint64_t)LOG_SCAN_BATCH)
                              ? (g_capacity - i) : (uint64_t)LOG_SCAN_BATCH);
        uint32_t k;
        int got_batch = 0;

        if (g_dev.read_many &&
            g_dev.read_many(g_dev.ctx, i + 1ull, batch, n) == 0) {
            got_batch = 1;
        }
        for (k = 0; k < n; k++) {
            const uint8_t *p;
            uint64_t seq = 0;

            if (got_batch) {
                p = batch[k];
            } else {
                if (g_dev.read(g_dev.ctx, i + 1ull + k, sec) != 0) {
                    continue;   /* an unreadable slot is not a reason to have
                                 * no log */
                }
                p = sec;
            }
            g_stats.scanned++;
            if (!log_valid(p, &seq, 0)) {
                if (get32(p) == LOG_MAGIC) {
                    g_stats.bad_records++;
                }
                continue;
            }
            /* `found_any` rather than `highest != 0`: a medium holding
             * exactly one record, whose sequence is 0, is indistinguishable
             * from an empty one otherwise - so an empty medium started at 1
             * and burned sequence 0 for the life of the disk. Harmless, and
             * found on the first run of the torture, which is what an
             * independent model is for. */
            found_any = 1;
            if (seq > highest) {
                highest = seq;
            }
        }
        i += n;
    }
    g_stats.highest_seq_seen = highest;
    /* Sequence numbers are never reused, across reboots included. That is what
     * lets a reader order records over a wrap and over a reset, and tell "the
     * machine stopped" from "the log wrapped". */
    g_next_seq = found_any ? highest + 1ull : 0ull;
    g_ready = 1;
    return 0;
}

int vibeos_logsink_write(const char *text, uint32_t len) {
    uint8_t *sec;
    uint64_t seq;
    uint32_t i;

    if (!g_ready || !text) {
        return -1;
    }
    if (len > VIBEOS_LOGSINK_PAYLOAD) {
        /* Truncated rather than refused: half a line is evidence and no line
         * is not. Counted, so a medium that is losing the ends of its records
         * says so instead of quietly shortening them. */
        len = VIBEOS_LOGSINK_PAYLOAD;
        g_stats.truncated++;
    }

    seq = (uint64_t)__atomic_fetch_add(&g_next_seq, 1ull, __ATOMIC_SEQ_CST);
    sec = g_stage[log_cpu()];

    for (i = 0; i < VIBEOS_LOGSINK_SECTOR; i++) {
        sec[i] = 0;
    }
    put32(sec, LOG_MAGIC);
    put64(sec + 4, seq);
    put32(sec + 12, len);
    for (i = 0; i < len; i++) {
        sec[VIBEOS_LOGSINK_HEADER + i] = (uint8_t)text[i];
    }
    put32(sec + 20, log_sum(sec));

    if (g_dev.write(g_dev.ctx, (seq % g_capacity) + 1ull, sec) != 0) {
        g_stats.write_failed++;
        return -1;
    }
    g_stats.records_written++;
    return 0;
}

int vibeos_logsink_read(uint32_t back, vibeos_logsink_record_t *out) {
    uint8_t sec[VIBEOS_LOGSINK_SECTOR];
    uint64_t seq;
    uint64_t want;
    uint64_t got = 0;
    uint32_t len = 0;
    uint32_t i;

    if (!g_ready || !out) {
        return -1;
    }
    seq = __atomic_load_n(&g_next_seq, __ATOMIC_ACQUIRE);
    if (seq == 0ull || (uint64_t)back + 1ull > seq) {
        return -1;
    }
    want = seq - 1ull - (uint64_t)back;
    if (g_dev.read(g_dev.ctx, (want % g_capacity) + 1ull, sec) != 0) {
        return -1;
    }
    if (!log_valid(sec, &got, &len) || got != want) {
        /* The slot has been overwritten by a later wrap, or never held what
         * was asked for. Reported as a miss rather than as whatever is in the
         * sector - a reader that returns the wrong record is worse than one
         * that returns none. */
        return -1;
    }
    out->seq = got;
    out->len = len;
    for (i = 0; i < VIBEOS_LOGSINK_PAYLOAD; i++) {
        out->text[i] = (i < len) ? sec[VIBEOS_LOGSINK_HEADER + i] : 0u;
    }
    return 0;
}

uint64_t vibeos_logsink_capacity(void) {
    return g_capacity;
}

const vibeos_logsink_stats_t *vibeos_logsink_stats(void) {
    return &g_stats;
}

void vibeos_logsink_set_cpu_id(uint32_t (*fn)(void)) {
    g_cpu_id = fn;
}

void vibeos_logsink_reset(void) {
    uint32_t i;
    g_ready = 0;
    g_next_seq = 0;
    g_capacity = 0;
    g_cpu_id = 0;
    for (i = 0; i < sizeof(g_stats) / sizeof(uint64_t); i++) {
        ((uint64_t *)&g_stats)[i] = 0ull;
    }
}
