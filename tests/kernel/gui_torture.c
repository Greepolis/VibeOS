/* Randomised torture for the graphical shell (kernel/io/gui.c), against a
 * reference model, and then under real threads.
 *
 * The host test checks the cases somebody thought of. This runs long random
 * sequences - geometries at and around the edges of what init accepts, buffers
 * one byte short, characters of every kind, a pointer anywhere, a "panic print"
 * from inside the GUI's own lock - and checks the terminal against a model that
 * shares no code with it.
 *
 * Independence is the point. The GUI keeps a grid with a row and a column; the
 * model keeps a list of lines and derives what a grid would show. A terminal
 * asked whether it scrolled correctly answers from the counters it used to
 * scroll, and the defect worth finding is the self-consistent one - a
 * character lost at a wrap that the grid never notices it lost.
 *
 * Every buffer the GUI is given is fenced on both sides, so a write that leaves
 * it is seen as it happens rather than as a crash somewhere else later - the
 * failure this subsystem was the prime suspect for.
 *
 * It prints its seed on the first line, so a failure can be replayed exactly:
 *
 *     vibeos_gui_torture <seed> [rounds] [threads]
 *
 * ## What is checked
 *
 * - **Init**: accepted exactly when the model says the geometry and the buffer
 *   allow it; refused with a reason, and with nothing written, otherwise.
 * - **The terminal**: every row, chars, scrolls and term_chars against the
 *   model, after random bursts of characters.
 * - **Memory**: both buffers' fences intact after every operation, and the
 *   canary too - including on the smallest screens, where the old clamps
 *   wrapped.
 * - **The composition**: after a repaint with the pointer on screen, the screen
 *   equals the back buffer everywhere except under the pointer - the pointer
 *   is drawn, and never baked into what the next frame restores from.
 * - **The canary detector**: a word flipped on purpose is found at the next
 *   repaint and counted once.
 * - **The lock**: taken and released in pairs, never held on return, and a
 *   print from inside it is dropped and counted rather than waited on.
 * - **Threads** (not on Windows): writers on several threads and a repaint on
 *   another, with a real mutex; afterwards every character is accounted for,
 *   no grid write was refused as out of bounds, and no row is longer than a
 *   row. Under ThreadSanitizer (the nightly) a race is reported whether or not
 *   it happened to corrupt anything this run.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <pthread.h>
#endif

#include "vibeos/device.h"
#include "vibeos/gui.h"

#define COLS VIBEOS_GUI_TERM_COLS
#define ROWS VIBEOS_GUI_TERM_ROWS
#define CUR_W 9u    /* the pointer's rectangle: 8 pixels and its outline */
#define CUR_H 12u

static uint64_t g_rng;
static uint64_t g_seed;
static int g_failed;

static uint64_t rnd(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return g_rng;
}

static uint32_t below(uint32_t n) {
    return n ? (uint32_t)(rnd() % n) : 0u;
}

static void fail(const char *what, uint32_t round) {
    printf("FAIL:gui_torture seed=%llu round=%u %s\n",
           (unsigned long long)g_seed, round, what);
    fflush(stdout);
    g_failed = 1;
}

/* ---- the model: a list of lines --------------------------------------------- */

typedef struct {
    char text[COLS];
    uint32_t len;
} mline_t;

static mline_t m_lines[ROWS];
static uint32_t m_count;           /* lines that exist, the current one included */
static uint64_t m_chars, m_scrolls, m_reentered;

static void model_reset(void) {
    memset(m_lines, 0, sizeof(m_lines));
    m_count = 1;
    m_chars = m_scrolls = m_reentered = 0;
}

static mline_t *model_cur(void) {
    return &m_lines[m_count - 1u];
}

static void model_newline(void) {
    if (m_count == ROWS) {
        memmove(&m_lines[0], &m_lines[1], sizeof(mline_t) * (ROWS - 1u));
        memset(&m_lines[ROWS - 1u], 0, sizeof(mline_t));
        m_scrolls++;
    } else {
        m_count++;
    }
}

