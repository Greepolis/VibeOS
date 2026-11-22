/* SPDX-License-Identifier: GPL-3.0-or-later */
/* vsh's line editor (docs/shell/s4.md step 4).
 *
 * The line is redrawn from where it starts, in rows counted relative to the
 * cursor, so a line longer than the screen survives the screen scrolling under
 * it. Typing at the end of a line that does not reach the margin prints the
 * byte alone: anything reading the terminal sees what was typed, once. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vsh_edit.h"

#define CTRL(x) ((unsigned char)((x) & 0x1f))
#define VSH_LIST_MAX 200u

static char *copy_of(const char *s) {
    size_t n = strlen(s) + 1u;
    char *p = malloc(n);

    if (!p) {
        (void)!fputs("vsh: out of memory\n", stderr);
        exit(2);
    }
    memcpy(p, s, n);
    return p;
}

/* ---- history ---------------------------------------------------------------- */

int vsh_hist_add(vsh_hist_t *h, const char *line) {
    if (!line[0] || (h->n > 0u && strcmp(h->items[h->n - 1u], line) == 0)) {
        return 0;
    }
    if (h->n == VSH_HIST_MAX) {
        free(h->items[0]);
        memmove(h->items, h->items + 1, (VSH_HIST_MAX - 1u) * sizeof(h->items[0]));
        h->n--;
    }
    h->items[h->n++] = copy_of(line);
    return 1;
}

void vsh_hist_clear(vsh_hist_t *h) {
    size_t i;

    for (i = 0; i < h->n; i++) {
        free(h->items[i]);
    }
    h->n = 0;
}

void vsh_names_add(vsh_names_t *l, const char *name) {
    size_t i;

    for (i = 0; i < l->n; i++) {
        if (strcmp(l->items[i], name) == 0) {
            return;     /* the same program in two PATH directories */
        }
    }
    if (l->n == l->cap) {
        size_t cap = l->cap ? l->cap * 2u : 32u;
        char **p = realloc(l->items, cap * sizeof(*p));

        if (!p) {
            (void)!fputs("vsh: out of memory\n", stderr);
            exit(2);
        }
        l->items = p;
        l->cap = cap;
    }
    l->items[l->n++] = copy_of(name);
}

static void names_free(vsh_names_t *l) {
    size_t i;

    for (i = 0; i < l->n; i++) {
        free(l->items[i]);
    }
    free(l->items);
    l->items = 0;
    l->n = l->cap = 0;
}

/* ---- the text --------------------------------------------------------------- */

static int is_cont(char c) {
    return ((unsigned char)c & 0xC0u) == 0x80u;
}

/* Columns: a character is one, whatever UTF-8 spends on it. */
static size_t width(const char *s, size_t n) {
    size_t i, w = 0;

    for (i = 0; i < n; i++) {
        w += !is_cont(s[i]);
    }
    return w;
}

static void reserve(vsh_edit_t *e, size_t more) {
    if (e->len + more + 1u > e->cap) {
        size_t cap = (e->len + more + 1u) * 2u;
        char *p = realloc(e->b, cap);

        if (!p) {
            (void)!fputs("vsh: out of memory\n", stderr);
            exit(2);
        }
        e->b = p;
        e->cap = cap;
    }
}

static void set_text(vsh_edit_t *e, const char *s) {
    size_t n = strlen(s);

    e->len = 0;
    reserve(e, n);
    memcpy(e->b, s, n + 1u);
    e->len = e->pos = n;
}

static void insert(vsh_edit_t *e, const char *s, size_t n) {
    reserve(e, n);
    memmove(e->b + e->pos + n, e->b + e->pos, e->len - e->pos + 1u);
    memcpy(e->b + e->pos, s, n);
    e->pos += n;
    e->len += n;
}

static void cut(vsh_edit_t *e, size_t from, size_t to) {
    memmove(e->b + from, e->b + to, e->len - to + 1u);
    e->len -= to - from;
    e->pos = from;
}

