/* SPDX-License-Identifier: GPL-3.0-or-later */
/* What a word, an arithmetic expression and a condition evaluate to. Nothing
 * here starts a process: $(...) and a command used as a condition go back to
 * the caller through vsh_ctx_t. Pathname expansion is the executor's. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vsh.h"

static int ctx_fail(vsh_ctx_t *c, const char *msg, const char *what) {
    if (what) {
        snprintf(c->err, sizeof(c->err), "%s: %.80s", msg, what);
    } else {
        snprintf(c->err, sizeof(c->err), "%s", msg);
    }
    return -1;
}

static void list_push(vsh_ctx_t *c, vsh_list_t *l, const char *s) {
    if (l->count == l->cap) {
        size_t cap = l->cap ? l->cap * 2u : 8u;
        const char **items = (const char **)vsh_alloc(c->arena, cap * sizeof(*items));

        if (l->count) {
            memcpy(items, l->items, l->count * sizeof(*items));
        }
        l->items = items;
        l->cap = cap;
    }
    l->items[l->count++] = s;
}

/* In a pattern, what must not act as a pattern is escaped. */
static void put_text(vsh_sb_t *o, const char *t, int pattern, int literal) {
    if (!pattern) {
        vsh_sb_puts(o, t);
        return;
    }
    for (; *t; t++) {
        if (*t == '\\' || (literal && (*t == '*' || *t == '?' || *t == '['))) {
            vsh_sb_putc(o, '\\');
        }
        vsh_sb_putc(o, *t);
    }
}

static void put_value(vsh_sb_t *o, const vsh_value_t *v, long index, int pattern, int literal) {
    size_t i;

    if (index >= 0) {
        if ((size_t)index < v->count) {
            put_text(o, v->items[index], pattern, literal);
        }
        return;
    }
    for (i = 0; i < v->count; i++) {
        if (i) {
            vsh_sb_putc(o, ' ');
        }
        put_text(o, v->items[i], pattern, literal);
    }
}

static int append_word(vsh_ctx_t *c, const vsh_word_t *w, vsh_sb_t *o, int pattern) {
    const vsh_part_t *p;

    vsh_sb_putn(o, "", 0u);
    for (p = w->parts; p; p = p->next) {
        vsh_value_t v;
        char num[32];

        switch (p->kind) {
            case VSH_P_LIT:
                put_text(o, p->text, pattern, p->quoted);
                break;
            case VSH_P_VAR:
                if (c->lookup(c->user, p->text, &v) == 0) {
                    put_value(o, &v, p->index, pattern, p->quoted);
                }
                break;
            case VSH_P_PERCENT:
                if (c->lookup(c->user, p->text, &v) == 0) {
                    put_value(o, &v, -1, pattern, p->quoted);
                } else {
                    /* Not a variable: the bytes that were written, so that
                     * `date +%d%m%Y` still means what it says. */
                    vsh_sb_putc(o, '%');
                    put_text(o, p->text, pattern, 1);
                    vsh_sb_putc(o, '%');
                }
                break;
            case VSH_P_CMD: {
                char *text = 0;
                size_t n;

                if (!c->capture || c->capture(c->user, p->script, &text) != 0 || !text) {
                    return ctx_fail(c, "command substitution failed", 0);
                }
                n = strlen(text);
                while (n > 0u && text[n - 1u] == '\n') {
                    text[--n] = 0;
                }
                put_text(o, text, pattern, p->quoted);
                break;
            }
            case VSH_P_ARITH: {
                int64_t r;

                if (vsh_arith(c, p->text, &r) != 0) {
                    return -1;
                }
                snprintf(num, sizeof(num), "%lld", (long long)r);
                vsh_sb_puts(o, num);
                break;
            }
            case VSH_P_STATUS:
                snprintf(num, sizeof(num), "%d", c->status);
                vsh_sb_puts(o, num);
                break;
        }
    }
    return 0;
}

