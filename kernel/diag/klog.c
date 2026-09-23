/* The kernel log. See include/vibeos/klog.h for what this is and why it left
 * arch_hw.c; the comments here are about how. */

#include "vibeos/klog.h"
#include "vibeos/mbz.h"

static void (*g_lock)(void);
static void (*g_unlock)(void);
static uint32_t (*g_context)(void);

static vibeos_log_t g_ring;

static vibeos_klog_sink_t g_sinks[VIBEOS_KLOG_MAX_SINKS];
/* Published after the slot is filled, read before a slot is used: a sink added
 * while another core is logging is either seen whole or not at all. */
static volatile uint32_t g_nsinks;
static volatile uint64_t g_offered[VIBEOS_KLOG_MAX_SINKS];
static volatile uint64_t g_lost[VIBEOS_KLOG_MAX_SINKS];
static volatile uint64_t g_reentered[VIBEOS_KLOG_MAX_SINKS];
/* Written only by the context it belongs to - a task runs on one core at a time -
 * so a plain byte is enough. */
static volatile uint8_t g_busy[VIBEOS_KLOG_MAX_SINKS][VIBEOS_KLOG_MAX_CONTEXTS];

static void lock(void) {
    if (g_lock) {
        g_lock();
    }
}

static void unlock(void) {
    if (g_unlock) {
        g_unlock();
    }
}

/* Out of range cannot happen on the machine - the architecture asserts its
 * numbering fits at compile time - and would merge two contexts if it did, which
 * errs toward skipping a line, never toward recursing. */
static uint32_t this_context(void) {
    uint32_t id = g_context ? g_context() : 0u;
    return (id < VIBEOS_KLOG_MAX_CONTEXTS) ? id : VIBEOS_KLOG_MAX_CONTEXTS - 1u;
}

void vibeos_klog_set_lock(void (*l)(void), void (*u)(void)) {
    g_lock = l;
    g_unlock = u;
}

void vibeos_klog_set_context(uint32_t (*fn)(void)) {
    g_context = fn;
}

void vibeos_klog_reset(void) {
    uint32_t i, c;

    lock();
    (void)vibeos_log_init(&g_ring);
    g_nsinks = 0;
    for (i = 0; i < VIBEOS_KLOG_MAX_SINKS; i++) {
        g_offered[i] = 0;
        g_lost[i] = 0;
        g_reentered[i] = 0;
        for (c = 0; c < VIBEOS_KLOG_MAX_CONTEXTS; c++) {
            g_busy[i][c] = 0;
        }
    }
    unlock();
}

int vibeos_klog_ready(void) {
    return g_ring.initialized ? 1 : 0;
}

int vibeos_klog_add_sink(const vibeos_klog_sink_t *sink) {
    uint32_t n;

    if (!sink || !sink->write) {
        return -1;
    }
    lock();
    n = g_nsinks;
    if (n >= VIBEOS_KLOG_MAX_SINKS) {
        unlock();
        return -1;
    }
    g_sinks[n] = *sink;
    __atomic_store_n(&g_nsinks, n + 1u, __ATOMIC_RELEASE);
    unlock();
    return (int)n;
}

uint32_t vibeos_klog_sink_count(void) {
    return __atomic_load_n(&g_nsinks, __ATOMIC_ACQUIRE);
}

int vibeos_klog_sink_stats(uint32_t index, vibeos_klog_sink_stats_t *out) {
    if (!out || index >= vibeos_klog_sink_count()) {
        return -1;
    }
    out->name = g_sinks[index].name ? g_sinks[index].name : "?";
    out->offered = g_offered[index];
    out->lost = g_lost[index];
    out->reentered = g_reentered[index];
    return 0;
}

/* ---- text ----------------------------------------------------------------- */

typedef struct {
    char *out;
    uint32_t cap;
    uint32_t n;
} buf_t;

/* Always leaves room for the terminator, so a full buffer is a short line and
 * never an overrun. */
static void put_c(buf_t *b, char c) {
    if (b->n + 1u < b->cap) {
        b->out[b->n++] = c;
    }
}

static void put_s(buf_t *b, const char *s) {
    while (s && *s) {
        put_c(b, *s++);
    }
}

/* Sixteen digits, as the serial writer has always printed them: the boot gate's
 * patterns are written against that width. */
static void put_h(buf_t *b, uint64_t v) {
    static const char hex[] = "0123456789abcdef";
    int i;

    for (i = 15; i >= 0; i--) {
        put_c(b, hex[(v >> (i * 4)) & 0xFu]);
    }
}

static void put_x(buf_t *b, uint64_t v) {
    put_s(b, "0x");
    put_h(b, v);
}

