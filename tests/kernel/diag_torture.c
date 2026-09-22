/* Randomised torture for kernel/diag (the kernel log and the crash records),
 * against a reference model.
 *
 * The host tests check the cases somebody thought of. This runs long random
 * sequences - events at random levels into randomly configured sinks that
 * refuse lines, log from inside their own writes (nested), and are stopped by
 * panics; dumps of random depth; crash records of random shape - and checks
 * every line every sink receives against a model kept independently.
 *
 * Independence is the point. The model formats its expected lines with
 * snprintf, not with the module's formatter, and keeps its own account of the
 * ring, of which sink is mid-write, and of every counter. A log asked whether
 * it wrote the right line answers from the same code that wrote it; the defects
 * worth finding are the self-consistent ones - a line number shared between two
 * sinks, a reentrancy guard left set, a truncated line that loses its newline -
 * and a model that shares no code with the module is what can see them.
 *
 * It prints its seed on the first line, so a failure can be replayed exactly:
 *
 *     vibeos_diag_torture <seed> [rounds]
 *
 * ## What is checked
 *
 * - **Every line, as it is written**: the right sink, the right event (the
 *   innermost one in progress), the exact text including prefix, truncation,
 *   line number and newline, written exactly once per admitting sink, and never
 *   with the ring's lock held.
 * - **Reentrancy**: an event raised inside sink j's write is not offered to j
 *   (or to any sink further out on the call stack), is counted as reentered
 *   there, and reaches every other sink normally.
 * - **Counters**: offered, lost and reentered per sink, and the must-be-zero
 *   registry's klog_line_lost, equal the model's after every round.
 * - **The ring**: count, dropped, and random events by index against the
 *   model's copy, through several wraps of the ring's capacity.
 * - **Dumps**: dump_recent of a random depth, line for line.
 * - **The lock**: never nested, never released unheld, never held on return.
 * - **Crash records**: the latest four by position against the model's ring,
 *   the count, and the full dump of the latest, line for line.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vibeos/crash.h"
#include "vibeos/klog.h"
#include "vibeos/mbz.h"

static uint64_t g_rng;

static uint64_t rnd(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return g_rng;
}

static uint32_t below(uint32_t n) {
    return n ? (uint32_t)(rnd() % n) : 0u;
}

static int g_failed;
static uint64_t g_round;

static void fail(const char *fmt, ...) {
    va_list ap;

    if (g_failed++ < 8) {
        printf("FAIL round %llu: ", (unsigned long long)g_round);
        va_start(ap, fmt);
        vprintf(fmt, ap);
        va_end(ap);
        printf("\n");
        /* Flushed, because the next step may be the crash this line predicts:
         * a record that lies about its size was named here, then lost with the
         * buffer when the dump walked off the array and took the process. */
        fflush(stdout);
    }
}

/* ---- the lock --------------------------------------------------------------- */

static int g_held;

/* Locks taken at each depth of the event stack (see log_one), so each call can
 * be held to exactly one: a torture that only checks the lock is balanced cannot
 * see a ring recorded with no lock at all. */
#define M_DEPTH 8
static int m_depth;
static int m_locks_at[M_DEPTH + 2];

/* A second core, logging in the instant this one lets go of the ring. Armed
 * only inside vibeos_klog, so a check that is reading the ring is not changed
 * under it. The torture runs on one thread; this is how it sees what the ring's
 * lock is for - an event printed as "the latest" after the lock was dropped is
 * the other core's, which is what hw_log used to do. */
static int g_second_core_armed;
static int g_in_second_core;
static int m_in_panic;
static void log_one(int nested);

static void t_lock(void) {
    if (g_held != 0) {
        fail("the ring's lock was taken while already held");
    }
    g_held++;
    m_locks_at[m_depth < M_DEPTH + 1 ? m_depth : M_DEPTH + 1]++;
}

