/* Crash records. See include/vibeos/crash.h. */

#include "vibeos/crash.h"

static void (*g_lock)(void);
static void (*g_unlock)(void);

static vibeos_crash_t g_ring[VIBEOS_CRASH_RECORDS];
static uint32_t g_next;
static uint64_t g_count;

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

void vibeos_crash_set_lock(void (*l)(void), void (*u)(void)) {
    g_lock = l;
    g_unlock = u;
}

void vibeos_crash_reset(void) {
    lock();
    g_next = 0;
    g_count = 0;
    unlock();
}

uint64_t vibeos_crash_record(const vibeos_crash_t *rec) {
    uint64_t total;

    if (!rec) {
        return vibeos_crash_count();
    }
    /* Claim, fill and advance as one step. Done as three, two cores faulting at
     * once took the same slot and the cursor skipped one nobody wrote. */
    lock();
    g_ring[g_next] = *rec;
    g_ring[g_next].exe[VIBEOS_CRASH_EXE - 1u] = 0;
    if (g_ring[g_next].nregs > VIBEOS_CRASH_REGS) {
        g_ring[g_next].nregs = VIBEOS_CRASH_REGS;
    }
    if (g_ring[g_next].stack_words > VIBEOS_CRASH_STACK_WORDS) {
        g_ring[g_next].stack_words = VIBEOS_CRASH_STACK_WORDS;
    }
    g_next = (g_next + 1u) % VIBEOS_CRASH_RECORDS;
    total = ++g_count;
    unlock();
    return total;
}

uint64_t vibeos_crash_count(void) {
    uint64_t n;

    lock();
    n = g_count;
    unlock();
    return n;
}

int vibeos_crash_get(uint32_t back, vibeos_crash_t *out) {
    int rc = -1;

    if (!out) {
        return -1;
    }
    lock();
    if (back < VIBEOS_CRASH_RECORDS && (uint64_t)back < g_count) {
        *out = g_ring[(g_next + VIBEOS_CRASH_RECORDS - 1u - back) %
                      VIBEOS_CRASH_RECORDS];
        rc = 0;
    }
    unlock();
    return rc;
}

/* ---- the dump -------------------------------------------------------------- */

typedef struct {
    char s[160];
    uint32_t n;
    uint32_t emitted;   /* lines handed to the device so far */
} line_t;

static void put_c(line_t *l, char c) {
    if (l->n + 1u < sizeof(l->s)) {
        l->s[l->n++] = c;
    }
}

static void put_s(line_t *l, const char *s) {
    while (s && *s) {
        put_c(l, *s++);
    }
}

static void put_x(line_t *l, uint64_t v) {
    static const char hex[] = "0123456789abcdef";
    int i;

    put_s(l, "0x");
    for (i = 15; i >= 0; i--) {
        put_c(l, hex[(v >> (i * 4)) & 0xFu]);
    }
}

static void emit(line_t *l, vibeos_crash_write_fn out, void *ctx) {
    put_c(l, '\n');
    l->s[l->n] = 0;
    (void)out(ctx, l->s, l->n);
    l->n = 0;
    l->emitted++;
}

void vibeos_crash_dump(vibeos_crash_write_fn out, void *ctx) {
    vibeos_crash_t rec;
    uint64_t total;
    line_t l;
    uint32_t i;

    if (!out) {
        return;
    }
    l.n = 0;
    l.emitted = 0;
    /* The count and the record in one critical section, so the total printed is
     * the one this record belongs to. */
    lock();
    total = g_count;
    if (total != 0u) {
        rec = g_ring[(g_next + VIBEOS_CRASH_RECORDS - 1u) % VIBEOS_CRASH_RECORDS];
    }
    unlock();
    if (total == 0u) {
        put_s(&l, "[CRASH] no process has faulted since boot");
        emit(&l, out, ctx);
        return;
    }

    put_s(&l, "[CRASH] total=");
    put_x(&l, total);
    put_s(&l, " pid=");
    put_x(&l, rec.pid);
    put_s(&l, " sig=");
    put_x(&l, rec.sig);
    put_s(&l, " exe=");
    put_s(&l, rec.exe[0] ? rec.exe : "(unknown)");
    emit(&l, out, ctx);

    put_s(&l, "[CRASH] vector=");
    put_x(&l, rec.vector);
    put_s(&l, " err=");
    put_x(&l, rec.error_code);
    put_s(&l, " fault_addr=");
    put_x(&l, rec.fault_addr);
    emit(&l, out, ctx);

    /* Four to a line, in the order the architecture listed them. */
    for (i = 0; i < rec.nregs; i++) {
        if (i % 4u == 0u) {
            put_s(&l, "[CRASH]");
        }
        put_c(&l, ' ');
        put_s(&l, rec.regs[i].name ? rec.regs[i].name : "?");
        put_c(&l, '=');
        put_x(&l, rec.regs[i].value);
        if (i % 4u == 3u || i + 1u == rec.nregs) {
            emit(&l, out, ctx);
        }
    }

    for (i = 0; i < rec.stack_words; i++) {
        put_s(&l, "[CRASH] stack+");
        put_x(&l, (uint64_t)i * 8ull);
        put_s(&l, " = ");
        put_x(&l, rec.stack[i]);
        emit(&l, out, ctx);
    }
    if (rec.stack_words < VIBEOS_CRASH_STACK_WORDS) {
        /* Said out loud: a short dump is a fact about the process's stack, not
         * a bug in the dumper. */
        put_s(&l, "[CRASH] stack truncated: the next word is not readable");
        emit(&l, out, ctx);
    }
    /* How many lines came before this one, so a line the device lost in the
     * middle of the dump is a count that does not match rather than a register
     * nobody noticed was missing. The gate counts them. */
    put_s(&l, "[CRASH] end lines=");
    put_x(&l, l.emitted);
    emit(&l, out, ctx);
}