static void model_putc(char c) {
    mline_t *l;
    if (c == '\r') {
        return;
    }
    if (c == '\n') {
        model_newline();
        return;
    }
    if (c == '\b') {
        l = model_cur();
        if (l->len > 0u) {
            l->len--;
            l->text[l->len] = 0;
        }
        return;
    }
    /* A row holds one fewer than the grid is wide: the last column is where
     * the terminal wraps, never where it writes. */
    if (model_cur()->len >= COLS - 1u) {
        model_newline();
    }
    l = model_cur();
    l->text[l->len++] = c;
    m_chars++;
}

/* What row r shows: its text up to the first NUL - a NUL written into the
 * terminal ends what the row prints, like anything that walks a C string. */
static void model_row(uint32_t r, char *out) {
    uint32_t i = 0;
    if (r < m_count) {
        for (; i < m_lines[r].len && m_lines[r].text[i]; i++) {
            out[i] = m_lines[r].text[i];
        }
    }
    out[i] = 0;
}

static uint64_t model_term_chars(void) {
    uint64_t n = 0;
    uint32_t r, i;
    for (r = 0; r < m_count; r++) {
        for (i = 0; i < m_lines[r].len; i++) {
            if (m_lines[r].text[i]) {
                n++;
            }
        }
    }
    return n;
}

/* ---- the lock operations (single-threaded phase) ------------------------------ */

static int t_depth, t_bad;
static uint32_t t_reenter_pct;   /* chance, per lock, of a print from inside it */
static int t_in_reentry;

static int t_lock(vibeos_dev_lock_t *l) {
    if (!l->name || strcmp(l->name, "gui") != 0) {
        t_bad = 1;
    }
    if (t_depth != 0) {
        return -1;        /* the caller holds it: the provider's answer */
    }
    t_depth = 1;
    if (!t_in_reentry && below(100u) < t_reenter_pct) {
        t_in_reentry = 1;
        vibeos_gui_putc((char)(0x21 + below(90u)));
        m_reentered++;    /* dropped, and counted */
        t_in_reentry = 0;
    }
    return 0;
}

static void t_unlock(vibeos_dev_lock_t *l) {
    (void)l;
    if (t_depth != 1) {
        t_bad = 1;
    }
    t_depth = 0;
}

/* ---- the pointer, through the registry ---------------------------------------- */

/* Atomics, not volatile: two repaint threads read them while the main thread
 * or a ticker writes, and a race in the test's own plumbing would be reported
 * by ThreadSanitizer as loudly as one in the GUI. */
static int32_t p_x, p_y;

static void set_pointer(int32_t x, int32_t y) {
    __atomic_store_n(&p_x, x, __ATOMIC_RELAXED);
    __atomic_store_n(&p_y, y, __ATOMIC_RELAXED);
}

static int fake_pointer(int32_t *x, int32_t *y, uint32_t *b) {
    if (x) { *x = __atomic_load_n(&p_x, __ATOMIC_RELAXED); }
    if (y) { *y = __atomic_load_n(&p_y, __ATOMIC_RELAXED); }
    if (b) { *b = 0; }
    return 0;
}

static const vibeos_input_ops_t g_ptr_ops = { 0, 0, fake_pointer };
static const vibeos_device_t g_ptr_dev = {
    "pointer", VIBEOS_DEV_INPUT, VIBEOS_DEVICE_NO_IRQ, 0, 0, 0, 0, &g_ptr_ops };

/* ---- fenced memory -------------------------------------------------------------- */

#define FENCE 256u
#define FENCE_WORD 0x5AA5C33Cu

typedef struct {
    uint32_t *raw;
    uint32_t *p;
    uint64_t words;
} fenced_t;

static int fenced_alloc(fenced_t *f, uint64_t words) {
    uint64_t i;
    f->words = words;
    f->raw = (uint32_t *)malloc((size_t)(words + 2u * FENCE) * 4u);
    if (!f->raw) {
        return -1;
    }
    for (i = 0; i < words + 2u * FENCE; i++) {
        f->raw[i] = FENCE_WORD;
    }
    f->p = f->raw + FENCE;
    return 0;
}

static int fenced_intact(const fenced_t *f) {
    uint64_t i;
    for (i = 0; i < FENCE; i++) {
        if (f->raw[i] != FENCE_WORD || f->raw[FENCE + f->words + i] != FENCE_WORD) {
            return 0;
        }
    }
    return 1;
}

