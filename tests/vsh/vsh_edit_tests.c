/* SPDX-License-Identifier: GPL-3.0-or-later */
/* vsh's line editor on the host (docs/shell/s4.md step 4): each key, then random
 * keys against a model of a terminal that shares no code with the editor. After
 * every key the screen has to show the prompt and the line, with the cursor on
 * the character the editor says it is on. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vsh_edit.h"

static int g_failed, g_checks;

static void check(int ok, const char *what, const char *detail) {
    g_checks++;
    if (!ok) {
        g_failed++;
        printf("FAIL:edit %s%s%s\n", what, detail ? " -> " : "", detail ? detail : "");
    }
}

/* ---- a terminal -------------------------------------------------------------- */

#define SCR_ROWS 60
#define SCR_COLS 64

typedef struct {
    char cell[SCR_ROWS][SCR_COLS];
    int cols, row, col;
    int pending;    /* written in the last column: the wrap waits for the next */
} screen_t;

static void scr_init(screen_t *s, int cols) {
    memset(s, ' ', sizeof(s->cell));
    s->cols = cols;
    s->row = s->col = s->pending = 0;
}

static void scr_down(screen_t *s) {
    if (s->row + 1 < SCR_ROWS) {
        s->row++;
    } else {
        memmove(s->cell[0], s->cell[1], (SCR_ROWS - 1) * SCR_COLS);
        memset(s->cell[SCR_ROWS - 1], ' ', SCR_COLS);
    }
}

static void scr_feed(screen_t *s, const char *b, size_t n) {
    size_t i;

    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)b[i];

        if (c == '\r') {
            s->col = s->pending = 0;
        } else if (c == '\n') {
            scr_down(s);
            s->pending = 0;
        } else if (c == 0x1b && i + 1 < n && b[i + 1] == '[') {
            int v = 0, have = 0;

            i += 2;
            while (i < n && b[i] >= '0' && b[i] <= '9') {
                v = v * 10 + (b[i++] - '0');
                have = 1;
            }
            v = have ? v : 1;
            s->pending = 0;
            switch (i < n ? b[i] : 0) {
                case 'A': s->row = s->row >= v ? s->row - v : 0; break;
                case 'B': s->row = s->row + v < SCR_ROWS ? s->row + v : SCR_ROWS - 1; break;
                case 'C': s->col = s->col + v < s->cols ? s->col + v : s->cols - 1; break;
                case 'D': s->col = s->col >= v ? s->col - v : 0; break;
                case 'H': s->row = s->col = 0; break;
                case 'J': {
                    int r;

                    if (have && v == 2) {
                        memset(s->cell, ' ', sizeof(s->cell));
                        break;
                    }
                    memset(s->cell[s->row] + s->col, ' ', (size_t)(SCR_COLS - s->col));
                    for (r = s->row + 1; r < SCR_ROWS; r++) {
                        memset(s->cell[r], ' ', SCR_COLS);
                    }
                    break;
                }
                default: check(0, "the editor sent a sequence the model does not know", 0); break;
            }
        } else if ((c & 0xC0u) == 0x80u) {
            /* the rest of a character already placed */
        } else {
            if (s->pending) {
                s->col = s->pending = 0;
                scr_down(s);
            }
            s->cell[s->row][s->col] = (char)c;
            if (s->col + 1 == s->cols) {
                s->pending = 1;
            } else {
                s->col++;
            }
        }
    }
}

/* ---- driving it --------------------------------------------------------------- */

typedef struct {
    vsh_edit_t e;
    screen_t s;
    vsh_ed_result_t last;
} rig_t;

static void pump(rig_t *r) {
    scr_feed(&r->s, r->e.out.b, r->e.out.n);
    r->e.out.n = 0;
}

static void start(rig_t *r, vsh_hist_t *h, int cols, vsh_complete_fn fn) {
    scr_init(&r->s, cols);
    vsh_edit_begin(&r->e, "p> ", h, (size_t)cols, fn, 0);
    r->last = VSH_ED_MORE;
    pump(r);
}

static void keys(rig_t *r, const char *k) {
    for (; *k; k++) {
        r->last = vsh_edit_feed(&r->e, (unsigned char)*k);
        pump(r);
    }
}

static void line_is(rig_t *r, const char *want, size_t pos, const char *what) {
    check(strcmp(r->e.b, want) == 0, what, r->e.b);
    check(r->e.pos == pos, what, "the cursor is elsewhere");
}

/* The screen against the editor: one character a column, from the row the
 * prompt is on. */