int vsh_word_text(vsh_ctx_t *c, const vsh_word_t *w, char **out) {
    vsh_sb_t o = {0, 0, 0};
    int r = append_word(c, w, &o, 0);

    if (r == 0) {
        *out = vsh_strndup(c->arena, o.b, o.n);
    }
    vsh_sb_free(&o);
    return r;
}

int vsh_expand_word(vsh_ctx_t *c, const vsh_word_t *w, vsh_list_t *out) {
    const vsh_part_t *p = w->parts;
    char *text;

    /* The one place a value becomes several words: a list, alone and
     * unquoted. Nothing is ever split on its spaces. */
    if (p && !p->next && p->kind == VSH_P_VAR && !p->quoted && p->index < 0) {
        vsh_value_t v;

        if (c->lookup(c->user, p->text, &v) == 0 && v.is_list) {
            size_t i;

            for (i = 0; i < v.count; i++) {
                list_push(c, out, v.items[i]);
            }
            return 0;
        }
    }
    if (vsh_word_text(c, w, &text) != 0) {
        return -1;
    }
    list_push(c, out, text);
    return 0;
}

/* ---- arithmetic ------------------------------------------------------------ */

typedef struct {
    vsh_ctx_t *c;
    const char *p;
    int depth;
} ar_t;

static int ar_expr(ar_t *a, int64_t *out);

static void ar_blank(ar_t *a) {
    while (*a->p == ' ' || *a->p == '\t' || *a->p == '\n' || *a->p == '\r') {
        a->p++;
    }
}

/* A whole decimal integer that fits, or nothing. */
static int parse_int(const char *t, int64_t *out) {
    int neg = 0;
    uint64_t v = 0, limit;

    if (*t == '+' || *t == '-') {
        neg = *t == '-';
        t++;
    }
    if (*t < '0' || *t > '9') {
        return -1;
    }
    limit = neg ? (uint64_t)INT64_MAX + 1u : (uint64_t)INT64_MAX;
    for (; *t; t++) {
        uint64_t d;

        if (*t < '0' || *t > '9') {
            return -1;
        }
        d = (uint64_t)(*t - '0');
        if (v > (limit - d) / 10u) {
            return -1;
        }
        v = v * 10u + d;
    }
    *out = neg ? (int64_t)(0u - v) : (int64_t)v;
    return 0;
}

static int ar_primary(ar_t *a, int64_t *out) {
    ar_blank(a);
    if (*a->p == '(') {
        a->p++;
        if (ar_expr(a, out) != 0) {
            return -1;
        }
        ar_blank(a);
        if (*a->p != ')') {
            return ctx_fail(a->c, "arithmetic: missing )", 0);
        }
        a->p++;
        return 0;
    }
    if (*a->p >= '0' && *a->p <= '9') {
        char num[32];
        size_t n = 0;

        while (*a->p >= '0' && *a->p <= '9') {
            if (n + 1u >= sizeof(num)) {
                return ctx_fail(a->c, "arithmetic: number too large", 0);
            }
            num[n++] = *a->p++;
        }
        num[n] = 0;
        if (parse_int(num, out) != 0) {
            return ctx_fail(a->c, "arithmetic: number too large", num);
        }
        return 0;
    }
    {
        char name[64];
        size_t n = 0;
        vsh_value_t v;

        if (*a->p == '$') {
            a->p++;
        }
        while ((*a->p >= 'a' && *a->p <= 'z') || (*a->p >= 'A' && *a->p <= 'Z') || *a->p == '_' ||
               (n > 0u && *a->p >= '0' && *a->p <= '9')) {
            if (n + 1u >= sizeof(name)) {
                return ctx_fail(a->c, "arithmetic: name too long", 0);
            }
            name[n++] = *a->p++;
        }
        name[n] = 0;
        if (n == 0u) {
            return ctx_fail(a->c, "arithmetic: a number expected", 0);
        }
        if (a->c->lookup(a->c->user, name, &v) != 0 || v.count != 1u ||
            parse_int(v.items[0], out) != 0) {
            return ctx_fail(a->c, "arithmetic: not a number", name);
        }
        return 0;
    }
}