static int fenced_untouched(const fenced_t *f) {
    uint64_t i;
    for (i = 0; i < f->words + 2u * FENCE; i++) {
        if (f->raw[i] != FENCE_WORD) {
            return 0;
        }
    }
    return 1;
}

/* ---- geometry ------------------------------------------------------------------- */

/* Mostly small, always near the edges of what init accepts: the old clamps
 * wrapped at a height of about 30 and a width under 10, so the interesting
 * screens are the ones nobody has. */
static uint32_t pick_dim(uint32_t min, uint32_t max, uint32_t cap_small) {
    switch (below(12u)) {
    case 0: return 0u;
    case 1: return 1u + below(40u);            /* the wrapping range, and below */
    case 2: return min - 1u;
    case 3: return min;
    case 4: return min + 1u;
    case 5: return below(50u) == 0 ? max : min + below(cap_small - min);
    case 6: return below(50u) == 0 ? max + 1u : min + below(cap_small - min);
    default: return min + below(cap_small - min);
    }
}

/* ---- one single-threaded round ----------------------------------------------------- */

static void check_terminal(uint32_t round) {
    vibeos_gui_stats_t st;
    char got[COLS + 1u], want[COLS + 1u];
    uint32_t r;

    for (r = 0; r < ROWS; r++) {
        if (vibeos_gui_term_row(r, got) < 0) {
            fail("term_row refused a row in range", round);
            return;
        }
        model_row(r, want);
        if (strcmp(got, want) != 0) {
            char msg[256];
            snprintf(msg, sizeof(msg), "row %u is \"%.40s\", the model says \"%.40s\"", r, got, want);
            fail(msg, round);
            return;
        }
    }
    vibeos_gui_stats(&st);
    if (st.chars != m_chars || st.scrolls != m_scrolls || st.term_chars != model_term_chars() ||
        st.reentered != m_reentered) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "counters chars=%llu/%llu scrolls=%llu/%llu term_chars=%llu/%llu reentered=%llu/%llu (gui/model)",
                 (unsigned long long)st.chars, (unsigned long long)m_chars,
                 (unsigned long long)st.scrolls, (unsigned long long)m_scrolls,
                 (unsigned long long)st.term_chars, (unsigned long long)model_term_chars(),
                 (unsigned long long)st.reentered, (unsigned long long)m_reentered);
        fail(msg, round);
    }
    if (st.term_overrun != 0u) {
        fail("a grid write fell outside the grid", round);
    }
}

static char random_char(void) {
    uint32_t k = below(100u);
    if (k < 70u) { return (char)(0x20 + below(95u)); }
    if (k < 82u) { return '\n'; }
    if (k < 88u) { return '\b'; }
    if (k < 92u) { return '\r'; }
    if (k < 97u) { return (char)(1u + below(31u)); }   /* control: stored, not drawn */
    if (k < 99u) { return (char)(0x80u + below(128u)); }
    return 0;                                          /* a NUL, rarely */
}