static void t_unlock(void) {
    if (g_held != 1) {
        fail("the ring's lock was released while held %d times", g_held);
        return;
    }
    g_held--;
    if (g_second_core_armed && !g_in_second_core && !m_in_panic &&
        m_depth > 0 && m_depth < M_DEPTH && below(6) == 0) {
        g_in_second_core = 1;
        log_one(1);
        g_in_second_core = 0;
    }
}

/* ---- the model ---------------------------------------------------------------- */

#define M_CAP VIBEOS_LOG_CAPACITY
#define M_MSG (VIBEOS_LOG_MESSAGE_SIZE - 1u)

typedef struct {
    uint32_t level;
    uint32_t code;
    uint64_t a0, a1;
    char msg[VIBEOS_LOG_MESSAGE_SIZE];
    uint64_t seq;
} m_event_t;

static m_event_t m_ring[M_CAP];
static uint64_t m_total;
static uint64_t m_most;     /* the most events one machine saw: did the ring wrap? */

typedef struct {
    int min_level;
    char prefix[240];
    int has_prefix;
    int numbered;
    int newline;
    uint32_t fail_one_in;     /* 0: never refuses */
    uint32_t reenter_one_in;  /* 0: never logs from inside */
    uint64_t offered, lost, reentered;
} m_sink_t;

static m_sink_t m_sinks[VIBEOS_KLOG_MAX_SINKS];
static uint32_t m_nsinks;
static int m_busy[VIBEOS_KLOG_MAX_SINKS];

/* The events in progress, innermost last, and which sinks each has written. */
static m_event_t m_stack[M_DEPTH];
static int m_written[M_DEPTH][VIBEOS_KLOG_MAX_SINKS];
static char m_panic_reason[128];
static int m_panic_written[VIBEOS_KLOG_MAX_SINKS];
static uint64_t m_lost_total;

static const char *level_name(uint32_t l) {
    static const char *const n[] = { "DEBUG", "INFO", "WARN", "ERROR", "FATAL" };
    return l < 5u ? n[l] : "UNKNOWN";
}

/* The body of a line, independently: "[LEVEL] msg" and the fields when any is
 * non-zero, exactly as the serial writer has always printed them. */
static int m_body(char *out, size_t cap, const m_event_t *e) {
    int n = snprintf(out, cap, "[%s] %s", level_name(e->level), e->msg);
    if (e->code || e->a0 || e->a1) {
        n += snprintf(out + n, cap - (size_t)n,
                      " code=0x%016llx a0=0x%016llx a1=0x%016llx",
                      (unsigned long long)e->code, (unsigned long long)e->a0,
                      (unsigned long long)e->a1);
    }
    return n;
}

/* What the sink must receive: prefix, body, number, then the cut to the line
 * size and the newline forced back in. Written from the contract in klog.h. */
static size_t m_line(char *out, const m_sink_t *s, const m_event_t *e, uint64_t ln,
                     int numbered, int newline) {
    static char full[2048];
    size_t n;

    n = (size_t)snprintf(full, sizeof(full), "%s", s->has_prefix ? s->prefix : "");
    n += (size_t)m_body(full + n, sizeof(full) - n, e);
    if (numbered) {
        n += (size_t)snprintf(full + n, sizeof(full) - n, " ln=0x%016llx",
                              (unsigned long long)ln);
    }
    if (n > VIBEOS_KLOG_LINE - 1u) {
        n = VIBEOS_KLOG_LINE - 1u;
    }
    if (newline) {
        if (n + 2u > VIBEOS_KLOG_LINE) {
            n = VIBEOS_KLOG_LINE - 2u;
        }
        full[n++] = '\n';
    }
    memcpy(out, full, n);
    out[n] = 0;
    return n;
}

static void m_record(const m_event_t *e) {
    m_event_t *slot = &m_ring[m_total % M_CAP];
    *slot = *e;
    m_total++;
    slot->seq = m_total;
}