static void screen_agrees(rig_t *r, const char *what) {
    char want[4096];
    size_t n = 0, at = 0, i;
    const char *p = "p> ";
    int top;

    if (r->e.searching) {
        return;     /* its prompt is the editor's to spell */
    }
    for (i = 0; p[i]; i++) {
        want[n++] = p[i];
    }
    at = n;
    for (i = 0; i < r->e.len && n + 1 < sizeof(want); i++) {
        if (i == r->e.pos) {
            at = n;
        }
        if (((unsigned char)r->e.b[i] & 0xC0u) != 0x80u) {
            want[n++] = r->e.b[i];
        }
    }
    if (r->e.pos >= r->e.len) {
        at = n;
    }
    top = r->s.row - (int)(at / (size_t)r->s.cols);
    if (top < 0) {
        check(0, what, "the cursor is above where the line starts");
        return;
    }
    for (i = 0; i < n; i++) {
        int row = top + (int)(i / (size_t)r->s.cols), col = (int)(i % (size_t)r->s.cols);

        if (row >= SCR_ROWS || r->s.cell[row][col] != want[i]) {
            check(0, what, "the screen does not show the line");
            return;
        }
    }
    check(r->s.col == (int)(at % (size_t)r->s.cols) && !r->s.pending, what, "the cursor is not on its character");
}

/* ---- each key ------------------------------------------------------------------ */

static void fake_complete(void *user, const char *prefix, int first, vsh_names_t *out) {
    static const char *const names[] = {"alfa", "alto", "my file", "dir/", "solo.txt", 0};
    int i;

    (void)user;
    for (i = 0; names[i]; i++) {
        if (first && names[i][0] == 'm') {
            continue;
        }
        if (strncmp(names[i], prefix, strlen(prefix)) == 0) {
            vsh_names_add(out, names[i]);
        }
    }
}

#define LEFT "\x1b[D"
#define RIGHT "\x1b[C"
#define UP "\x1b[A"
#define DOWN "\x1b[B"
#define CLEFT "\x1b[1;5D"
#define CRIGHT "\x1b[1;5C"

static void test_moving(void) {
    vsh_hist_t h = {{0}, 0};
    rig_t r;

    start(&r, &h, 40, 0);
    keys(&r, "abc" LEFT LEFT "X");
    line_is(&r, "aXbc", 2, "insert in the middle");
    keys(&r, "\x1b[H");
    line_is(&r, "aXbc", 0, "Home");
    keys(&r, "\x1b[F");
    line_is(&r, "aXbc", 4, "End");
    keys(&r, "\x01");
    line_is(&r, "aXbc", 0, "Ctrl+A");
    keys(&r, "\x05");
    line_is(&r, "aXbc", 4, "Ctrl+E");
    keys(&r, "\x7f");
    line_is(&r, "aXb", 3, "Backspace");
    keys(&r, "\x01\x1b[3~");
    line_is(&r, "Xb", 0, "Delete");
    vsh_edit_end(&r.e);

    start(&r, &h, 40, 0);
    keys(&r, "uno due-tre  quattro");
    keys(&r, CLEFT);
    line_is(&r, "uno due-tre  quattro", 13, "Ctrl+Left to the word's start");
    keys(&r, CLEFT);
    line_is(&r, "uno due-tre  quattro", 8, "Ctrl+Left over the gap");
    keys(&r, CLEFT CLEFT);
    line_is(&r, "uno due-tre  quattro", 0, "Ctrl+Left to the line's start");
    keys(&r, CLEFT);
    line_is(&r, "uno due-tre  quattro", 0, "Ctrl+Left stops at the start");
    keys(&r, CRIGHT);
    line_is(&r, "uno due-tre  quattro", 3, "Ctrl+Right to the word's end");
    keys(&r, CRIGHT CRIGHT);
    line_is(&r, "uno due-tre  quattro", 11, "Ctrl+Right over a dash");
    keys(&r, CRIGHT CRIGHT);
    line_is(&r, "uno due-tre  quattro", 20, "Ctrl+Right stops at the end");
    keys(&r, "\x1b" "b");
    line_is(&r, "uno due-tre  quattro", 13, "Alt+b");
    keys(&r, "\x1b" "f");
    line_is(&r, "uno due-tre  quattro", 20, "Alt+f");
    keys(&r, "\x1bOd");
    line_is(&r, "uno due-tre  quattro", 13, "rxvt's Ctrl+Left");
    keys(&r, "\x1b[3;5~");
    line_is(&r, "uno due-tre  ", 13, "Ctrl+Delete");
    keys(&r, "\x17");
    line_is(&r, "uno due-", 8, "Ctrl+W");
    keys(&r, "\x15");
    line_is(&r, "", 0, "Ctrl+U");
    vsh_edit_end(&r.e);

    start(&r, &h, 40, 0);
    keys(&r, "perch\xc3\xa9 s\xc3\xac");
    keys(&r, LEFT);
    line_is(&r, "perch\xc3\xa9 s\xc3\xac", 9, "Left over a two-byte character");
    keys(&r, "\x7f");
    line_is(&r, "perch\xc3\xa9 \xc3\xac", 8, "Backspace of one byte");
    keys(&r, LEFT LEFT);
    line_is(&r, "perch\xc3\xa9 \xc3\xac", 5, "Left lands on a character's start");
    keys(&r, "\x7f");
    line_is(&r, "perc\xc3\xa9 \xc3\xac", 4, "Backspace before a wide one");
    keys(&r, "\x01" CRIGHT);
    line_is(&r, "perc\xc3\xa9 \xc3\xac", 6, "a word with an accent is one word");
    screen_agrees(&r, "the cursor counts characters, not bytes");
    keys(&r, "\x05");
    screen_agrees(&r, "the end of a line with accents");
    keys(&r, CLEFT CLEFT);
    line_is(&r, "perc\xc3\xa9 \xc3\xac", 0, "and back over it");
    keys(&r, "\x1b[3~\x1b[3~\x1b[3~\x1b[3~\x1b[3~");
    line_is(&r, " \xc3\xac", 0, "Delete takes the whole character");
    vsh_edit_end(&r.e);

    /* A key this editor does not know is not text. */
    (void)vsh_hist_add(&h, "storia");
    start(&r, &h, 40, 0);
    keys(&r, "a\x1b[[A\x1b[15~\x1b[99;99;99;99;99;99;99;99;99Zb");
    line_is(&r, "ab", 2, "unknown sequences insert nothing");
    vsh_edit_end(&r.e);
    vsh_hist_clear(&h);
}