static size_t prev_char(const vsh_edit_t *e, size_t p) {
    if (p > 0u) {
        p--;
        while (p > 0u && is_cont(e->b[p])) {
            p--;
        }
    }
    return p;
}

static size_t next_char(const vsh_edit_t *e, size_t p) {
    if (p < e->len) {
        p++;
        while (p < e->len && is_cont(e->b[p])) {
            p++;
        }
    }
    return p;
}

/* A letter of any alphabet counts: `perché` is one word to Ctrl+Right. */
static int in_word(char c) {
    unsigned char u = (unsigned char)c;

    return u >= 0x80u || u == '_' || (u >= '0' && u <= '9') || (u >= 'a' && u <= 'z') ||
           (u >= 'A' && u <= 'Z');
}

static size_t word_left(const vsh_edit_t *e, size_t p) {
    while (p > 0u && !in_word(e->b[p - 1u])) {
        p--;
    }
    while (p > 0u && in_word(e->b[p - 1u])) {
        p--;
    }
    return p;
}

static size_t word_right(const vsh_edit_t *e, size_t p) {
    while (p < e->len && !in_word(e->b[p])) {
        p++;
    }
    while (p < e->len && in_word(e->b[p])) {
        p++;
    }
    return p;
}

/* ---- the screen ------------------------------------------------------------- */

static void csi(vsh_edit_t *e, size_t n, char what) {
    char t[32];

    if (n > 0u) {
        int k = snprintf(t, sizeof(t), "\x1b[%zu%c", n, what);

        vsh_sb_putn(&e->out, t, (size_t)k);
    }
}

static void draw_prompt(vsh_edit_t *e, char *out, size_t cap) {
    if (e->searching) {
        snprintf(out, cap, "(cerca%s) '%s': ", e->found == e->hist->n && e->query_len ? " - nulla" : "",
                 e->query);
    } else {
        snprintf(out, cap, "%s", e->prompt);
    }
}

static void refresh(vsh_edit_t *e) {
    char p[192];
    size_t total, at, end_row, row;

    draw_prompt(e, p, sizeof(p));
    csi(e, e->cursor_row, 'A');
    vsh_sb_puts(&e->out, "\r\x1b[J");
    vsh_sb_puts(&e->out, p);
    vsh_sb_putn(&e->out, e->b, e->len);
    total = width(p, strlen(p)) + width(e->b, e->len);
    at = width(p, strlen(p)) + width(e->b, e->pos);
    /* Exactly at the margin a terminal holds the cursor on the last column
     * until the next character: make the wrap happen, so a row count is true. */
    if (total > 0u && total % e->cols == 0u) {
        vsh_sb_puts(&e->out, "\r\n");
    }
    end_row = total / e->cols;
    row = at / e->cols;
    csi(e, end_row - row, 'A');
    vsh_sb_putc(&e->out, '\r');
    csi(e, at % e->cols, 'C');
    e->cursor_row = row;
}

/* Below the whole line, wherever the cursor is in it. */
static void leave_line(vsh_edit_t *e) {
    char p[192];
    size_t total;

    draw_prompt(e, p, sizeof(p));
    total = width(p, strlen(p)) + width(e->b, e->len);
    csi(e, total / e->cols - e->cursor_row, 'B');
    vsh_sb_puts(&e->out, "\r\n");
    e->cursor_row = 0;
}

/* ---- history keys ----------------------------------------------------------- */

static void show_hist(vsh_edit_t *e, size_t at) {
    if (e->hist_at == e->hist->n) {
        free(e->draft);
        e->draft = copy_of(e->b);
    }
    e->hist_at = at;
    set_text(e, at == e->hist->n ? (e->draft ? e->draft : "") : e->hist->items[at]);
    refresh(e);
}

