/* Host tests for the kernel log (C6, kernel/diag/klog.c). Each check names the
 * defect it stands for; several are defects this kernel has actually had. */

#include <stdio.h>
#include <string.h>

#include "vibeos/klog.h"
#include "vibeos/mbz.h"

int test_klog(void);

/* ---- a lock that says when it is misused -------------------------------------- */

static int g_locks, g_unlocks, g_held, g_bad_unlock, g_nested;
/* Runs once, from inside an unlock, standing in for a second core that logs in
 * the instant after this one lets go of the ring. */
static void (*g_on_unlock)(void);

static void t_lock(void) {
    g_locks++;
    if (g_held != 0) {
        g_nested = 1;
    }
    g_held++;
}

static void t_unlock(void) {
    void (*hook)(void) = g_on_unlock;

    g_unlocks++;
    if (g_held <= 0) {
        /* An unlock nobody took. On the console lock this handed a line away
         * mid-word and the gate reported crashes that had not happened. */
        g_bad_unlock = 1;
        return;
    }
    g_held--;
    if (hook && g_held == 0) {
        g_on_unlock = 0;
        hook();
    }
}

/* ---- sinks that remember what they were given ------------------------------------ */

#define T_LINES 32
#define T_LINE 300

typedef struct {
    char line[T_LINES][T_LINE];
    uint32_t len[T_LINES];
    int held_at_write[T_LINES];
    int calls;
    int fail;         /* refuse every line */
    int reenter;      /* log from inside the write, once */
} t_sink_t;

static t_sink_t g_a, g_b;

static int t_write(void *ctx, const char *line, uint32_t len) {
    t_sink_t *s = (t_sink_t *)ctx;

    if (s->calls < T_LINES) {
        uint32_t n = len < T_LINE - 1u ? len : T_LINE - 1u;
        memcpy(s->line[s->calls], line, n);
        s->line[s->calls][n] = 0;
        s->len[s->calls] = len;
        s->held_at_write[s->calls] = g_held;
    }
    s->calls++;
    if (s->reenter) {
        s->reenter = 0;
        vibeos_klog(VIBEOS_LOG_WARN, 0, 0, 0, "from inside the sink");
    }
    return s->fail ? -1 : 0;
}

/* The dump's device: every line appended to one buffer. */
static char g_dump[4096];
static uint32_t g_dump_len;
static int g_dump_calls;

static int t_dump(void *ctx, const char *line, uint32_t len) {
    (void)ctx;
    if (g_dump_len + len < sizeof(g_dump)) {
        memcpy(g_dump + g_dump_len, line, len);
        g_dump_len += len;
        g_dump[g_dump_len] = 0;
    }
    g_dump_calls++;
    return 0;
}

static void t_second_core_logs(void) {
    vibeos_klog(VIBEOS_LOG_INFO, 0, 0, 0, "second");
}

static int expect(int cond, const char *what) {
    if (!cond) {
        printf("FAIL:klog %s\n", what);
    }
    return cond;
}

static void fresh(void) {
    memset(&g_a, 0, sizeof(g_a));
    memset(&g_b, 0, sizeof(g_b));
    /* The misuse flags are sticky across the whole test, and a lock still held
     * between two blocks is itself misuse: checking them only at the end would
     * see only the last block. */
    if (g_held != 0) {
        g_bad_unlock = 1;
    }
    g_locks = g_unlocks = g_held = 0;
    g_on_unlock = 0;
    vibeos_klog_set_lock(t_lock, t_unlock);
    vibeos_klog_set_cpu_id(0);
    vibeos_klog_reset();
}

static int add(const char *name, vibeos_log_level_t min, const char *prefix,
               int numbered, int newline, t_sink_t *ctx) {
    vibeos_klog_sink_t s;

    memset(&s, 0, sizeof(s));
    s.name = name;
    s.min_level = min;
    s.prefix = prefix;
    s.numbered = numbered;
    s.newline = newline;
    s.write = t_write;
    s.ctx = ctx;
    return vibeos_klog_add_sink(&s);
}