/* A deliberate cut to `cap` bytes including the terminator. */
static void copy_cut(char *dst, const char *src, size_t cap) {
    size_t n = strlen(src);
    if (n > cap - 1u) {
        n = cap - 1u;
    }
    memcpy(dst, src, n);
    dst[n] = 0;
}

/* ---- the sinks ---------------------------------------------------------------- */


static int t_write(void *ctx, const char *line, uint32_t len) {
    uint32_t j = (uint32_t)(uintptr_t)ctx;
    m_sink_t *s = &m_sinks[j];
    char want[VIBEOS_KLOG_LINE + 4];
    size_t wn;
    int refuse;

    if (g_held != 0) {
        fail("sink %u written with the ring's lock held", j);
    }
    if (line[len] != 0 || strlen(line) != len) {
        fail("sink %u: length %u is not the line's", j, len);
    }
    if (m_in_panic) {
        m_event_t e;
        memset(&e, 0, sizeof(e));
        e.level = 4;
        memcpy(e.msg, "PANIC: ", 7u);
        copy_cut(e.msg + 7, m_panic_reason, sizeof(e.msg) - 7u);
        wn = m_line(want, s, &e, 0, 0, s->newline);
        if (m_panic_written[j]++) {
            fail("sink %u given the panic line twice", j);
        }
        if (wn != len || memcmp(want, line, len) != 0) {
            fail("sink %u panic line\n  want '%s'\n  got  '%s'", j, want, line);
        }
        refuse = s->fail_one_in && below(s->fail_one_in) == 0;
        if (refuse) {
            s->lost++;
            m_lost_total++;
        }
        return refuse ? -1 : 0;
    }

    if (m_depth <= 0) {
        fail("sink %u written with no event in progress", j);
        return 0;
    }
    {
        const m_event_t *e = &m_stack[m_depth - 1];
        if (m_busy[j]) {
            fail("sink %u written while it is already mid-write", j);
        }
        if ((int)e->level < s->min_level) {
            fail("sink %u given a level-%u event below its minimum %d", j, e->level,
                 s->min_level);
        }
        if (m_written[m_depth - 1][j]++) {
            fail("sink %u given one event twice", j);
        }
        s->offered++;
        wn = m_line(want, s, e, s->offered, s->numbered, s->newline);
        if (wn != len || memcmp(want, line, len) != 0) {
            fail("sink %u line\n  want '%s'\n  got  '%s'", j, want, line);
        }
    }

    refuse = s->fail_one_in && below(s->fail_one_in) == 0;
    if (refuse) {
        s->lost++;
        m_lost_total++;
    }
    /* Log from inside the write, which the disk sink's block layer really does. */
    if (s->reenter_one_in && below(s->reenter_one_in) == 0 && m_depth < M_DEPTH) {
        m_busy[j] = 1;
        log_one(1);
        m_busy[j] = 0;
    }
    return refuse ? -1 : 0;
}

/* ---- one event, through the module and the model -------------------------------- */