static void one_round(uint32_t round) {
    fenced_t fb, back;
    uint32_t w = pick_dim(VIBEOS_GUI_MIN_W, VIBEOS_GUI_MAX_W, 480u);
    uint32_t h = pick_dim(VIBEOS_GUI_MIN_H, VIBEOS_GUI_MAX_H, 360u);
    uint64_t need = (uint64_t)w * h * 4u + VIBEOS_GUI_GUARD_BYTES;
    uint64_t give;
    int no_fb = below(20u) == 0;
    int want_ok, got;
    uint32_t op, ops;
    uint32_t ticks_since_pointer = 0;
    int pointer_sane = 1;

    switch (below(8u)) {
    case 0: give = need > 0u ? need - 1u : 0u; break;
    case 1: give = need + 4u * below(1024u); break;
    default: give = need; break;
    }
    want_ok = !no_fb && w >= VIBEOS_GUI_MIN_W && h >= VIBEOS_GUI_MIN_H &&
              w <= VIBEOS_GUI_MAX_W && h <= VIBEOS_GUI_MAX_H && give >= need;

    /* The framebuffer is sized to the screen; for a refused one, whatever it is,
     * so a write into it is seen. */
    if (fenced_alloc(&fb, (uint64_t)(w ? w : 1u) * (h ? h : 1u)) != 0 ||
        fenced_alloc(&back, (give + 3u) / 4u + 1u) != 0) {
        fail("host out of memory", round);
        return;
    }

    vibeos_gui_reset();
    model_reset();
    t_depth = 0;
    t_reenter_pct = below(4u) == 0 ? 5u : 0u;

    got = vibeos_gui_init(no_fb ? 0u : (uint64_t)(uintptr_t)fb.p, w, h, back.p, give);
    if ((got == 0) != want_ok) {
        char msg[200];
        snprintf(msg, sizeof(msg), "init %ux%u buffer %llu of %llu: gui says %d, model says %s (%s)",
                 w, h, (unsigned long long)give, (unsigned long long)need, got,
                 want_ok ? "accept" : "refuse", vibeos_gui_why());
        fail(msg, round);
        goto out;
    }
    if (got != 0) {
        if (strcmp(vibeos_gui_why(), "ok") == 0 || vibeos_gui_active()) {
            fail("a refusal without a reason, or an active refusal", round);
        }
        if (!fenced_untouched(&fb) || !fenced_untouched(&back)) {
            fail("a refused init wrote into a buffer", round);
        }
        goto out;
    }
    if (!fenced_intact(&fb) || !fenced_intact(&back)) {
        fail("composing the desktop wrote outside a buffer", round);
        goto out;
    }
    /* On this screen. The pointer is a global and outlives the round: a
     * position left by a larger screen, kept by a tick that does not move it,
     * sent the first version of the checks below reading past this one. */
    set_pointer((int32_t)below(w), (int32_t)below(h));

    ops = 50u + below(400u);
    for (op = 0; op < ops && !g_failed; op++) {
        uint32_t k = below(100u);
        if (k < 70u) {
            uint32_t n = 1u + below(40u), i;
            for (i = 0; i < n; i++) {
                char c = random_char();
                vibeos_gui_putc(c);
                model_putc(c);
            }
        } else if (k < 95u) {
            vibeos_gui_stats_t before, after;
            int moved;
            vibeos_gui_stats(&before);
            if (below(10u) == 0) {
                /* Anywhere at all - negative, past the edge. Memory safety
                 * only: a pointer the mouse would never report leaves the old
                 * rectangle unrestored, which is drawing, not corruption. */
                set_pointer((int32_t)(rnd() & 0xFFFFFFFFu), (int32_t)(rnd() & 0xFFFFFFFFu));
                pointer_sane = 0;
            } else if (below(3u) != 0) {
                set_pointer((int32_t)below(w), (int32_t)below(h));
            }
            vibeos_gui_tick();
            ticks_since_pointer++;
            vibeos_gui_stats(&after);
            moved = after.frames != before.frames;
            (void)moved;
            if (after.guard_checks != before.guard_checks + 1u) {
                fail("a repaint did not examine the canary", round);
            }
            if (after.guard_broken != 0u) {
                fail("the canary broke with nobody touching it", round);
            }
            /* The screen is the composition, except under the pointer. */
            if (pointer_sane && (uint64_t)w * h <= 640u * 480u) {
                uint32_t x, y;
                uint32_t px = (uint32_t)__atomic_load_n(&p_x, __ATOMIC_RELAXED);
                uint32_t py = (uint32_t)__atomic_load_n(&p_y, __ATOMIC_RELAXED);
                /* And the pointer is there at all. "The screen is the
                 * composition except under the pointer" is also true of a
                 * pointer that was erased and never drawn again; the tip of
                 * the arrow is always the cursor's colour. */
                if (fb.p[(uint64_t)py * w + px] != 0x00FFFFFFu) {
                    char msg[120];
                    snprintf(msg, sizeof(msg), "the pointer at %u,%u is not on the screen", px, py);
                    fail(msg, round);
                }
                for (y = 0; y < h && !g_failed; y++) {
                    for (x = 0; x < w; x++) {
                        int under = x >= px && x < px + CUR_W && y >= py && y < py + CUR_H;
                        if (!under && fb.p[(uint64_t)y * w + x] != back.p[(uint64_t)y * w + x]) {
                            char msg[160];
                            snprintf(msg, sizeof(msg),
                                     "screen %ux%u differs from the composition at %u,%u, pointer at %u,%u",
                                     w, h, x, y, px, py);
                            fail(msg, round);
                            break;
                        }
                    }
                }
            }
        } else if (k < 97u) {
            /* The canary detector: one word flipped, found at the next
             * repaint, counted once, and put back. */
            uint32_t *guard = back.p + (uint64_t)w * h;
            uint32_t word = below(VIBEOS_GUI_GUARD_BYTES / 4u);
            vibeos_gui_stats_t st;
            guard[word] ^= 0x10000u;
            vibeos_gui_tick();
            vibeos_gui_stats(&st);
            guard[word] ^= 0x10000u;
            if (st.guard_broken != 1u) {
                fail("a flipped canary word was not counted exactly once", round);
            }
            /* The count is the worst seen, so the rest of the round would
             * report it: start the round's canary accounting again. */
            break;
        } else {
            check_terminal(round);
        }
        if (t_depth != 0 || t_bad) {
            fail("the lock was held on return, or released unheld", round);
        }
        if (!fenced_intact(&fb) || !fenced_intact(&back)) {
            char msg[120];
            snprintf(msg, sizeof(msg), "a write left a buffer, screen %ux%u", w, h);
            fail(msg, round);
            break;
        }
    }
    if (!g_failed) {
        check_terminal(round);
    }
out:
    free(fb.raw);
    free(back.raw);
}