int test_klog(void) {
    vibeos_klog_sink_stats_t st;
    vibeos_log_event_t ev;
    uint32_t count = 0, dropped = 0;
    uint64_t lost_before;

    g_bad_unlock = g_nested = 0;
    fresh();
    if (!expect(vibeos_klog_ready(), "ready after reset")) { return -1; }

    /* ---- one event is one complete line, in one call, per sink ------------------- */
    fresh();
    if (!expect(add("serial", VIBEOS_LOG_INFO, "[LOG]", 1, 1, &g_a) == 0,
                "the first sink registers as 0")) { return -1; }
    vibeos_klog(VIBEOS_LOG_WARN, 0xa, 1, 2, "hello");
    if (!expect(g_a.calls == 1,
                "an event reaches a sink as exactly one write: a line built from "
                "several is several critical sections, and one of them gets cut")) { return -1; }
    if (!expect(strcmp(g_a.line[0],
                       "[LOG][WARN] hello code=0x000000000000000a a0=0x0000000000000001 "
                       "a1=0x0000000000000002 ln=0x0000000000000001\n") == 0,
                "the line is prefix, level, message, fields, number and newline")) {
        printf("  got: %s", g_a.line[0]);
        return -1;
    }
    if (!expect(g_a.len[0] == (uint32_t)strlen(g_a.line[0]), "the length is the line's")) { return -1; }

    /* ---- the ring is touched under the lock and a sink never is ------------------- */
    {
        /* Counted around one call: the registration above takes the lock too, so
         * "the lock was taken at some point" would pass with the ring unlocked. */
        int before = g_locks;
        vibeos_klog(VIBEOS_LOG_DEBUG, 0, 0, 0, "counted");
        if (!expect(g_locks == before + 1,
                    "recording an event takes the ring's lock once: without it two cores "
                    "claimed one slot and printed each other's lines")) { return -1; }
    }
    if (!expect(g_locks == g_unlocks && g_held == 0,
                "recording took and released the module's own lock")) { return -1; }
    if (!expect(g_a.held_at_write[0] == 0,
                "a sink is written outside the ring's lock: the serial line is slow, "
                "and a slow device under the lock serialises every logging core")) { return -1; }

    /* ---- the event written is the one recorded, not "the latest" ------------------ */
    fresh();
    (void)add("serial", VIBEOS_LOG_INFO, 0, 0, 0, &g_a);
    g_on_unlock = t_second_core_logs;
    vibeos_klog(VIBEOS_LOG_INFO, 0, 0, 0, "first");
    if (!expect(g_a.calls == 2, "both events were written")) { return -1; }
    if (!expect(strcmp(g_a.line[0], "[INFO] second") == 0 &&
                strcmp(g_a.line[1], "[INFO] first") == 0,
                "each core writes its own event: asking the ring for the newest after "
                "letting go of it printed the other core's line twice and its own never")) {
        printf("  got: '%s' then '%s'\n", g_a.line[0], g_a.line[1]);
        return -1;
    }

    /* ---- levels: recorded always, written where admitted --------------------------- */
    fresh();
    (void)add("serial", VIBEOS_LOG_INFO, 0, 1, 0, &g_a);
    (void)add("loud", VIBEOS_LOG_WARN, 0, 1, 0, &g_b);
    vibeos_klog(VIBEOS_LOG_DEBUG, 0, 0, 0, "quiet");
    vibeos_klog(VIBEOS_LOG_INFO, 0, 0, 0, "normal");
    vibeos_klog(VIBEOS_LOG_ERROR, 0, 0, 0, "loud");
    if (!expect(vibeos_klog_count(&count, &dropped) == 0 && count == 3u && dropped == 0u,
                "every event is recorded, whatever the sinks admit")) { return -1; }
    if (!expect(g_a.calls == 2 && g_b.calls == 1,
                "a sink is written only the levels it asked for")) { return -1; }
    /* One sequence per sink, so a gap in one is not hidden by the other's lines. */
    if (!expect(strstr(g_a.line[0], " ln=0x0000000000000001") &&
                strstr(g_a.line[1], " ln=0x0000000000000002") &&
                strstr(g_b.line[0], " ln=0x0000000000000001"),
                "each sink numbers its own lines from one, without gaps")) { return -1; }
    if (!expect(vibeos_klog_get(0, &ev) == 0 && strcmp(ev.message, "quiet") == 0,
                "the quiet event is in the ring, oldest first")) { return -1; }

    /* ---- a refused line is counted, never dropped in silence ----------------------- */
    fresh();
    (void)add("serial", VIBEOS_LOG_INFO, 0, 1, 0, &g_a);
    (void)add("broken", VIBEOS_LOG_INFO, 0, 1, 0, &g_b);
    g_b.fail = 1;
    lost_before = vibeos_mbz_count(VIBEOS_MBZ_KLOG_LINE_LOST);
    vibeos_klog(VIBEOS_LOG_INFO, 0, 0, 0, "one");
    vibeos_klog(VIBEOS_LOG_INFO, 0, 0, 0, "two");
    if (!expect(vibeos_klog_sink_stats(1, &st) == 0 && st.offered == 2u && st.lost == 2u,
                "a sink that refuses lines has them counted as lost")) { return -1; }
    if (!expect(vibeos_mbz_count(VIBEOS_MBZ_KLOG_LINE_LOST) == lost_before + 2u &&
                vibeos_mbz_witness(VIBEOS_MBZ_KLOG_LINE_LOST) == 1u,
                "a lost line is a must-be-zero, and the witness names the sink")) { return -1; }
    if (!expect(vibeos_klog_sink_stats(0, &st) == 0 && st.lost == 0u && g_a.calls == 2,
                "one sink failing costs the others nothing")) { return -1; }

    /* ---- a sink that logs from its own write does not recurse ---------------------- */
    fresh();
    (void)add("disk", VIBEOS_LOG_DEBUG, 0, 0, 0, &g_a);
    (void)add("serial", VIBEOS_LOG_INFO, 0, 0, 0, &g_b);
    g_a.reenter = 1;
    vibeos_klog(VIBEOS_LOG_INFO, 0, 0, 0, "outer");
    if (!expect(g_a.calls == 1,
                "the event raised inside a sink's write is not offered back to it: "
                "the block layer logs a refusal, which would write, which would log")) { return -1; }
    if (!expect(vibeos_klog_sink_stats(0, &st) == 0 && st.reentered == 1u,
                "and that is counted, not silent")) { return -1; }
    if (!expect(g_b.calls == 2 && vibeos_klog_count(&count, 0) == 0 && count == 2u,
                "the inner event is still recorded and reaches the other sinks")) { return -1; }
    /* The guard is cleared afterwards: the next event reaches the disk again. */
    vibeos_klog(VIBEOS_LOG_INFO, 0, 0, 0, "after");
    if (!expect(g_a.calls == 2, "the reentrancy guard is released after the write")) { return -1; }

    /* ---- a line that does not fit is cut, and still ends its line ------------------ */
    fresh();
    {
        static char big[VIBEOS_KLOG_LINE + 40];
        memset(big, 'P', sizeof(big) - 1u);
        big[sizeof(big) - 1u] = 0;
        (void)add("long", VIBEOS_LOG_INFO, big, 1, 1, &g_a);
        vibeos_klog(VIBEOS_LOG_INFO, 0, 0, 0, "x");
        if (!expect(g_a.calls == 1 && g_a.len[0] < VIBEOS_KLOG_LINE &&
                    g_a.line[0][g_a.len[0] - 1u] == '\n',
                    "a truncated line keeps its newline: two lines run together is "
                    "what the interleaving check calls a defect")) { return -1; }
    }

    /* ---- the dumps ------------------------------------------------------------------ */
    fresh();
    vibeos_klog(VIBEOS_LOG_INFO, 0, 0, 0, "a");
    vibeos_klog(VIBEOS_LOG_WARN, 0, 0, 0, "b");
    vibeos_klog(VIBEOS_LOG_ERROR, 0, 0, 0, "c");
    g_dump_len = 0; g_dump_calls = 0; g_dump[0] = 0;
    vibeos_klog_dump_recent(2, t_dump, 0);
    if (!expect(strcmp(g_dump,
                       "[LOG] kernel ring: showing 0x0000000000000002 of 0x0000000000000003\n"
                       "[LOG][WARN] b\n[LOG][ERROR] c\n") == 0 && g_dump_calls == 3,
                "dump_recent: a header, then the newest events oldest first, a line a call")) {
        printf("  got:\n%s", g_dump);
        return -1;
    }
    g_dump_len = 0; g_dump_calls = 0; g_dump[0] = 0;
    {
        int held = g_locks;
        vibeos_klog_dump_unlocked(t_dump, 0);
        if (!expect(g_locks == held,
                    "the panic dump takes no lock: the core holding it may be the one "
                    "that faulted")) { return -1; }
    }
    if (!expect(strstr(g_dump, "[LOG] dump count=0x0000000000000003 dropped=0x0000000000000000\n") &&
                strstr(g_dump, "[LOG] #0000000000000001 [LOG][INFO] a\n") &&
                strstr(g_dump, "[LOG] #0000000000000003 [LOG][ERROR] c\n") &&
                g_dump_calls == 4,
                "dump_unlocked: every event with its sequence number")) {
        printf("  got:\n%s", g_dump);
        return -1;
    }

    /* ---- a panic reaches every sink, quiet ones too, with no lock ------------------- */
    fresh();
    (void)add("serial", VIBEOS_LOG_ERROR, "[LOG]", 1, 1, &g_a);
    (void)add("disk", VIBEOS_LOG_DEBUG, 0, 0, 0, &g_b);
    {
        int held = g_locks;
        vibeos_klog_panic("it broke");
        if (!expect(g_locks == held, "vibeos_klog_panic takes no lock")) { return -1; }
    }
    if (!expect(g_a.calls == 1 && g_b.calls == 1 &&
                strcmp(g_a.line[0], "[LOG][FATAL] PANIC: it broke\n") == 0 &&
                strcmp(g_b.line[0], "[FATAL] PANIC: it broke") == 0,
                "the panic line reaches every sink, unnumbered")) {
        printf("  got: '%s' / '%s'\n", g_a.line[0], g_b.line[0]);
        return -1;
    }
    if (!expect(vibeos_klog_count(&count, 0) == 0 && count == 0u,
                "the panic line is not recorded: the ring is about to be dumped")) { return -1; }

    /* ---- registration ---------------------------------------------------------------- */
    fresh();
    {
        vibeos_klog_sink_t s;
        int i;
        memset(&s, 0, sizeof(s));
        if (!expect(vibeos_klog_add_sink(&s) == -1, "a sink with no write is refused")) { return -1; }
        for (i = 0; i < (int)VIBEOS_KLOG_MAX_SINKS; i++) {
            (void)add("n", VIBEOS_LOG_INFO, 0, 0, 0, &g_a);
        }
        if (!expect(add("n", VIBEOS_LOG_INFO, 0, 0, 0, &g_a) == -1 &&
                    vibeos_klog_sink_count() == VIBEOS_KLOG_MAX_SINKS,
                    "a full table refuses, rather than writing past it")) { return -1; }
    }

    /* ---- every call balanced its lock ------------------------------------------------ */
    if (!expect(!g_bad_unlock,
                "no unlock without a lock: on the console, an unmatched unlock freed "
                "another core's line mid-word and invented crashes")) { return -1; }
    if (!expect(!g_nested && g_held == 0, "the ring's lock is never taken twice")) { return -1; }

    vibeos_klog_set_lock(0, 0);
    vibeos_klog_reset();
    return 0;
}