static void search_from(vsh_edit_t *e, size_t before) {
    size_t i;

    e->found = e->hist->n;
    for (i = before; e->query_len > 0u && i > 0u; i--) {
        const char *hit = strstr(e->hist->items[i - 1u], e->query);

        if (hit) {
            e->found = i - 1u;
            set_text(e, e->hist->items[i - 1u]);
            e->pos = (size_t)(hit - e->hist->items[i - 1u]);
            break;
        }
    }
    refresh(e);
}

static void search_leave(vsh_edit_t *e) {
    e->searching = 0;
    refresh(e);
}

/* ---- completion -------------------------------------------------------------- */

/* Where the word under the cursor starts, read from the beginning of the line
 * because only that knows which spaces are inside quotes. */
static size_t word_start(const vsh_edit_t *e, int *first) {
    size_t i, start = 0;
    char quote = 0;
    int seen = 0, cmd_seen = 0;

    for (i = 0; i < e->pos; i++) {
        char c = e->b[i];

        if (quote) {
            quote = c == quote ? 0 : quote;
        } else if (c == '\\' && i + 1u < e->pos) {
            i++;
            seen = 1;
        } else if (c == '\'' || c == '"') {
            quote = c;
            seen = 1;
        } else if (c == ' ' || c == '\t') {
            start = i + 1u;
            cmd_seen |= seen;
            seen = 0;
        } else if (c == ';' || c == '|' || c == '&' || c == '(') {
            start = i + 1u;
            seen = 0;
            cmd_seen = 0;
        } else {
            seen = 1;
        }
    }
    *first = !cmd_seen;
    return start;
}

/* The word as a name: its quotes and escapes gone. */
static char *word_text(const char *s, size_t n) {
    char *out = malloc(n + 1u);
    size_t i, k = 0;
    char quote = 0;

    if (!out) {
        (void)!fputs("vsh: out of memory\n", stderr);
        exit(2);
    }
    for (i = 0; i < n; i++) {
        char c = s[i];

        if (quote) {
            if (c == quote) {
                quote = 0;
            } else {
                out[k++] = c;
            }
        } else if (c == '\'' || c == '"') {
            quote = c;
        } else if (c == '\\' && i + 1u < n && strchr(" '\"$\\", s[i + 1u])) {
            out[k++] = s[++i];
        } else {
            out[k++] = c;
        }
    }
    out[k] = 0;
    return out;
}

/* The name as a word. A backslash escapes only a few characters in this
 * language, so anything else that means something goes in single quotes. */
static void put_quoted(vsh_sb_t *o, const char *name) {
    const char *p;

    if (!strpbrk(name, " \t'\"$\\;|&()<>*?[]{}%#~=")) {
        vsh_sb_puts(o, name);
    } else if (!strchr(name, '\'')) {
        vsh_sb_putc(o, '\'');
        vsh_sb_puts(o, name);
        vsh_sb_putc(o, '\'');
    } else {
        for (p = name; *p; p++) {
            if (*p == '\'') {
                vsh_sb_puts(o, "\"'\"");
            } else {
                vsh_sb_putc(o, '\'');
                while (*p && *p != '\'') {
                    vsh_sb_putc(o, *p++);
                }
                vsh_sb_putc(o, '\'');
                p--;
            }
        }
    }
}