/* ---- threads -------------------------------------------------------------------- */

#ifndef _WIN32

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_owned;           /* under g_mu */
static int g_nest_bad;        /* under g_mu */
/* "Does the caller hold it?" is a question about the caller, so it is kept per
 * thread - the machine answers it from the lock's owning CPU. */
static __thread int t_holding;

/* A real lock, with the provider's one extra duty: refuse, rather than wait,
 * when the caller already holds it. */
static int th_lock(vibeos_dev_lock_t *l) {
    (void)l;
    if (t_holding) {
        return -1;
    }
    pthread_mutex_lock(&g_mu);
    if (g_owned) {
        g_nest_bad = 1;
    }
    g_owned = 1;
    t_holding = 1;
    return 0;
}

static void th_unlock(vibeos_dev_lock_t *l) {
    (void)l;
    g_owned = 0;
    t_holding = 0;
    pthread_mutex_unlock(&g_mu);
}

#define WRITES 20000u

static int g_stop;

static void *writer(void *arg) {
    uint64_t r = (uint64_t)(uintptr_t)arg * 0x9E3779B97F4A7C15ull + 1u;
    uint32_t i;
    for (i = 0; i < WRITES; i++) {
        char c;
        r ^= r << 13; r ^= r >> 7; r ^= r << 17;
        c = (r % 9u == 0u) ? '\n' : (char)(0x21 + (r % 90u));
        vibeos_gui_putc(c);
    }
    return 0;
}

static void *ticker(void *arg) {
    uint64_t r = (uint64_t)(uintptr_t)arg + 7u;
    (void)arg;
    while (!__atomic_load_n(&g_stop, __ATOMIC_ACQUIRE)) {
        r ^= r << 13; r ^= r >> 7; r ^= r << 17;
        set_pointer((int32_t)(r % 600u), (int32_t)((r >> 20) % 440u));
        vibeos_gui_tick();
    }
    return 0;
}

