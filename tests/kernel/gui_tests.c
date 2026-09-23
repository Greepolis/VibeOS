/* Host tests for the graphical shell (C7, kernel/io/gui.c). Each check names the
 * property it stands for. The randomised model and the threads are in
 * gui_torture.c; these are the cases somebody thought of. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vibeos/device.h"
#include "vibeos/gui.h"

int test_gui(void);

static int expect(int cond, const char *what) {
    if (!cond) {
        printf("FAIL:gui %s\n", what);
    }
    return cond;
}

/* ---- the lock operations, as the machine would register them ------------------ */

static int g_depth, g_nested, g_locks, g_refused, g_bad_name;
static int g_reenter_on_lock;   /* putc from inside the lock, as a panic would */

/* One caller, so "the caller holds it already" is "it is held": the provider's
 * answer, as the machine's spinlocks give it from their owning CPU. */
static int t_lock(vibeos_dev_lock_t *l) {
    if (!l->name || strcmp(l->name, "gui") != 0) {
        g_bad_name = 1;
    }
    if (g_depth != 0) {
        g_refused++;
        return -1;
    }
    g_depth++;
    g_locks++;
    if (g_reenter_on_lock) {
        g_reenter_on_lock = 0;
        vibeos_gui_putc('Z');
    }
    return 0;
}

static void t_unlock(vibeos_dev_lock_t *l) {
    (void)l;
    if (g_depth != 1) {
        g_nested = 1;
    }
    g_depth--;
}

/* ---- memory with fences on both sides ------------------------------------------ */

#define FENCE 64u
#define FENCE_WORD 0xA5A5A5A5u

typedef struct {
    uint32_t *raw;
    uint32_t *p;     /* what the GUI is given */
    uint64_t words;
} fenced_t;