static void put_event(buf_t *b, const vibeos_log_event_t *ev) {
    put_c(b, '[');
    put_s(b, vibeos_log_level_name((vibeos_log_level_t)ev->level));
    put_s(b, "] ");
    put_s(b, ev->message);
    if (ev->code != 0u || ev->arg0 != 0u || ev->arg1 != 0u) {
        put_s(b, " code=");
        put_x(b, ev->code);
        put_s(b, " a0=");
        put_x(b, ev->arg0);
        put_s(b, " a1=");
        put_x(b, ev->arg1);
    }
}

/* ---- writing -------------------------------------------------------------- */

/* The line one sink receives for one event: prefix, event, number, newline.
 *
 * One builder for the live path and the panic path. They were two, and the
 * torture found the panic's copy dropping the newline whenever a line had to be
 * cut - the forced-newline rule written once and not the second time. Returns
 * the length; `line` is always terminated. */
static uint32_t build_line(const vibeos_klog_sink_t *s, const vibeos_log_event_t *ev,
                           int numbered, uint64_t ln, char *line) {
    buf_t b;

    b.out = line;
    b.cap = VIBEOS_KLOG_LINE;
    b.n = 0;
    put_s(&b, s->prefix);
    put_event(&b, ev);
    if (numbered) {
        put_s(&b, " ln=");
        put_x(&b, ln);
    }
    if (s->newline) {
        /* Forced in even when the line was truncated: two lines run together is
         * exactly what the interleaving check exists to call a defect. */
        if (b.n + 2u > b.cap) {
            b.n = b.cap - 2u;
        }
        put_c(&b, '\n');
    }
    line[b.n] = 0;
    return b.n;
}

static void write_line(uint32_t i, const char *line, uint32_t n) {
    const vibeos_klog_sink_t *s = &g_sinks[i];

    if (s->write(s->ctx, line, n) != 0) {
        __atomic_fetch_add(&g_lost[i], 1ull, __ATOMIC_RELAXED);
        vibeos_mbz_hit(VIBEOS_MBZ_KLOG_LINE_LOST, (uint64_t)i);
    }
}

/* One event to one sink: the whole line in one call, and a refusal counted.
 *
 * The line number is taken before the write, so a device that drops a line and
 * says it did not leaves a gap in the numbers it did print - which is the only
 * way a lie from the device can be seen from outside it. */
static void offer(uint32_t i, const vibeos_log_event_t *ev) {
    const vibeos_klog_sink_t *s = &g_sinks[i];
    char line[VIBEOS_KLOG_LINE];
    uint32_t ctx = 0;
    uint64_t ln;

    if (ev->level < (uint32_t)s->min_level) {
        return;
    }
    if (s->guard_reentry) {
        ctx = this_context();
        if (g_busy[i][ctx]) {
            __atomic_fetch_add(&g_reentered[i], 1ull, __ATOMIC_RELAXED);
            return;
        }
        g_busy[i][ctx] = 1u;
    }
    ln = __atomic_add_fetch(&g_offered[i], 1ull, __ATOMIC_RELAXED);
    write_line(i, line, build_line(s, ev, s->numbered, ln, line));
    if (s->guard_reentry) {
        g_busy[i][ctx] = 0u;
    }
}

void vibeos_klog(vibeos_log_level_t level, uint32_t code, uint64_t arg0,
                 uint64_t arg1, const char *message) {
    vibeos_log_event_t ev;
    uint32_t i, n;
    int ok;

    if (!g_ring.initialized) {
        return;
    }
    /* Recorded and copied out in one critical section. Asking for "the latest"
     * after releasing the lock is how the old code printed another core's line
     * in place of its own. */
    lock();
    ok = vibeos_log_record(&g_ring, level, code, arg0, arg1, message) == 0 &&
         vibeos_log_latest(&g_ring, &ev) == 0;
    unlock();
    if (!ok) {
        return;
    }
    n = vibeos_klog_sink_count();
    for (i = 0; i < n; i++) {
        offer(i, &ev);
    }
}

void vibeos_klog_panic(const char *why) {
    vibeos_log_event_t ev;
    uint32_t i, k, n;
    const char *r = why ? why : "panic with no reason";

    ev.seq = 0;
    ev.level = (uint32_t)VIBEOS_LOG_FATAL;
    ev.code = 0;
    ev.arg0 = 0;
    ev.arg1 = 0;
    /* The bound first, then the read: a reason that is ever built rather than a
     * literal must not be read past its end. */
    for (k = 0; k < 7u; k++) {
        ev.message[k] = "PANIC: "[k];
    }
    for (i = 0; k + 1u < VIBEOS_LOG_MESSAGE_SIZE && r[i] != 0; i++, k++) {
        ev.message[k] = r[i];
    }
    ev.message[k] = 0;

    n = vibeos_klog_sink_count();
    for (i = 0; i < n; i++) {
        char line[VIBEOS_KLOG_LINE];

        write_line(i, line, build_line(&g_sinks[i], &ev, 0, 0, line));
    }
}