static void test_ends(void) {
    vsh_hist_t h = {{0}, 0};
    rig_t r;

    start(&r, &h, 40, 0);
    keys(&r, "x\r");
    check(r.last == VSH_ED_LINE, "Enter ends the line", 0);
    vsh_edit_end(&r.e);
    start(&r, &h, 40, 0);
    keys(&r, "x\x03");
    check(r.last == VSH_ED_CANCEL, "Ctrl+C cancels", 0);
    vsh_edit_end(&r.e);
    start(&r, &h, 40, 0);
    keys(&r, "\x04");
    check(r.last == VSH_ED_EOF, "Ctrl+D on nothing is the end", 0);
    vsh_edit_end(&r.e);
    start(&r, &h, 40, 0);
    keys(&r, "ab\x01\x04");
    check(r.last == VSH_ED_MORE, "Ctrl+D on text is not the end", 0);
    line_is(&r, "b", 0, "Ctrl+D deletes under the cursor");
    vsh_edit_end(&r.e);
}

static void test_history(void) {
    vsh_hist_t h = {{0}, 0};
    rig_t r;
    size_t i;

    check(vsh_hist_add(&h, "uno") == 1, "history keeps a line", 0);
    check(vsh_hist_add(&h, "uno") == 0, "history skips a repeat", 0);
    check(vsh_hist_add(&h, "") == 0, "history skips nothing", 0);
    (void)vsh_hist_add(&h, "due");
    (void)vsh_hist_add(&h, "tre uno");

    start(&r, &h, 40, 0);
    keys(&r, "bozza" UP);
    line_is(&r, "tre uno", 7, "Up shows the last");
    keys(&r, UP UP UP UP);
    line_is(&r, "uno", 3, "Up stops at the first");
    keys(&r, DOWN);
    line_is(&r, "due", 3, "Down");
    keys(&r, DOWN DOWN);
    line_is(&r, "bozza", 5, "Down returns to what was being typed");
    keys(&r, "!" UP DOWN);
    line_is(&r, "bozza!", 6, "and to its latest version");

    keys(&r, "\x12uno");
    line_is(&r, "tre uno", 4, "Ctrl+R finds the newest");
    keys(&r, "\x12");
    line_is(&r, "uno", 0, "Ctrl+R again goes further back");
    keys(&r, "\x12");
    check(r.e.found == h.n, "Ctrl+R past the oldest finds none", 0);
    keys(&r, "\x07");
    line_is(&r, "bozza!", 6, "Ctrl+G gives the draft back");
    keys(&r, "\x12" "du\x1b[C");
    check(!r.e.searching, "an arrow leaves the search", 0);
    line_is(&r, "due", 1, "keeping what it found");
    keys(&r, "\r");
    check(r.last == VSH_ED_LINE && strcmp(r.e.b, "due") == 0, "Enter runs what it found", r.e.b);
    vsh_edit_end(&r.e);

    for (i = 0; i < VSH_HIST_MAX + 10u; i++) {
        char t[32];

        snprintf(t, sizeof(t), "n%zu", i);
        (void)vsh_hist_add(&h, t);
    }
    check(h.n == VSH_HIST_MAX, "history is bounded", 0);
    check(strcmp(h.items[h.n - 1u], "n1009") == 0, "and drops the oldest", h.items[h.n - 1u]);
    vsh_hist_clear(&h);
}