static void fenced_alloc(fenced_t *f, uint64_t words) {
    uint64_t i;
    f->words = words;
    f->raw = (uint32_t *)malloc((size_t)(words + 2u * FENCE) * 4u);
    for (i = 0; i < words + 2u * FENCE; i++) {
        f->raw[i] = FENCE_WORD;
    }
    f->p = f->raw + FENCE;
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

/* ---- a pointer, through the device registry --------------------------------------- */

static int32_t g_px = 10, g_py = 10;

static int fake_pointer(int32_t *x, int32_t *y, uint32_t *b) {
    if (x) { *x = g_px; }
    if (y) { *y = g_py; }
    if (b) { *b = 0; }
    return 0;
}

static const vibeos_input_ops_t g_mouse_ops = { 0, 0, fake_pointer };
static const vibeos_device_t g_mouse = {
    "mouse", VIBEOS_DEV_INPUT, VIBEOS_DEVICE_NO_IRQ, 0, 0, 0, 0, &g_mouse_ops };

/* ---- displays, for the class ---------------------------------------------------- */

static char g_disp_trace[64];

static void disp_a_putc(char c) { size_t n = strlen(g_disp_trace); if (n < 60) { g_disp_trace[n] = 'A'; g_disp_trace[n + 1] = c; g_disp_trace[n + 2] = 0; } }
static void disp_a_tick(void) { strcat(g_disp_trace, "Ta"); }
static void disp_b_putc(char c) { size_t n = strlen(g_disp_trace); if (n < 60) { g_disp_trace[n] = 'B'; g_disp_trace[n + 1] = c; g_disp_trace[n + 2] = 0; } }
static void disp_b_tick(void) { strcat(g_disp_trace, "Tb"); }
static void disp_h_putc(char c) { size_t n = strlen(g_disp_trace); if (n < 60) { g_disp_trace[n] = 'H'; g_disp_trace[n + 1] = c; g_disp_trace[n + 2] = 0; } }
static int probe_no(const vibeos_dev_env_t *e) { (void)e; return -1; }

static const vibeos_display_ops_t ops_a = { disp_a_putc, disp_a_tick };
static const vibeos_display_ops_t ops_b = { disp_b_putc, disp_b_tick };
static const vibeos_display_ops_t ops_no_tick = { disp_h_putc, 0 };
static const vibeos_device_t d_absent = {
    "absent", VIBEOS_DEV_DISPLAY, VIBEOS_DEVICE_NO_IRQ, probe_no, 0, 0, 0, &ops_a };
static const vibeos_device_t d_half = {
    "half", VIBEOS_DEV_DISPLAY, VIBEOS_DEVICE_NO_IRQ, 0, 0, 0, 0, &ops_no_tick };
static const vibeos_device_t d_whole = {
    "whole", VIBEOS_DEV_DISPLAY, VIBEOS_DEVICE_NO_IRQ, 0, 0, 0, 0, &ops_b };

static void write_str(const char *s) {
    while (*s) {
        vibeos_gui_putc(*s++);
    }
}

static int row_is(uint32_t r, const char *want) {
    char buf[VIBEOS_GUI_TERM_COLS + 1u];
    return vibeos_gui_term_row(r, buf) >= 0 && strcmp(buf, want) == 0;
}

#define W 200u
#define H 150u

int test_gui(void) {
    fenced_t fb, back;
    uint64_t back_bytes = (uint64_t)W * H * 4u + VIBEOS_GUI_GUARD_BYTES;
    vibeos_gui_stats_t st;
    char line[VIBEOS_GUI_TERM_COLS + 8u];
    uint32_t i;
    int ok = 1;

    g_depth = g_nested = g_locks = g_refused = g_bad_name = 0;
    vibeos_device_set_lock_ops(t_lock, t_unlock);
    vibeos_gui_reset();
    fenced_alloc(&fb, (uint64_t)W * H);
    fenced_alloc(&back, back_bytes / 4u);

    /* ---- refusals, by name, touching nothing ------------------------------------ */
    vibeos_gui_putc('x');
    vibeos_gui_stats(&st);
    ok &= expect(st.chars == 0u, "putc before init is ignored, not buffered into a grid nobody draws");
    ok &= expect(vibeos_gui_init(0, W, H, back.p, back_bytes) == -1 &&
                 strcmp(vibeos_gui_why(), "no framebuffer") == 0,
                 "no framebuffer is refused by name");
    ok &= expect(vibeos_gui_init((uint64_t)(uintptr_t)fb.p, VIBEOS_GUI_MAX_W + 1u, H,
                                 back.p, back_bytes) == -1 &&
                 strstr(vibeos_gui_why(), "larger") != 0,
                 "a screen wider than the buffer is sized for is refused by name");
    /* The two geometries the old clamps wrapped on: a text area 25 - 26 rows
     * tall, and a window 8 - 8 pixels wide. Both used to be accepted and then
     * fill four billion rows or columns. */
    ok &= expect(vibeos_gui_init((uint64_t)(uintptr_t)fb.p, 1024u, 30u, back.p, back_bytes) == -1 &&
                 strstr(vibeos_gui_why(), "smaller") != 0,
                 "a screen 30 pixels high is refused by name (the text area was 25 - 26)");
    ok &= expect(vibeos_gui_init((uint64_t)(uintptr_t)fb.p, 9u, 150u, back.p, back_bytes) == -1 &&
                 strstr(vibeos_gui_why(), "smaller") != 0,
                 "a screen 9 pixels wide is refused by name");
    ok &= expect(vibeos_gui_init((uint64_t)(uintptr_t)fb.p, W, H, back.p, back_bytes - 1u) == -1 &&
                 strstr(vibeos_gui_why(), "back buffer") != 0,
                 "a back buffer with no room for the canary is refused by name");
    ok &= expect(!vibeos_gui_active() && fenced_untouched(&fb) && fenced_untouched(&back),
                 "a refused init writes nothing, to either buffer");

    /* ---- the desktop, and its canary ---------------------------------------------- */
    ok &= expect(vibeos_gui_init((uint64_t)(uintptr_t)fb.p, W, H, back.p, back_bytes) == 0 &&
                 vibeos_gui_active() && strcmp(vibeos_gui_why(), "ok") == 0,
                 "a screen in range comes up and says ok");
    ok &= expect(fenced_intact(&fb) && fenced_intact(&back),
                 "composing the desktop stays inside both buffers");
    ok &= expect(memcmp(fb.p, back.p, (size_t)W * H * 4u) == 0,
                 "the first frame is the whole composition");
    ok &= expect(vibeos_gui_init((uint64_t)(uintptr_t)fb.p, W, H, back.p, back_bytes) == -1 &&
                 strcmp(vibeos_gui_why(), "already active") == 0,
                 "a second init is refused - it would recompose under a live repaint");

    /* ---- the terminal ---------------------------------------------------------------- */
    write_str("ab\r\ncd");
    ok &= expect(row_is(0, "ab") && row_is(1, "cd"), "text lands in rows; \\r is ignored");
    write_str("\b\bx");
    ok &= expect(row_is(1, "x"), "backspace removes what it passes over");
    write_str("\n");
    for (i = 0; i < VIBEOS_GUI_TERM_COLS - 1u; i++) {
        vibeos_gui_putc('w');
    }
    vibeos_gui_putc('y');
    memset(line, 'w', VIBEOS_GUI_TERM_COLS - 1u);
    line[VIBEOS_GUI_TERM_COLS - 1u] = 0;
    ok &= expect(row_is(2, line) && row_is(3, "y"),
                 "a row holds 71 characters and the 72nd starts the next");
    vibeos_gui_reset();
    vibeos_gui_init((uint64_t)(uintptr_t)fb.p, W, H, back.p, back_bytes);
    for (i = 0; i < 30u; i++) {
        char l[8];
        snprintf(l, sizeof(l), "L%02u\n", i);
        write_str(l);
    }
    vibeos_gui_stats(&st);
    ok &= expect(st.scrolls == 7u && row_is(0, "L07") && row_is(22, "L29") && row_is(23, ""),
                 "thirty lines scroll seven times and keep the last twenty-three plus the cursor row");
    ok &= expect(st.chars == 90u && st.term_chars == 69u,
                 "chars counts what arrived, term_chars what is on screen");
    /* Wrapping on the last row scrolls before it writes. The version before C7
     * wrapped, found the row past the end, skipped the write and scrolled
     * afterwards - so the character that caused the wrap was lost. */
    for (i = 0; i < VIBEOS_GUI_TERM_COLS; i++) {
        vibeos_gui_putc('q');
    }
    memset(line, 'q', VIBEOS_GUI_TERM_COLS - 1u);
    ok &= expect(row_is(22, line) && row_is(23, "q"),
                 "the character that wraps the last row is written, not lost");

    /* ---- the repaint -------------------------------------------------------------- */
    vibeos_device_reset();
    {
        const vibeos_device_t *t[] = { &g_mouse };
        vibeos_device_set_table(t, 1u);
    }
    g_px = 20;
    g_py = 30;
    vibeos_gui_tick();
    vibeos_gui_stats(&st);
    ok &= expect(st.frames == 1u && st.guard_checks == 1u && st.guard_broken == 0u,
                 "a repaint draws the pointer and examines the canary");
    ok &= expect(fenced_intact(&fb) && fenced_intact(&back), "the repaint stays inside both buffers");
    {
        /* The pointer is on the screen and not in the composition: outside its
         * rectangle the two buffers agree. */
        uint32_t x, y;
        int same = 1;
        for (y = 0; y < H; y++) {
            for (x = 0; x < W; x++) {
                int in = x >= 20u && x < 29u && y >= 30u && y < 42u;
                if (!in && fb.p[y * W + x] != back.p[y * W + x]) {
                    same = 0;
                }
            }
        }
        ok &= expect(same, "outside the pointer the screen is the composition");
        ok &= expect(memcmp(&fb.p[30u * W + 20u], &back.p[30u * W + 20u], 9u * 4u) != 0,
                     "the pointer is on the screen and not baked into the composition");
    }
    vibeos_gui_tick();
    vibeos_gui_stats(&st);
    ok &= expect(st.frames == 1u && st.guard_checks == 2u,
                 "a pointer that did not move is not redrawn; the canary is checked anyway");

    /* The canary, on the repaint - not only at the end of the boot. */
    back.p[W * H + 5u] ^= 1u;
    vibeos_gui_tick();
    vibeos_gui_stats(&st);
    ok &= expect(st.guard_broken == 1u, "one broken canary word is one, found at the next repaint");
    vibeos_gui_tick();
    vibeos_gui_stats(&st);
    ok &= expect(st.guard_broken == 1u, "and stays one: the worst seen, not a number that grows with uptime");
    back.p[W * H + 5u] ^= 1u;

    /* ---- reentrancy: a print from inside the lock is dropped and counted ---------- */
    g_reenter_on_lock = 1;
    vibeos_gui_putc('k');
    vibeos_gui_stats(&st);
    ok &= expect(st.reentered == 1u && g_refused == 1 && !g_nested,
                 "a putc by the lock's own holder is dropped and counted, not a wait on itself");
    ok &= expect(row_is(23, "qk"), "and only the holder's character reached the grid");

    ok &= expect(!g_nested && g_depth == 0 && !g_bad_name && g_locks > 0,
                 "every call took the GUI's own lock once and released it");
    vibeos_gui_stats(&st);
    ok &= expect(st.term_overrun == 0u, "no grid write fell outside the grid");

    /* ---- the display class -------------------------------------------------------- */
    vibeos_device_reset();
    g_disp_trace[0] = 0;
    vibeos_display_putc('x');
    vibeos_display_tick();
    ok &= expect(!vibeos_display_present() && g_disp_trace[0] == 0,
                 "no display: nothing is called and the registry says so");
    {
        const vibeos_device_t *t[] = { &d_absent, &d_half, &d_whole };
        vibeos_dev_env_t env;
        memset(&env, 0, sizeof(env));
        vibeos_device_set_table(t, 3u);
        (void)vibeos_device_probe_all(&env);
    }
    vibeos_display_putc('x');
    /* Checked before the repaint is asked for: handed the display with no
     * repaint, the tick would call through a null pointer and the failure
     * would be a crash with no name. */
    if (expect(vibeos_display_present() && strcmp(g_disp_trace, "Bx") == 0,
               "the display is the first present one with both operations - not the "
               "one whose probe failed, not the one with no repaint")) {
        vibeos_display_tick();
        ok &= expect(strcmp(g_disp_trace, "BxTb") == 0, "and the repaint goes to the same one");
    } else {
        ok = 0;
    }

    vibeos_device_reset();
    vibeos_device_set_lock_ops(0, 0);
    vibeos_gui_reset();
    free(fb.raw);
    free(back.raw);
    return ok ? 0 : -1;
}