/* ---- reading -------------------------------------------------------------- */

int vibeos_klog_count(uint32_t *count, uint32_t *dropped) {
    int rc;

    lock();
    rc = (count ? vibeos_log_count(&g_ring, count) : 0) |
         (dropped ? vibeos_log_dropped(&g_ring, dropped) : 0);
    unlock();
    return rc != 0 ? -1 : 0;
}

int vibeos_klog_get(uint32_t index, vibeos_log_event_t *out) {
    int rc;

    lock();
    rc = vibeos_log_get(&g_ring, index, out);
    unlock();
    return rc;
}

/* Event number `seq`, if the ring still holds it. Under the lock, so the index it
 * computes and the event it reads belong to the same state of the ring. */
static int get_by_seq(uint64_t seq, vibeos_log_event_t *out) {
    vibeos_log_event_t newest;
    uint32_t count = 0;
    int rc = -1;

    lock();
    if (vibeos_log_count(&g_ring, &count) == 0 && count != 0u &&
        vibeos_log_latest(&g_ring, &newest) == 0 &&
        seq <= newest.seq && newest.seq - seq < (uint64_t)count &&
        vibeos_log_get(&g_ring, count - 1u - (uint32_t)(newest.seq - seq), out) == 0 &&
        out->seq == seq) {
        rc = 0;
    }
    unlock();
    return rc;
}

void vibeos_klog_dump_recent(uint32_t want, vibeos_klog_write_fn out, void *ctx) {
    vibeos_log_event_t newest;
    uint32_t count = 0, shown, k;
    uint64_t first;
    char line[VIBEOS_KLOG_LINE];
    buf_t b;
    int ok;

    if (!out) {
        return;
    }
    /* What to show is fixed here, as a range of sequence numbers, in one
     * critical section. It used to be a range of *indices*, re-read one lock at a
     * time: with the ring full, every event another core logged during the dump
     * moved them all by one, and the dump showed a neighbour twice or skipped one
     * with nothing to say so. */
    lock();
    ok = vibeos_log_count(&g_ring, &count) == 0 &&
         (count == 0u || vibeos_log_latest(&g_ring, &newest) == 0);
    unlock();
    if (!ok) {
        static const char msg[] = "[LOG] kernel ring unavailable\n";
        (void)out(ctx, msg, (uint32_t)(sizeof(msg) - 1u));
        return;
    }
    shown = (want == 0u || count <= want) ? count : want;
    first = count ? newest.seq - shown + 1u : 0u;

    b.out = line;
    b.cap = sizeof(line);
    b.n = 0;
    put_s(&b, "[LOG] kernel ring: showing ");
    put_x(&b, (uint64_t)shown);
    put_s(&b, " of ");
    put_x(&b, (uint64_t)count);
    put_c(&b, '\n');
    line[b.n] = 0;
    (void)out(ctx, line, b.n);

    /* Exactly `shown` lines follow the header, whatever happens meanwhile - the
     * gate counts them. An event overwritten before its turn says so. */
    for (k = 0; k < shown; k++) {
        vibeos_log_event_t ev;

        b.n = 0;
        /* One event per critical section, and never a device call inside one. */
        if (get_by_seq(first + k, &ev) == 0) {
            put_s(&b, "[LOG]");
            put_event(&b, &ev);
        } else {
            put_s(&b, "[LOG] #");
            put_h(&b, first + k);
            put_s(&b, " overwritten before it could be shown");
        }
        put_c(&b, '\n');
        line[b.n] = 0;
        (void)out(ctx, line, b.n);
    }
}

void vibeos_klog_dump_unlocked(vibeos_klog_write_fn out, void *ctx) {
    uint32_t count = 0, dropped = 0, i;
    char line[VIBEOS_KLOG_LINE];
    buf_t b;

    if (!out || vibeos_log_count(&g_ring, &count) != 0) {
        return;
    }
    (void)vibeos_log_dropped(&g_ring, &dropped);

    b.out = line;
    b.cap = sizeof(line);
    b.n = 0;
    put_s(&b, "[LOG] dump count=");
    put_x(&b, count);
    put_s(&b, " dropped=");
    put_x(&b, dropped);
    put_c(&b, '\n');
    line[b.n] = 0;
    (void)out(ctx, line, b.n);

    for (i = 0; i < count; i++) {
        vibeos_log_event_t ev;

        if (vibeos_log_get(&g_ring, i, &ev) != 0) {
            continue;
        }
        b.n = 0;
        put_s(&b, "[LOG] #");
        put_h(&b, ev.seq);
        put_s(&b, " [LOG]");
        put_event(&b, &ev);
        put_c(&b, '\n');
        line[b.n] = 0;
        (void)out(ctx, line, b.n);
    }
}