static int ar_unary(ar_t *a, int64_t *out) {
    ar_blank(a);
    if (*a->p == '-' || *a->p == '+') {
        int neg = *a->p == '-';
        int r;

        a->p++;
        if (++a->depth > VSH_MAX_DEPTH) {
            return ctx_fail(a->c, "arithmetic: nested too deeply", 0);
        }
        r = ar_unary(a, out);
        a->depth--;
        if (r == 0 && neg) {
            *out = (int64_t)(0u - (uint64_t)*out);   /* wraps, never undefined */
        }
        return r;
    }
    return ar_primary(a, out);
}

static int ar_term(ar_t *a, int64_t *out) {
    if (ar_unary(a, out) != 0) {
        return -1;
    }
    for (;;) {
        char op;
        int64_t r;

        ar_blank(a);
        op = *a->p;
        if (op != '*' && op != '/' && op != '%') {
            return 0;
        }
        a->p++;
        if (ar_unary(a, &r) != 0) {
            return -1;
        }
        if (op == '*') {
            *out = (int64_t)((uint64_t)*out * (uint64_t)r);
        } else if (r == 0) {
            return ctx_fail(a->c, "arithmetic: division by zero", 0);
        } else if (r == -1) {
            /* INT64_MIN / -1 traps on x86. */
            *out = op == '/' ? (int64_t)(0u - (uint64_t)*out) : 0;
        } else {
            *out = op == '/' ? *out / r : *out % r;
        }
    }
}

static int ar_expr(ar_t *a, int64_t *out) {
    int r;

    if (++a->depth > VSH_MAX_DEPTH) {
        return ctx_fail(a->c, "arithmetic: nested too deeply", 0);
    }
    r = ar_term(a, out);
    while (r == 0) {
        char op;
        int64_t v;

        ar_blank(a);
        op = *a->p;
        if (op != '+' && op != '-') {
            break;
        }
        a->p++;
        r = ar_term(a, &v);
        if (r == 0) {
            *out = op == '+' ? (int64_t)((uint64_t)*out + (uint64_t)v)
                             : (int64_t)((uint64_t)*out - (uint64_t)v);
        }
    }
    a->depth--;
    return r;
}

int vsh_arith(vsh_ctx_t *c, const char *text, int64_t *out) {
    ar_t a;

    a.c = c;
    a.p = text;
    a.depth = 0;
    if (ar_expr(&a, out) != 0) {
        return -1;
    }
    ar_blank(&a);
    if (*a.p) {
        return ctx_fail(c, "arithmetic: unexpected text", a.p);
    }
    return 0;
}

/* ---- patterns -------------------------------------------------------------- */

static unsigned char fold(unsigned char ch, int ignore_case) {
    return ignore_case && ch >= 'A' && ch <= 'Z' ? (unsigned char)(ch - 'A' + 'a') : ch;
}

/* The class at p (just past '['): 1 matched, 0 did not, -1 not a class; *end is
 * past its ']'. */
static int match_class(const char *p, unsigned char ch, int ic, const char **end) {
    int neg = 0, hit = 0, first = 1;

    if (*p == '!' || *p == '^') {
        neg = 1;
        p++;
    }
    for (;; first = 0) {
        unsigned char lo, hi;

        if (!*p) {
            return -1;
        }
        if (*p == ']' && !first) {
            *end = p + 1;
            return hit != neg;
        }
        if (*p == '\\' && p[1]) {
            p++;
        }
        lo = (unsigned char)*p++;
        hi = lo;
        if (p[0] == '-' && p[1] && p[1] != ']') {
            p++;
            if (*p == '\\' && p[1]) {
                p++;
            }
            hi = (unsigned char)*p++;
        }
        if (ic) {
            unsigned char u = ch >= 'a' && ch <= 'z' ? (unsigned char)(ch - 'a' + 'A') : ch;
            unsigned char l = fold(ch, 1);

            hit |= (u >= lo && u <= hi) || (l >= lo && l <= hi);
        } else {
            hit |= ch >= lo && ch <= hi;
        }
    }
}

/* One backtrack point is enough for `*`, which keeps this linear per star:
 * a pattern is input, and the textbook recursion is exponential on a*a*a*b. */