static void test_completion(void) {
    vsh_hist_t h = {{0}, 0};
    rig_t r;

    start(&r, &h, 40, fake_complete);
    keys(&r, "so\t");
    line_is(&r, "solo.txt ", 9, "one match, and a space");
    vsh_edit_end(&r.e);
    start(&r, &h, 40, fake_complete);
    keys(&r, "d\t");
    line_is(&r, "dir/", 4, "a directory takes no space");
    vsh_edit_end(&r.e);
    start(&r, &h, 40, fake_complete);
    keys(&r, "a\t");
    line_is(&r, "al", 2, "as far as they agree");
    keys(&r, "\t");
    line_is(&r, "al", 2, "first Tab with nothing to add");
    keys(&r, "\t");
    check(r.s.row > 0, "second Tab lists", 0);
    line_is(&r, "al", 2, "a list leaves the line");
    screen_agrees(&r, "the line comes back under the list");
    vsh_edit_end(&r.e);
    start(&r, &h, 40, fake_complete);
    keys(&r, "cat my\t");
    line_is(&r, "cat 'my file' ", 14, "a name with a space is quoted");
    vsh_edit_end(&r.e);
    start(&r, &h, 40, fake_complete);
    keys(&r, "my\t");
    line_is(&r, "my", 2, "the first word is asked for as a program");
    vsh_edit_end(&r.e);
    start(&r, &h, 40, fake_complete);
    keys(&r, "ls | so\t");
    line_is(&r, "ls | solo.txt ", 14, "after a pipe it is a first word again");
    vsh_edit_end(&r.e);
    start(&r, &h, 40, fake_complete);
    keys(&r, "cat 'my f\t");
    line_is(&r, "cat 'my file' ", 14, "inside an open quote");
    vsh_edit_end(&r.e);
}

/* Wrapped lines, exactly at the margin, edited from anywhere. */
static void test_random(void) {
    static const char *const k[] = {
        LEFT, RIGHT, CLEFT, CRIGHT, "\x7f", "\x1b[3~", "\x01", "\x05", UP, DOWN, "\x17", "\x0b",
        "\x15", "\x1b[3;5~", "a", "b c", "-", "xyz", "\xc3\xa9", "lungo ", "0123456789",
    };
    unsigned seed;

    for (seed = 1; seed <= 300u; seed++) {
        vsh_hist_t h = {{0}, 0};
        rig_t r;
        int round, cols = 8 + (int)(seed % 25u);
        char what[64];

        srand(seed);
        (void)vsh_hist_add(&h, "una riga di storia abbastanza lunga da andare a capo");
        (void)vsh_hist_add(&h, "breve");
        start(&r, &h, cols, 0);
        snprintf(what, sizeof(what), "seed %u cols %d", seed, cols);
        for (round = 0; round < 250; round++) {
            keys(&r, k[(size_t)rand() % (sizeof(k) / sizeof(k[0]))]);
            if (r.e.len > (size_t)cols * 5u) {
                keys(&r, "\x05\x15");
            }
            screen_agrees(&r, what);
            check(r.e.pos <= r.e.len && r.e.b[r.e.len] == 0, what, "the cursor left the text");
            check(r.e.pos == r.e.len || ((unsigned char)r.e.b[r.e.pos] & 0xC0u) != 0x80u, what,
                  "the cursor is inside a character");
            if (g_failed) {
                break;
            }
        }
        vsh_edit_end(&r.e);
        vsh_hist_clear(&h);
        if (g_failed) {
            return;
        }
    }
}

/* Whatever bytes arrive. */
static void test_noise(void) {
    unsigned seed;

    for (seed = 1; seed <= 100u; seed++) {
        vsh_hist_t h = {{0}, 0};
        vsh_edit_t e;
        int i;

        srand(seed);
        (void)vsh_hist_add(&h, "storia");
        vsh_edit_begin(&e, "p> ", &h, 20u, fake_complete, 0);
        for (i = 0; i < 4000; i++) {
            vsh_ed_result_t res = vsh_edit_feed(&e, (unsigned char)(rand() & 0xff));

            e.out.n = 0;
            if (res != VSH_ED_MORE) {
                vsh_edit_end(&e);
                vsh_edit_begin(&e, "p> ", &h, 20u, fake_complete, 0);
            }
            if (e.pos > e.len || e.b[e.len] != 0) {
                check(0, "noise broke the line", 0);
                break;
            }
        }
        vsh_edit_end(&e);
        vsh_hist_clear(&h);
    }
    check(1, "noise", 0);
}

int main(void) {
    test_moving();
    test_ends();
    test_history();
    test_completion();
    test_random();
    test_noise();
    if (g_failed) {
        printf("FAIL:edit %d of %d checks\n", g_failed, g_checks);
        return 1;
    }
    printf("edit: %d checks\n", g_checks);
    return 0;
}