static void threaded(uint32_t nthreads) {
    enum { TW = 640, TH = 480 };
    fenced_t fb, back;
    uint64_t need = (uint64_t)TW * TH * 4u + VIBEOS_GUI_GUARD_BYTES;
    pthread_t th[16], tk[2];
    uint64_t expect_chars = 0;
    vibeos_gui_stats_t st;
    uint32_t i, r;
    char row[COLS + 1u];

    if (nthreads > 16u) {
        nthreads = 16u;
    }
    /* What each writer sends, counted by replaying its generator. */
    for (i = 0; i < nthreads; i++) {
        uint64_t x = (uint64_t)(uintptr_t)(i + 1u) * 0x9E3779B97F4A7C15ull + 1u;
        uint32_t j;
        for (j = 0; j < WRITES; j++) {
            x ^= x << 13; x ^= x >> 7; x ^= x << 17;
            if (x % 9u != 0u) {
                expect_chars++;
            }
        }
    }
    fenced_alloc(&fb, (uint64_t)TW * TH);
    fenced_alloc(&back, need / 4u + 1u);
    vibeos_gui_reset();
    vibeos_device_set_lock_ops(th_lock, th_unlock);
    if (vibeos_gui_init((uint64_t)(uintptr_t)fb.p, TW, TH, back.p, need) != 0) {
        fail("threaded init refused", 0);
        return;
    }
    __atomic_store_n(&g_stop, 0, __ATOMIC_RELEASE);
    pthread_create(&tk[0], 0, ticker, (void *)(uintptr_t)1u);
    pthread_create(&tk[1], 0, ticker, (void *)(uintptr_t)2u);  /* two repaints race for g_ticking */
    for (i = 0; i < nthreads; i++) {
        pthread_create(&th[i], 0, writer, (void *)(uintptr_t)(i + 1u));
    }
    for (i = 0; i < nthreads; i++) {
        pthread_join(th[i], 0);
    }
    __atomic_store_n(&g_stop, 1, __ATOMIC_RELEASE);
    pthread_join(tk[0], 0);
    pthread_join(tk[1], 0);

    vibeos_gui_stats(&st);
    if (st.chars != expect_chars) {
        char msg[160];
        snprintf(msg, sizeof(msg), "threads: %llu characters sent, %llu counted",
                 (unsigned long long)expect_chars, (unsigned long long)st.chars);
        fail(msg, 0);
    }
    if (st.term_overrun != 0u) {
        fail("threads: a grid write fell outside the grid", 0);
    }
    if (st.guard_broken != 0u || !fenced_intact(&fb) || !fenced_intact(&back)) {
        fail("threads: a write left a buffer", 0);
    }
    if (st.reentered != 0u || g_nest_bad) {
        fail("threads: a writer was refused as the holder, or the lock nested", 0);
    }
    for (r = 0; r < ROWS; r++) {
        if (vibeos_gui_term_row(r, row) < 0 || strlen(row) > COLS - 1u) {
            fail("threads: a row longer than a row", 0);
        }
    }
    printf("gui_torture threads=%u chars=%llu scrolls=%llu frames=%llu overlapped=%llu\n",
           nthreads, (unsigned long long)st.chars, (unsigned long long)st.scrolls,
           (unsigned long long)st.frames, (unsigned long long)st.ticks_overlapped);
    vibeos_device_set_lock_ops(0, 0);
    free(fb.raw);
    free(back.raw);
}

#endif

int main(int argc, char **argv) {
    uint32_t rounds = argc > 2 ? (uint32_t)strtoul(argv[2], 0, 10) : 400u;
    uint32_t nthreads = argc > 3 ? (uint32_t)strtoul(argv[3], 0, 10) : 4u;
    uint32_t i;
    const vibeos_device_t *table[] = { &g_ptr_dev };

    g_seed = argc > 1 ? strtoull(argv[1], 0, 10) : 1u;
    g_rng = g_seed * 0x2545F4914F6CDD1Dull + 0x9E3779B97F4A7C15ull;
    printf("gui_torture seed=%llu rounds=%u threads=%u\n",
           (unsigned long long)g_seed, rounds, nthreads);

    vibeos_device_reset();
    vibeos_device_set_table(table, 1u);
    vibeos_device_set_lock_ops(t_lock, t_unlock);
    for (i = 0; i < rounds && !g_failed; i++) {
        one_round(i);
    }
    vibeos_device_set_lock_ops(0, 0);
#ifndef _WIN32
    if (!g_failed && nthreads > 0u) {
        threaded(nthreads);
    }
#endif
    vibeos_gui_reset();
    vibeos_device_reset();
    if (g_failed) {
        return 1;
    }
    printf("gui_torture ok\n");
    return 0;
}