int vsh_match(const char *pattern, const char *text, int ic) {
    const char *p = pattern, *t = text, *star = 0, *resume = 0;

    for (;;) {
        if (*p == '*') {
            while (*p == '*') {
                p++;
            }
            star = p;
            resume = t;
            continue;
        }
        if (!*t) {
            return *p == 0;
        }
        {
            const char *next = 0;
            int ok;

            if (*p == '?') {
                ok = 1;
                next = p + 1;
            } else if (*p == '[') {
                const char *end = 0;
                int m = match_class(p + 1, (unsigned char)*t, ic, &end);

                if (m < 0) {        /* no closing ]: a plain [ */
                    ok = *t == '[';
                    next = p + 1;
                } else {
                    ok = m;
                    next = end;
                }
            } else if (*p == '\\' && p[1]) {
                ok = fold((unsigned char)p[1], ic) == fold((unsigned char)*t, ic);
                next = p + 2;
            } else if (*p) {
                ok = fold((unsigned char)*p, ic) == fold((unsigned char)*t, ic);
                next = p + 1;
            } else {
                ok = 0;
            }
            if (ok) {
                p = next;
                t++;
                continue;
            }
        }
        if (!star) {
            return 0;
        }
        p = star;
        t = ++resume;
    }
}

/* ---- conditions ------------------------------------------------------------ */

static int compare(vsh_ctx_t *c, const vsh_node_t *n, int *out) {
    const vsh_word_t *lw = n->words, *rw = n->words->next;
    char *l, *r = 0;
    int64_t li, ri;
    int ord;

    if (vsh_word_text(c, lw, &l) != 0) {
        return -1;
    }
    if (n->cmp == VSH_C_MATCH || n->cmp == VSH_C_IMATCH) {
        vsh_sb_t pat = {0, 0, 0};
        int e = append_word(c, rw, &pat, 1);

        if (e == 0) {
            *out = vsh_match(pat.b, l, n->cmp == VSH_C_IMATCH);
        }
        vsh_sb_free(&pat);
        return e;
    }
    if (vsh_word_text(c, rw, &r) != 0) {
        return -1;
    }
    /* Numbers if both are, so that 10 > 9; otherwise the text. */
    if (parse_int(l, &li) == 0 && parse_int(r, &ri) == 0) {
        ord = li < ri ? -1 : li > ri ? 1 : 0;
    } else {
        ord = strcmp(l, r);
    }
    switch (n->cmp) {
        case VSH_C_EQ: *out = ord == 0; break;
        case VSH_C_NE: *out = ord != 0; break;
        case VSH_C_LT: *out = ord < 0; break;
        case VSH_C_LE: *out = ord <= 0; break;
        case VSH_C_GT: *out = ord > 0; break;
        default:       *out = ord >= 0; break;
    }
    return 0;
}

int vsh_cond(vsh_ctx_t *c, const vsh_node_t *n, int *out) {
    switch (n->kind) {
        case VSH_N_AND:
        case VSH_N_OR:
            if (vsh_cond(c, n->a, out) != 0) {
                return -1;
            }
            /* The right side is not evaluated if the left decided: it may be
             * a command. */
            if ((n->kind == VSH_N_AND) == (*out != 0)) {
                return vsh_cond(c, n->b, out);
            }
            return 0;
        case VSH_N_NOT:
            if (vsh_cond(c, n->a, out) != 0) {
                return -1;
            }
            *out = !*out;
            return 0;
        case VSH_N_CMP:
            return compare(c, n, out);
        case VSH_N_TRUTH: {
            char *t;

            if (vsh_word_text(c, n->words, &t) != 0) {
                return -1;
            }
            *out = t[0] != 0;
            return 0;
        }
        case VSH_N_CONDCMD: {
            int status = 1;

            if (!c->run || c->run(c->user, n->a, &status) != 0) {
                return ctx_fail(c, "the condition's command could not run", 0);
            }
            *out = status == 0;
            return 0;
        }
        default:
            return ctx_fail(c, "not a condition", 0);
    }
}