static void random_text(char *out, uint32_t n) {
    static const char set[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 _-=:/.()[]";
    uint32_t i;
    for (i = 0; i < n; i++) {
        out[i] = set[below((uint32_t)(sizeof(set) - 1u))];
    }
    out[n] = 0;
}

static void log_one(int nested) {
    char msg[160];
    m_event_t e;
    uint32_t j;
    int d;

    memset(&e, 0, sizeof(e));
    e.level = below(5);
    if (below(3) != 0) {
        e.code = (uint32_t)rnd();
        e.a0 = rnd();
        e.a1 = below(4) == 0 ? 0 : rnd();
    }
    random_text(msg, below(nested ? 40u : 140u));
    copy_cut(e.msg, msg, sizeof(e.msg));   /* the ring keeps 95 of them */

    d = m_depth++;
    m_stack[d] = e;
    for (j = 0; j < VIBEOS_KLOG_MAX_SINKS; j++) {
        m_written[d][j] = 0;
    }
    m_record(&e);

    {
        int armed = g_second_core_armed;
        m_locks_at[d + 1] = 0;
        g_second_core_armed = 1;
        vibeos_klog((vibeos_log_level_t)e.level, e.code, e.a0, e.a1, msg);
        g_second_core_armed = armed;
        /* Nested events count at their own depth, so this is this call's alone. */
        if (m_locks_at[d + 1] != 1) {
            fail("recording one event took the ring's lock %d times (want 1)",
                 m_locks_at[d + 1]);
        }
    }

    /* Every admitting sink not on the call stack got exactly one write; every
     * admitting sink that was on it got none and counted the reentry. */
    for (j = 0; j < m_nsinks; j++) {
        m_sink_t *s = &m_sinks[j];
        if ((int)e.level < s->min_level) {
            if (m_written[d][j]) {
                fail("sink %u written below its level", j);
            }
            continue;
        }
        if (m_busy[j]) {
            s->reentered++;
            if (m_written[d][j]) {
                fail("sink %u given an event raised inside its own write", j);
            }
        } else if (m_written[d][j] != 1) {
            fail("sink %u written %d times for one event (want 1)", j, m_written[d][j]);
        }
    }
    m_depth--;
}

/* ---- checks after each round -------------------------------------------------- */

static char g_dump[VIBEOS_KLOG_LINE * 80];
static size_t g_dump_len;

static int t_dump(void *ctx, const char *line, uint32_t len) {
    (void)ctx;
    if (g_dump_len + len < sizeof(g_dump)) {
        memcpy(g_dump + g_dump_len, line, len);
        g_dump_len += len;
        g_dump[g_dump_len] = 0;
    }
    return 0;
}

static void check_counters(uint64_t mbz_base) {
    uint32_t j;
    for (j = 0; j < m_nsinks; j++) {
        vibeos_klog_sink_stats_t st;
        if (vibeos_klog_sink_stats(j, &st) != 0) {
            fail("sink %u has no stats", j);
            continue;
        }
        if (st.offered != m_sinks[j].offered || st.lost != m_sinks[j].lost ||
            st.reentered != m_sinks[j].reentered) {
            fail("sink %u stats offered/lost/reentered %llu/%llu/%llu, model %llu/%llu/%llu",
                 j, (unsigned long long)st.offered, (unsigned long long)st.lost,
                 (unsigned long long)st.reentered,
                 (unsigned long long)m_sinks[j].offered,
                 (unsigned long long)m_sinks[j].lost,
                 (unsigned long long)m_sinks[j].reentered);
        }
    }
    if (vibeos_mbz_count(VIBEOS_MBZ_KLOG_LINE_LOST) - mbz_base != m_lost_total) {
        fail("klog_line_lost moved by %llu, the model lost %llu",
             (unsigned long long)(vibeos_mbz_count(VIBEOS_MBZ_KLOG_LINE_LOST) - mbz_base),
             (unsigned long long)m_lost_total);
    }
}

static void check_ring(void) {
    uint32_t count = 0, dropped = 0, want_count, i, k;
    vibeos_log_event_t ev;

    want_count = m_total < M_CAP ? (uint32_t)m_total : M_CAP;
    if (vibeos_klog_count(&count, &dropped) != 0 || count != want_count ||
        (uint64_t)dropped != (m_total > M_CAP ? m_total - M_CAP : 0u)) {
        fail("ring count/dropped %u/%u, model %u/%llu", count, dropped, want_count,
             (unsigned long long)(m_total > M_CAP ? m_total - M_CAP : 0u));
        return;
    }
    for (k = 0; k < 6u && count; k++) {
        const m_event_t *m;
        i = below(count);
        m = &m_ring[(m_total - count + i) % M_CAP];
        if (vibeos_klog_get(i, &ev) != 0 || ev.seq != m->seq || ev.level != m->level ||
            ev.code != m->code || ev.arg0 != m->a0 || ev.arg1 != m->a1 ||
            strcmp(ev.message, m->msg) != 0) {
            fail("ring event %u: seq %llu '%s', model seq %llu '%s'", i,
                 (unsigned long long)ev.seq, ev.message, (unsigned long long)m->seq,
                 m->msg);
        }
    }
    /* A dump of random depth, line for line. */
    {
        uint32_t want = below(12);
        uint32_t start = (want == 0u || count <= want) ? 0u : count - want;
        static char expect[sizeof(g_dump)];
        size_t n;

        g_dump_len = 0;
        g_dump[0] = 0;
        vibeos_klog_dump_recent(want, t_dump, 0);
        if (want == 0u) {
            return;   /* the whole ring: too long to build here, checked by depth */
        }
        n = (size_t)snprintf(expect, sizeof(expect),
                             "[LOG] kernel ring: showing 0x%016llx of 0x%016llx\n",
                             (unsigned long long)(count - start),
                             (unsigned long long)count);
        for (i = start; i < count; i++) {
            char body[512];
            m_body(body, sizeof(body), &m_ring[(m_total - count + i) % M_CAP]);
            n += (size_t)snprintf(expect + n, sizeof(expect) - n, "[LOG]%s\n", body);
        }
        if (strcmp(expect, g_dump) != 0) {
            fail("dump_recent(%u)\n  want:\n%s  got:\n%s", want, expect, g_dump);
        }
    }
}

/* ---- crash records -------------------------------------------------------------- */

static vibeos_crash_t c_ring[VIBEOS_CRASH_RECORDS];
static uint64_t c_total;

static void crash_round(void) {
    static const char *const names[] = { "rip", "rsp", "rbp", "rflags", "rax", "rbx",
                                         "rcx", "rdx", "rsi", "rdi", "r8", "r9",
                                         "r10", "r11", "r12", "r13" };
    vibeos_crash_t r, got;
    uint32_t i, back;

    memset(&r, 0, sizeof(r));
    r.pid = (uint32_t)rnd();
    r.sig = below(64);
    r.vector = below(32);
    r.error_code = rnd();
    r.fault_addr = rnd();
    r.nregs = below(VIBEOS_CRASH_REGS + 1u);
    for (i = 0; i < r.nregs; i++) {
        r.regs[i].name = names[i];
        r.regs[i].value = rnd();
    }
    r.stack_words = below(VIBEOS_CRASH_STACK_WORDS + 1u);
    for (i = 0; i < r.stack_words; i++) {
        r.stack[i] = rnd();
    }
    random_text(r.exe, below(VIBEOS_CRASH_EXE));

    /* Sometimes the capture lies about its own sizes, or hands over a name with
     * no terminator: the record must be kept clamped, or the dump walks off the
     * end of its arrays. What should be kept is worked out here, separately. */
    {
        vibeos_crash_t k;
        uint32_t lie = below(8);

        if (lie == 0) {
            r.nregs = VIBEOS_CRASH_REGS + 1u + below(1000);
        } else if (lie == 1) {
            r.stack_words = VIBEOS_CRASH_STACK_WORDS + 1u + below(1000);
        } else if (lie == 2) {
            memset(r.exe, 'Z', sizeof(r.exe));
        }
        k = r;
        if (k.nregs > VIBEOS_CRASH_REGS) {
            k.nregs = VIBEOS_CRASH_REGS;
            for (i = 0; i < VIBEOS_CRASH_REGS; i++) {
                k.regs[i].name = names[i];   /* what an honest capture would have */
            }
        }
        if (k.stack_words > VIBEOS_CRASH_STACK_WORDS) {
            k.stack_words = VIBEOS_CRASH_STACK_WORDS;
        }
        k.exe[VIBEOS_CRASH_EXE - 1u] = 0;
        /* A lying capture still filled the arrays it claimed: give the kept
         * record the contents the clamp exposes. */
        for (i = 0; i < VIBEOS_CRASH_REGS; i++) {
            r.regs[i] = k.regs[i];
        }

        c_ring[c_total % VIBEOS_CRASH_RECORDS] = k;
        c_total++;
        if (vibeos_crash_record(&r) != c_total || vibeos_crash_count() != c_total) {
            fail("crash count %llu, model %llu",
                 (unsigned long long)vibeos_crash_count(), (unsigned long long)c_total);
        }
        r = k;   /* everything below checks what should have been kept */
    }
    for (back = 0; back <= VIBEOS_CRASH_RECORDS; back++) {
        int have = back < VIBEOS_CRASH_RECORDS && (uint64_t)back < c_total;
        int rc = vibeos_crash_get(back, &got);
        if (have != (rc == 0)) {
            fail("crash_get(%u) returned %d with %llu recorded", back, rc,
                 (unsigned long long)c_total);
        } else if (have &&
                   memcmp(&got, &c_ring[(c_total - 1u - back) % VIBEOS_CRASH_RECORDS],
                          sizeof(got)) != 0) {
            fail("crash_get(%u) is not the record kept %u back", back, back);
        }
    }

    /* The dump of the latest, line for line - unless the record is already known
     * to be wrong, when dumping it can only turn a named failure into a crash. */
    if (below(4) == 0 && g_failed == 0) {
        static char expect[4096];
        size_t n;

        n = (size_t)snprintf(expect, sizeof(expect),
            "[CRASH] total=0x%016llx pid=0x%016llx sig=0x%016llx exe=%s\n"
            "[CRASH] vector=0x%016llx err=0x%016llx fault_addr=0x%016llx\n",
            (unsigned long long)c_total, (unsigned long long)r.pid,
            (unsigned long long)r.sig, r.exe[0] ? r.exe : "(unknown)",
            (unsigned long long)r.vector, (unsigned long long)r.error_code,
            (unsigned long long)r.fault_addr);
        for (i = 0; i < r.nregs; i++) {
            n += (size_t)snprintf(expect + n, sizeof(expect) - n, "%s %s=0x%016llx%s",
                                  i % 4u == 0u ? "[CRASH]" : "", r.regs[i].name,
                                  (unsigned long long)r.regs[i].value,
                                  (i % 4u == 3u || i + 1u == r.nregs) ? "\n" : "");
        }
        for (i = 0; i < r.stack_words; i++) {
            n += (size_t)snprintf(expect + n, sizeof(expect) - n,
                                  "[CRASH] stack+0x%016llx = 0x%016llx\n",
                                  (unsigned long long)i * 8ull,
                                  (unsigned long long)r.stack[i]);
        }
        if (r.stack_words < VIBEOS_CRASH_STACK_WORDS) {
            n += (size_t)snprintf(expect + n, sizeof(expect) - n,
                                  "[CRASH] stack truncated: the next word is not readable\n");
        }
        snprintf(expect + n, sizeof(expect) - n, "[CRASH] end\n");
        g_dump_len = 0;
        g_dump[0] = 0;
        vibeos_crash_dump(t_dump, 0);
        if (strcmp(expect, g_dump) != 0) {
            fail("crash dump\n  want:\n%s  got:\n%s", expect, g_dump);
        }
    }
}

/* ---- setup ------------------------------------------------------------------------ */

static void configure(void) {
    uint32_t j;

    memset(m_sinks, 0, sizeof(m_sinks));
    memset(m_busy, 0, sizeof(m_busy));
    m_total = 0;
    m_depth = 0;
    m_lost_total = 0;
    vibeos_klog_set_lock(t_lock, t_unlock);
    vibeos_klog_set_cpu_id(0);
    vibeos_klog_reset();

    m_nsinks = 1u + below(VIBEOS_KLOG_MAX_SINKS);
    for (j = 0; j < m_nsinks; j++) {
        m_sink_t *s = &m_sinks[j];
        vibeos_klog_sink_t k;
        uint32_t p = below(4);

        s->min_level = (int)below(5);
        s->has_prefix = p != 0;
        if (p == 1) {
            strcpy(s->prefix, "[LOG]");
        } else if (p == 2) {
            random_text(s->prefix, below(12));
        } else if (p == 3) {
            random_text(s->prefix, 150u + below(80));   /* forces the cut */
        }
        s->numbered = (int)below(2);
        s->newline = (int)below(2);
        s->fail_one_in = below(3) == 0 ? 2u + below(10) : 0u;
        s->reenter_one_in = below(3) == 0 ? 2u + below(8) : 0u;

        memset(&k, 0, sizeof(k));
        k.name = "t";
        k.min_level = (vibeos_log_level_t)s->min_level;
        k.prefix = s->has_prefix ? s->prefix : 0;
        k.numbered = s->numbered;
        k.newline = s->newline;
        k.write = t_write;
        k.ctx = (void *)(uintptr_t)j;
        if (vibeos_klog_add_sink(&k) != (int)j) {
            fail("sink %u did not register at its index", j);
        }
    }
}

int main(int argc, char **argv) {
    uint64_t seed = argc > 1 ? strtoull(argv[1], 0, 10) : 1u;
    uint64_t rounds = argc > 2 ? strtoull(argv[2], 0, 10) : 4000u;
    uint64_t mbz_base;

    printf("diag torture seed=%llu rounds=%llu\n", (unsigned long long)seed,
           (unsigned long long)rounds);
    g_rng = seed * 0x9E3779B97F4A7C15ull + 1u;

    vibeos_crash_set_lock(t_lock, t_unlock);
    vibeos_crash_reset();
    configure();
    mbz_base = vibeos_mbz_count(VIBEOS_MBZ_KLOG_LINE_LOST);

    for (g_round = 0; g_round < rounds && g_failed == 0; g_round++) {
        uint32_t op = below(100);

        if (op < 80) {
            log_one(0);
        } else if (op < 88) {
            check_ring();
        } else if (op < 94) {
            crash_round();
        } else if (op < 96) {
            uint32_t j;
            random_text(m_panic_reason, below(120));
            memset(m_panic_written, 0, sizeof(m_panic_written));
            m_in_panic = 1;
            vibeos_klog_panic(m_panic_reason);
            m_in_panic = 0;
            for (j = 0; j < m_nsinks; j++) {
                if (m_panic_written[j] != 1) {
                    fail("sink %u given the panic line %d times", j, m_panic_written[j]);
                }
            }
        } else if (op < 97 && below(50) == 0) {
            /* A new machine: fresh sinks and an empty ring, the counters kept.
             *
             * Rare on purpose - about once in five thousand rounds. The first
             * version did it once in a hundred, so the 2048-event ring never
             * filled and a run of two hundred thousand rounds wrapped it zero
             * times: a torture of a ring that never wraps tortures nothing. */
            check_counters(mbz_base);
            if (m_total > m_most) {
                m_most = m_total;
            }
            configure();
            mbz_base = vibeos_mbz_count(VIBEOS_MBZ_KLOG_LINE_LOST);
        }
        check_counters(mbz_base);
        if (g_held != 0) {
            fail("the ring's lock is still held between calls");
        }
    }
    check_ring();
    if (m_total > m_most) {
        m_most = m_total;
    }
    /* Said, not assumed: a long run that never wrapped the ring checked nothing
     * about wrapping, and the summary line below would not show it. */
    if (rounds >= 50000u && m_most <= (uint64_t)M_CAP) {
        fail("a long run never wrapped the ring (at most %llu events on one machine)",
             (unsigned long long)m_most);
    }

    if (g_failed) {
        printf("diag torture FAILED seed=%llu (%d failures)\n", (unsigned long long)seed,
               g_failed);
        return 1;
    }
    printf("diag torture ok seed=%llu most_events_one_machine=%llu ring_wraps=%llu "
           "crashes=%llu\n", (unsigned long long)seed, (unsigned long long)m_most,
           (unsigned long long)(m_most / M_CAP), (unsigned long long)c_total);
    return 0;
}
