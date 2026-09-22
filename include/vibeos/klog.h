#ifndef VIBEOS_KLOG_H
#define VIBEOS_KLOG_H

#include <stdint.h>

#include "vibeos/log.h"

/* C6: the kernel log, as a module.
 *
 * This lived in arch_hw.c as a ring nobody locked, a formatter, a writer to the
 * serial line and a writer to the disk, each wired to the next by hand. Two
 * things about that arrangement were defects rather than untidiness:
 *
 * - The ring had no lock and every core wrote to it. hw_log recorded an event
 *   and then asked the ring for "the latest" one to print - which, with two
 *   cores logging at once, is whichever of the two landed second. One line was
 *   printed twice and the other never, and the ring's head could be claimed by
 *   both. Serialised by its own lock now, and the event printed is the copy
 *   taken under that lock, not a second look.
 * - A line reached the serial port as eight separate writes, each its own
 *   critical section, and it was bracketed only because somebody remembered to.
 *   Forgetting to is how a marker got cut in half and the boot gate reported
 *   crashes that had not happened. Here a line is formatted whole and handed to
 *   each sink in *one* call, so there is nothing to bracket.
 *
 * Everything above the device is the same answer on any architecture. The
 * device is a sink: a name, the quietest level it wants, and a write that takes
 * one complete line and says whether it took it. A sink that refuses a line is
 * counted (VIBEOS_MBZ_KLOG_LINE_LOST): a log that loses lines in silence is the
 * failure where the evidence is missing rather than wrong, which is the one
 * nobody can see. A sink that asks to be numbered gets " ln=0x..." on every
 * line, one sequence per sink, so a line the *device* dropped while reporting
 * success leaves a gap the boot gate can name.
 *
 * Serialised by its own lock, supplied by the architecture
 * (vibeos_klog_set_lock) - a registration function, not a weak symbol, because
 * a weak definition in another object does not resolve on the Windows build.
 * The lock covers the ring only. Sinks are written outside it: the serial port
 * is slow, and a slow device inside the ring's lock would serialise every
 * logging core behind it, which is the virtio-net lesson one layer up.
 *
 * Lock order, for anyone adding a caller: the console lock may be held while
 * this module takes its own (a dump does exactly that); this module never takes
 * the console lock or calls a sink while holding its own. */

#define VIBEOS_KLOG_MAX_SINKS 4u
#define VIBEOS_KLOG_MAX_CPUS 8u
/* A whole line: prefix, level, a 96-byte message, three 18-character fields and
 * the line number, with room to spare. Longer is truncated, never overrun. */
#define VIBEOS_KLOG_LINE 256u

/* Write one complete line. 0 when the device took all of it, anything else when
 * it did not. Called once per line, never with a partial one. */
typedef int (*vibeos_klog_write_fn)(void *ctx, const char *line, uint32_t len);

typedef struct {
    const char *name;
    vibeos_log_level_t min_level;  /* quieter events are recorded, not written here */
    const char *prefix;            /* written before the level, e.g. "[LOG]"; may be 0 */
    int numbered;                  /* append " ln=0x<n>", one sequence per sink */
    int newline;                   /* end the line with '\n' */
    vibeos_klog_write_fn write;
    void *ctx;
} vibeos_klog_sink_t;

typedef struct {
    const char *name;    /* the sink's, as registered */
    uint64_t offered;    /* lines handed to write; also the last ln= given out    */
    uint64_t lost;       /* of those, refused by the device - must be zero         */
    uint64_t reentered;  /* events raised from inside this sink's own write, and so
                          * not offered to it: a disk sink whose block layer logs a
                          * refusal would otherwise log, write, log, forever       */
} vibeos_klog_sink_stats_t;

/* The lock the ring is serialised with, and who is asking - the reentrancy guard
 * is per core, because two cores logging at once are not recursion. Either may be
 * left unset for a single-threaded host test. */
void vibeos_klog_set_lock(void (*lock)(void), void (*unlock)(void));
void vibeos_klog_set_cpu_id(uint32_t (*cpu_id)(void));

/* Boot and tests: the ring empty, no sinks, every count zero. Leaves the lock and
 * cpu id registered. Nothing is recorded before this has run. */
void vibeos_klog_reset(void);
int vibeos_klog_ready(void);

/* Register a sink; its index, or -1 when the table is full or the sink has no
 * write. Sinks are never removed: a device that goes away refuses its lines,
 * and refusing is counted. */
int vibeos_klog_add_sink(const vibeos_klog_sink_t *sink);
uint32_t vibeos_klog_sink_count(void);
int vibeos_klog_sink_stats(uint32_t index, vibeos_klog_sink_stats_t *out);

/* Record one event and write it to every sink whose level admits it. */
void vibeos_klog(vibeos_log_level_t level, uint32_t code, uint64_t arg0,
                 uint64_t arg1, const char *message);

/* The machine is stopping. "[FATAL] PANIC: <why>" to every sink, taking no lock
 * and consulting no level: another core may hold anything, and this line is the
 * one the on-disk log exists for. Not recorded in the ring and not numbered - the
 * ring is about to be dumped, and a number taken here could race a core that was
 * mid-line when the machine stopped. */
void vibeos_klog_panic(const char *why);

/* How many events the ring holds, and how many it has overwritten. */
int vibeos_klog_count(uint32_t *count, uint32_t *dropped);
/* Event `index`, oldest first, copied out under the lock. */
int vibeos_klog_get(uint32_t index, vibeos_log_event_t *out);

/* The newest `want` events (0: all of them) to `out`, one call per line, after a
 * header "[LOG] kernel ring: showing 0x.. of 0x..". For the console's `log`
 * command; the caller brackets the whole dump on its device if it wants it
 * unbroken. Each event is copied out under the ring's lock. */
void vibeos_klog_dump_recent(uint32_t want, vibeos_klog_write_fn out, void *ctx);

/* Every event, oldest first, as "[LOG] #<seq> [LOG][LEVEL] ...", after a header
 * "[LOG] dump count=0x.. dropped=0x..". For a panic: it takes *no* lock, because
 * the core that held it may be the one that faulted. */
void vibeos_klog_dump_unlocked(vibeos_klog_write_fn out, void *ctx);

#endif
