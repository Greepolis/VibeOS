/* The kernel's own log, on a medium that survives the machine (I5b).
 *
 * Not the same thing as kernel/fs/journal.c. That is a write-ahead journal for
 * filesystem consistency; this is the operating system recording what happened
 * so it can be read after the machine that recorded it has stopped. The two
 * share a word and nothing else, which has already caused one misunderstanding.
 *
 * Why it earns its place: every hard defect in this project was diagnosed from
 * a serial log - the silent wedge, the interleaved console, the copy-on-write
 * corruption, all of it. On a developer's machine QEMU captures that. On an
 * appliance, or on real hardware with no serial cable, a wedge leaves nothing,
 * and the evidence behind the last several fixes would not have existed.
 */
#ifndef VIBEOS_LOGSINK_H
#define VIBEOS_LOGSINK_H

#include <stdint.h>

#define VIBEOS_LOGSINK_SECTOR   512u
#define VIBEOS_LOGSINK_HEADER   24u
#define VIBEOS_LOGSINK_PAYLOAD  (VIBEOS_LOGSINK_SECTOR - VIBEOS_LOGSINK_HEADER)

/* At most this many cores may write concurrently without a lock. There is no
 * lock on the write path on purpose - see the .c file - and the per-core
 * staging buffers are what make that safe. */
#define VIBEOS_LOGSINK_MAX_CPUS 8u

typedef struct {
    uint64_t seq;                            /* monotonic, never reused      */
    uint32_t len;
    uint8_t  text[VIBEOS_LOGSINK_PAYLOAD];
} vibeos_logsink_record_t;

typedef struct {
    uint64_t records_written;
    uint64_t write_failed;
    uint64_t bad_records;      /* a slot whose checksum did not hold         */
    uint64_t scanned;          /* slots read at attach                       */
    uint64_t truncated;        /* a line longer than a record                */
    uint64_t highest_seq_seen; /* at attach: the previous machine's last one */
} vibeos_logsink_stats_t;

/* The medium, as two functions. Deliberately not a vibeos_blockdev_t and
 * deliberately not the block cache: a cache holding the last few lines when
 * the power goes is the one failure this feature cannot have. Every write here
 * goes straight at the device. */
typedef struct {
    int (*read)(void *ctx, uint64_t lba, void *buf);
    /* Optional, and only the attach scan uses it. A boot recovers the head by
     * reading the whole medium, and doing that a sector at a time made a 4 MiB
     * log cost thousands of synchronous transfers during bring-up - which
     * showed up as boots that wedged, not as a slow log. The write path
     * deliberately stays single-sector: it is one record, and batching there
     * would be the write-back this module exists to refuse. */
    int (*read_many)(void *ctx, uint64_t lba, void *buf, uint32_t sectors);
    int (*write)(void *ctx, uint64_t lba, const void *buf);
    void *ctx;
    uint64_t sectors;
} vibeos_logsink_dev_t;

/* Take over a medium. Scans it for the highest sequence number already there,
 * so records from this boot sort after the previous machine's.
 *
 * Returns 0 when the medium is usable. Does not allocate. */
int vibeos_logsink_attach(const vibeos_logsink_dev_t *dev);

/* One record, written through to the medium before this returns.
 *
 * Safe from a panic handler and from any core, without taking a lock. */
int vibeos_logsink_write(const char *text, uint32_t len);

/* Read back the nth-newest record. 0 is the newest. Returns 0 on success. */
int vibeos_logsink_read(uint32_t back, vibeos_logsink_record_t *out);

/* How many records the medium holds, at most. */
uint64_t vibeos_logsink_capacity(void);

const vibeos_logsink_stats_t *vibeos_logsink_stats(void);

/* Which core am I? Registered rather than assumed, so the portable module does
 * not have to know about APIC ids - and so the host tests can drive it. */
void vibeos_logsink_set_cpu_id(uint32_t (*fn)(void));

void vibeos_logsink_reset(void);

#endif /* VIBEOS_LOGSINK_H */