static int by_name(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static void list_names(vsh_edit_t *e, vsh_names_t *l) {
    size_t i, widest = 0, per_row, shown = l->n > VSH_LIST_MAX ? VSH_LIST_MAX : l->n;
    char t[64];

    for (i = 0; i < shown; i++) {
        size_t w = width(l->items[i], strlen(l->items[i]));

        widest = w > widest ? w : widest;
    }
    per_row = e->cols / (widest + 2u);
    per_row = per_row ? per_row : 1u;
    leave_line(e);
    for (i = 0; i < shown; i++) {
        size_t w = width(l->items[i], strlen(l->items[i])), pad;

        vsh_sb_puts(&e->out, l->items[i]);
        if ((i + 1u) % per_row == 0u || i + 1u == shown) {
            vsh_sb_puts(&e->out, "\r\n");
        } else {
            for (pad = w; pad < widest + 2u; pad++) {
                vsh_sb_putc(&e->out, ' ');
            }
        }
    }
    if (shown < l->n) {
        snprintf(t, sizeof(t), "... e altri %zu\r\n", l->n - shown);
        vsh_sb_puts(&e->out, t);
    }
    refresh(e);
}

static void replace_word(vsh_edit_t *e, size_t start, const char *name, int finished) {
    vsh_sb_t o = {0, 0, 0};
    size_t n = strlen(name);

    vsh_sb_putn(&o, "", 0u);
    put_quoted(&o, name);
    if (finished && (n == 0u || name[n - 1u] != '/')) {
        vsh_sb_putc(&o, ' ');
    }
    cut(e, start, e->pos);
    insert(e, o.b, o.n);
    vsh_sb_free(&o);
    refresh(e);
}

static void complete_word(vsh_edit_t *e) {
    vsh_names_t l = {0, 0, 0};
    int first = 0, again = e->tabbed;
    size_t start = word_start(e, &first), common, i;
    char *prefix = word_text(e->b + start, e->pos - start);

    e->tabbed = 0;
    if (e->complete) {
        e->complete(e->complete_user, prefix, first, &l);
    }
    if (l.n == 1u) {
        replace_word(e, start, l.items[0], 1);
    } else if (l.n > 1u) {
        qsort(l.items, l.n, sizeof(l.items[0]), by_name);
        common = strlen(l.items[0]);
        for (i = 1; i < l.n; i++) {
            size_t k = 0;

            while (k < common && l.items[i][k] == l.items[0][k]) {
                k++;
            }
            common = k;
        }
        /* Never half a character. */
        while (common > 0u && is_cont(l.items[0][common])) {
            common--;
        }
        if (common > strlen(prefix)) {
            l.items[0][common] = 0;
            replace_word(e, start, l.items[0], 0);
        } else if (again) {
            list_names(e, &l);
        } else {
            e->tabbed = 1;
        }
    }
    free(prefix);
    names_free(&l);
}

/* ---- keys ---------------------------------------------------------------------- */

static void key_left(vsh_edit_t *e) { e->pos = prev_char(e, e->pos); refresh(e); }
static void key_right(vsh_edit_t *e) { e->pos = next_char(e, e->pos); refresh(e); }
static void key_home(vsh_edit_t *e) { e->pos = 0; refresh(e); }
static void key_end(vsh_edit_t *e) { e->pos = e->len; refresh(e); }
static void key_word_left(vsh_edit_t *e) { e->pos = word_left(e, e->pos); refresh(e); }
static void key_word_right(vsh_edit_t *e) { e->pos = word_right(e, e->pos); refresh(e); }

static void key_up(vsh_edit_t *e) {
    if (e->hist_at > 0u) {
        show_hist(e, e->hist_at - 1u);
    }
}

static void key_down(vsh_edit_t *e) {
    if (e->hist_at < e->hist->n) {
        show_hist(e, e->hist_at + 1u);
    }
}

static void key_delete(vsh_edit_t *e) {
    if (e->pos < e->len) {
        cut(e, e->pos, next_char(e, e->pos));
        refresh(e);
    }
}

static void key_backspace(vsh_edit_t *e) {
    if (e->pos > 0u) {
        cut(e, prev_char(e, e->pos), e->pos);
        refresh(e);
    }
}

static void key_kill_word_left(vsh_edit_t *e) {
    cut(e, word_left(e, e->pos), e->pos);
    refresh(e);
}

static void key_kill_word_right(vsh_edit_t *e) {
    cut(e, e->pos, word_right(e, e->pos));
    refresh(e);
}

/* ESC [ ... or ESC O ...: the sequence is whole. Modifier 5 is Ctrl, 3 Alt. */
static void sequence(vsh_edit_t *e) {
    char last = e->seq[e->seq_len - 1u];
    int code = 0, mod = 1;

    e->seq[e->seq_len] = 0;
    if (e->seq[1] == '[') {
        return;     /* the Linux console's F1 to F5: ESC [ [ A is not an arrow */
    }
    (void)sscanf(e->seq + 1, "%d;%d", &code, &mod);
    if (last == '~') {
        int fast = mod == 5 || mod == 3;

        switch (code) {
            case 1: case 7: key_home(e); break;
            case 4: case 8: key_end(e); break;
            case 3: if (fast) { key_kill_word_right(e); } else { key_delete(e); } break;
            default: break;
        }
        return;
    }
    /* rxvt spells Ctrl+arrow ESC O c and ESC O d. */
    if (e->seq[0] == 'O' && last == 'c') { key_word_right(e); return; }
    if (e->seq[0] == 'O' && last == 'd') { key_word_left(e); return; }
    switch (last) {
        case 'A': key_up(e); break;
        case 'B': key_down(e); break;
        case 'C': if (mod == 5 || mod == 3) { key_word_right(e); } else { key_right(e); } break;
        case 'D': if (mod == 5 || mod == 3) { key_word_left(e); } else { key_left(e); } break;
        case 'H': key_home(e); break;
        case 'F': key_end(e); break;
        default: break;
    }
}

static vsh_ed_result_t accept(vsh_edit_t *e) {
    /* Only if something has to change: a line typed to its end is on the
     * screen already, and printing it again shows it twice to whatever reads
     * the terminal. */
    if (e->searching || e->pos != e->len) {
        e->searching = 0;
        e->pos = e->len;
        refresh(e);
    }
    leave_line(e);
    return VSH_ED_LINE;
}

static vsh_ed_result_t search_key(vsh_edit_t *e, unsigned char c) {
    if (c == CTRL('R')) {
        search_from(e, e->found < e->hist->n ? e->found : e->hist->n);
    } else if (c == 0x7f || c == '\b') {
        if (e->query_len > 0u) {
            do {
                e->query_len--;
            } while (e->query_len > 0u && is_cont(e->query[e->query_len]));
            e->query[e->query_len] = 0;
            search_from(e, e->hist->n);
        }
    } else if (c == '\r' || c == '\n') {
        return accept(e);
    } else if (c == CTRL('C') || c == CTRL('G')) {
        e->searching = 0;
        set_text(e, e->draft ? e->draft : "");
        refresh(e);
    } else if (c == 0x1b) {
        search_leave(e);
        e->esc = 1;
    } else if (c < 0x20u) {
        search_leave(e);
        return vsh_edit_feed(e, c);
    } else if (e->query_len + 1u < sizeof(e->query)) {
        e->query[e->query_len++] = (char)c;
        e->query[e->query_len] = 0;
        search_from(e, e->hist->n);
    }
    return VSH_ED_MORE;
}

void vsh_edit_begin(vsh_edit_t *e, const char *prompt, vsh_hist_t *hist, size_t cols,
                    vsh_complete_fn complete, void *user) {
    memset(e, 0, sizeof(*e));
    e->prompt = prompt;
    e->hist = hist;
    e->hist_at = hist->n;
    e->found = hist->n;
    e->cols = cols >= 8u ? cols : 80u;
    e->complete = complete;
    e->complete_user = user;
    reserve(e, 64u);
    e->b[0] = 0;
    vsh_sb_putn(&e->out, "", 0u);
    /* Printed and not redrawn: a redraw starts by clearing the row, and a
     * program's last line without a newline is on it. */
    vsh_sb_puts(&e->out, prompt);
    e->cursor_row = width(prompt, strlen(prompt)) / e->cols;
    if (width(prompt, strlen(prompt)) % e->cols == 0u) {
        vsh_sb_puts(&e->out, "\r\n");
    }
}

void vsh_edit_esc_alone(vsh_edit_t *e) {
    e->esc = 0;
    e->seq_len = 0;
}

void vsh_edit_end(vsh_edit_t *e) {
    free(e->b);
    free(e->draft);
    vsh_sb_free(&e->out);
    e->b = e->draft = 0;
}

vsh_ed_result_t vsh_edit_feed(vsh_edit_t *e, unsigned char c) {
    if (e->esc == 1) {
        e->esc = 0;
        if (c == '[' || c == 'O') {
            e->esc = 2;
            e->seq[0] = (char)c;
            e->seq_len = 1;
            return VSH_ED_MORE;
        }
        /* Alt+letter, as a terminal sends it. */
        if (c == 'b') { key_word_left(e); return VSH_ED_MORE; }
        if (c == 'f') { key_word_right(e); return VSH_ED_MORE; }
        if (c == 'd') { key_kill_word_right(e); return VSH_ED_MORE; }
        if (c == 0x7f) { key_kill_word_left(e); return VSH_ED_MORE; }
        /* The key before was Esc itself; this one is a key of its own. */
    } else if (e->esc == 2) {
        if (e->seq_len + 1u < sizeof(e->seq)) {
            e->seq[e->seq_len++] = (char)c;
        }
        /* A final byte ends it, even one too long to have been kept. */
        if (c >= 0x40u && c <= 0x7eu && !(c == '[' && e->seq_len == 2u)) {
            e->esc = 0;
            if (e->seq_len + 1u < sizeof(e->seq)) {
                sequence(e);
            }
            e->seq_len = 0;
        }
        return VSH_ED_MORE;
    }
    if (e->searching) {
        return search_key(e, c);
    }
    if (c != '\t') {
        e->tabbed = 0;
    }
    switch (c) {
        case '\r': case '\n': return accept(e);
        case CTRL('C'):
            e->pos = e->len;
            refresh(e);
            vsh_sb_puts(&e->out, "^C");
            leave_line(e);
            return VSH_ED_CANCEL;
        case CTRL('D'):
            if (e->len == 0u) {
                return VSH_ED_EOF;
            }
            key_delete(e);
            break;
        case 0x7f: case '\b': key_backspace(e); break;
        case CTRL('A'): key_home(e); break;
        case CTRL('E'): key_end(e); break;
        case CTRL('B'): key_left(e); break;
        case CTRL('F'): key_right(e); break;
        case CTRL('P'): key_up(e); break;
        case CTRL('N'): key_down(e); break;
        case CTRL('K'): e->len = e->pos; e->b[e->len] = 0; refresh(e); break;
        case CTRL('U'): cut(e, 0, e->pos); refresh(e); break;
        case CTRL('W'): key_kill_word_left(e); break;
        case CTRL('L'):
            vsh_sb_puts(&e->out, "\x1b[H\x1b[2J");
            e->cursor_row = 0;
            refresh(e);
            break;
        case CTRL('R'):
            free(e->draft);
            e->draft = copy_of(e->b);
            e->searching = 1;
            e->query[0] = 0;
            e->query_len = 0;
            e->found = e->hist->n;
            refresh(e);
            break;
        case '\t': complete_word(e); break;
        case 0x1b: e->esc = 1; break;
        default:
            if (c >= 0x20u) {
                char one = (char)c;
                int at_end = e->pos == e->len;
                size_t total;

                insert(e, &one, 1u);
                total = width(e->prompt, strlen(e->prompt)) + width(e->b, e->len);
                if (at_end && total % e->cols != 0u) {
                    vsh_sb_putc(&e->out, one);
                    e->cursor_row = total / e->cols;
                } else {
                    refresh(e);
                }
            }
            break;
    }
    return VSH_ED_MORE;
}
