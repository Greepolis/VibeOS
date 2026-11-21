/* SPDX-License-Identifier: GPL-3.0-or-later */
/* vsh's parser: recursive descent straight over the characters.
 *
 * Not a token stream, because what a character means depends on where it is:
 * `<` compares after `if` and redirects in a command, `=` is the assignment in
 * `set x = 1` and a plain letter in `grep a=b`. The grammar is in
 * docs/shell/s4.md. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vsh.h"

/* ---- memory ---------------------------------------------------------------- */

struct vsh_chunk {
    vsh_chunk_t *next;
    size_t used, cap;
};

#define VSH_CHUNK 16384u
#define VSH_ALIGN 16u
#define VSH_HEADER ((sizeof(vsh_chunk_t) + VSH_ALIGN - 1u) & ~(size_t)(VSH_ALIGN - 1u))

static void vsh_oom(void) {
    fputs("vsh: out of memory\n", stderr);
    exit(2);
}

void vsh_arena_init(vsh_arena_t *a) {
    a->head = 0;
}

void vsh_arena_free(vsh_arena_t *a) {
    while (a->head) {
        vsh_chunk_t *c = a->head;

        a->head = c->next;
        free(c);
    }
}

void *vsh_alloc(vsh_arena_t *a, size_t n) {
    vsh_chunk_t *c = a->head;
    char *p;

    if (n > (size_t)-1 / 2u) {
        vsh_oom();
    }
    n = (n + VSH_ALIGN - 1u) & ~(size_t)(VSH_ALIGN - 1u);
    if (!c || c->cap - c->used < n) {
        size_t cap = n > VSH_CHUNK ? n : VSH_CHUNK;

        c = (vsh_chunk_t *)malloc(VSH_HEADER + cap);
        if (!c) {
            vsh_oom();
        }
        c->next = a->head;
        c->used = 0;
        c->cap = cap;
        a->head = c;
    }
    p = (char *)c + VSH_HEADER + c->used;
    c->used += n;
    memset(p, 0, n);
    return p;
}

char *vsh_strndup(vsh_arena_t *a, const char *s, size_t n) {
    char *d = (char *)vsh_alloc(a, n + 1u);

    if (n) {
        memcpy(d, s, n);
    }
    d[n] = 0;
    return d;
}

void vsh_sb_putn(vsh_sb_t *s, const char *t, size_t n) {
    if (s->cap - s->n < n + 1u) {
        size_t cap = s->cap ? s->cap : 64u;
        char *b;

        while (cap - s->n < n + 1u) {
            cap *= 2u;
        }
        b = (char *)realloc(s->b, cap);
        if (!b) {
            vsh_oom();
        }
        s->b = b;
        s->cap = cap;
    }
    if (n) {
        memcpy(s->b + s->n, t, n);
    }
    s->n += n;
    s->b[s->n] = 0;
}

void vsh_sb_puts(vsh_sb_t *s, const char *t) {
    vsh_sb_putn(s, t, strlen(t));
}

void vsh_sb_putc(vsh_sb_t *s, char c) {
    vsh_sb_putn(s, &c, 1u);
}

void vsh_sb_free(vsh_sb_t *s) {
    free(s->b);
    s->b = 0;
    s->n = s->cap = 0;
}

/* ---- characters ------------------------------------------------------------ */

typedef struct {
    vsh_arena_t *a;
    const char *src, *p;
    char *err;
    size_t errcap;
    size_t errpos;
    int failed;
    int depth;
} ps_t;

static void fail(ps_t *s, const char *msg) {
    if (!s->failed) {
        s->failed = 1;
        s->errpos = (size_t)(s->p - s->src);
        snprintf(s->err, s->errcap, "%s", msg);
    }
}

static int is_blank(char c) {
    /* \r so that a script saved on Windows reads the same. */
    return c == ' ' || c == '\t' || c == '\r';
}

static int is_name_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static int is_name(char c) {
    return is_name_start(c) || (c >= '0' && c <= '9');
}

static int is_digit(char c) {
    return c >= '0' && c <= '9';
}

static char lower(char c) {
    return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

static int is_delim(char c) {
    return !c || is_blank(c) || c == '\n' || c == ';';
}

/* A brace is structure only standing alone, so that `find -exec {} ;`,
 * `a{b}` and an argument that merely contains one stay words. */
static int at_open(const ps_t *s) {
    return s->p[0] == '{' &&
           (is_delim(s->p[1]) || (s->p[1] == '}' && (is_delim(s->p[2]) || s->p[2] == ')')));
}

static int at_close(const ps_t *s) {
    return s->p[0] == '}' && (is_delim(s->p[1]) || s->p[1] == ')');
}

static void skip_blank(ps_t *s) {
    for (;;) {
        if (is_blank(*s->p)) {
            s->p++;
        } else if (s->p[0] == '\\' && s->p[1] == '\n') {
            s->p += 2;
        } else if (*s->p == '#') {
            while (*s->p && *s->p != '\n') {
                s->p++;
            }
        } else {
            return;
        }
    }
}

static void skip_lines(ps_t *s) {
    for (;;) {
        skip_blank(s);
        if (*s->p != '\n') {
            return;
        }
        s->p++;
    }
}

/* Keywords ignore case, as builtins do. */
static int at_keyword(const ps_t *s, const char *kw) {
    size_t i;

    for (i = 0; kw[i]; i++) {
        if (lower(s->p[i]) != kw[i]) {
            return 0;
        }
    }
    return is_delim(s->p[i]) || s->p[i] == ')';
}

static int take_keyword(ps_t *s, const char *kw) {
    if (!at_keyword(s, kw)) {
        return 0;
    }
    s->p += strlen(kw);
    return 1;
}

static const char *take_name(ps_t *s) {
    const char *b = s->p;

    if (!is_name_start(*s->p)) {
        return 0;
    }
    while (is_name(*s->p)) {
        s->p++;
    }
    return vsh_strndup(s->a, b, (size_t)(s->p - b));
}

static vsh_node_t *node(ps_t *s, vsh_node_kind_t k) {
    vsh_node_t *n = (vsh_node_t *)vsh_alloc(s->a, sizeof(*n));

    n->kind = k;
    return n;
}

/* ---- words ----------------------------------------------------------------- */

static vsh_node_t *parse_script(ps_t *s, char closer);

static vsh_part_t *add_part(ps_t *s, vsh_word_t *w, vsh_part_kind_t k, int quoted) {
    vsh_part_t *p = (vsh_part_t *)vsh_alloc(s->a, sizeof(*p));

    p->kind = k;
    p->quoted = quoted;
    p->index = -1;
    if (w->tail) {
        w->tail->next = p;
    } else {
        w->parts = p;
    }
    w->tail = p;
    return p;
}

static void flush(ps_t *s, vsh_word_t *w, vsh_sb_t *lit, int quoted) {
    if (lit->n) {
        add_part(s, w, VSH_P_LIT, quoted)->text = vsh_strndup(s->a, lit->b, lit->n);
        lit->n = 0;
        lit->b[0] = 0;
    }
}

static int starts_dollar(const char *p) {
    return p[0] == '$' && (p[1] == '?' || p[1] == '(' || p[1] == '{' || is_name_start(p[1]));
}

static void parse_dollar(ps_t *s, vsh_word_t *w, int quoted) {
    vsh_part_t *part;

    s->p++;
    if (*s->p == '?') {
        s->p++;
        add_part(s, w, VSH_P_STATUS, quoted);
    } else if (s->p[0] == '(' && s->p[1] == '(') {
        const char *b = s->p + 2;
        int open = 2;

        s->p += 2;
        while (*s->p && open > 0) {
            open += *s->p == '(' ? 1 : *s->p == ')' ? -1 : 0;
            s->p++;
        }
        if (open != 0 || s->p[-1] != ')' || s->p[-2] != ')') {
            fail(s, "missing )) after $((");
            return;
        }
        add_part(s, w, VSH_P_ARITH, quoted)->text = vsh_strndup(s->a, b, (size_t)(s->p - 2 - b));
    } else if (*s->p == '(') {
        vsh_node_t *body;

        s->p++;
        body = parse_script(s, ')');
        if (s->failed) {
            return;
        }
        s->p++;      /* parse_script stopped on it */
        add_part(s, w, VSH_P_CMD, quoted)->script = body;
    } else if (*s->p == '{') {
        const char *name;
        long index = -1;

        s->p++;
        name = take_name(s);
        if (!name) {
            fail(s, "a variable name expected after ${");
            return;
        }
        if (*s->p == '[') {
            s->p++;
            if (!is_digit(*s->p)) {
                fail(s, "an index expected after [");
                return;
            }
            index = 0;
            while (is_digit(*s->p)) {
                if (index > 100000000L) {
                    fail(s, "index too large");
                    return;
                }
                index = index * 10 + (*s->p++ - '0');
            }
            if (*s->p != ']') {
                fail(s, "missing ]");
                return;
            }
            s->p++;
        }
        if (*s->p != '}') {
            fail(s, "missing } after ${");
            return;
        }
        s->p++;
        part = add_part(s, w, VSH_P_VAR, quoted);
        part->text = name;
        part->index = index;
    } else {
        add_part(s, w, VSH_P_VAR, quoted)->text = take_name(s);
    }
}

/* %NAME%, or nothing: `date +%d` has no closing percent to be one. */
static size_t percent_len(const char *p) {
    size_t n = 1;

    if (p[0] != '%' || !is_name_start(p[1])) {
        return 0;
    }
    while (is_name(p[n])) {
        n++;
    }
    return p[n] == '%' ? n + 1u : 0u;
}

static void parse_percent(ps_t *s, vsh_word_t *w, size_t len, int quoted) {
    add_part(s, w, VSH_P_PERCENT, quoted)->text = vsh_strndup(s->a, s->p + 1, len - 2u);
    s->p += len;
}

static void parse_dq(ps_t *s, vsh_word_t *w, vsh_sb_t *lit) {
    const vsh_part_t *before = w->tail;

    s->p++;
    for (;;) {
        char c = *s->p;
        size_t pct;

        if (!c) {
            fail(s, "missing closing \"");
            return;
        }
        if (c == '"') {
            s->p++;
            break;
        }
        if (c == '\\') {
            char n = s->p[1];

            if (n == '\n') {
                s->p += 2;
            } else if (n == '"' || n == '$' || n == '\\') {
                vsh_sb_putc(lit, n);
                s->p += 2;
            } else {
                vsh_sb_putc(lit, c);
                s->p++;
            }
        } else if (starts_dollar(s->p)) {
            flush(s, w, lit, 1);
            parse_dollar(s, w, 1);
            if (s->failed) {
                return;
            }
        } else if ((pct = percent_len(s->p)) != 0u) {
            flush(s, w, lit, 1);
            parse_percent(s, w, pct, 1);
        } else {
            vsh_sb_putc(lit, c);
            s->p++;
        }
    }
    flush(s, w, lit, 1);
    if (w->tail == before) {
        add_part(s, w, VSH_P_LIT, 1)->text = "";   /* "" is a word */
    }
}

static int word_ends(const ps_t *s, const vsh_word_t *w, const vsh_sb_t *lit, int in_cond) {
    char c = *s->p;

    if (!c || is_blank(c) || c == '\n' || c == ';' || c == '|' || c == '&' || c == '(' ||
        c == ')' || c == '<' || c == '>') {
        return 1;
    }
    if (in_cond) {
        if ((c == '=' || c == '!') && s->p[1] == '=') {
            return 1;
        }
        /* At the start of a word it is a home directory. */
        if (c == '~' && (w->parts || lit->n)) {
            return 1;
        }
    }
    return 0;
}

static vsh_word_t *parse_word(ps_t *s, int in_cond) {
    vsh_word_t *w = (vsh_word_t *)vsh_alloc(s->a, sizeof(*w));
    vsh_sb_t lit = {0, 0, 0};

    vsh_sb_putn(&lit, "", 0u);
    while (!s->failed && !word_ends(s, w, &lit, in_cond)) {
        char c = *s->p;
        size_t pct;

        if (c == '\'') {
            const char *b = ++s->p;

            flush(s, w, &lit, 0);
            while (*s->p && *s->p != '\'') {
                s->p++;
            }
            if (!*s->p) {
                fail(s, "missing closing '");
                break;
            }
            add_part(s, w, VSH_P_LIT, 1)->text = vsh_strndup(s->a, b, (size_t)(s->p - b));
            s->p++;
        } else if (c == '"') {
            flush(s, w, &lit, 0);
            parse_dq(s, w, &lit);
        } else if (c == '\\') {
            /* Only before these, so that C:\Users\vibe needs no doubling. */
            char n = s->p[1];

            if (n == '\n') {
                s->p += 2;
            } else if (n == ' ' || n == '\'' || n == '"' || n == '$' || n == '\\') {
                vsh_sb_putc(&lit, n);
                s->p += 2;
            } else {
                vsh_sb_putc(&lit, c);
                s->p++;
            }
        } else if (starts_dollar(s->p)) {
            flush(s, w, &lit, 0);
            parse_dollar(s, w, 0);
        } else if ((pct = percent_len(s->p)) != 0u) {
            flush(s, w, &lit, 0);
            parse_percent(s, w, pct, 0);
        } else {
            vsh_sb_putc(&lit, c);
            s->p++;
        }
    }
    if (!s->failed) {
        flush(s, w, &lit, 0);
    }
    vsh_sb_free(&lit);
    return !s->failed && w->parts ? w : 0;
}

static vsh_word_t *need_word(ps_t *s, int in_cond, const char *msg) {
    vsh_word_t *w;

    skip_blank(s);
    w = parse_word(s, in_cond);
    if (!w) {
        fail(s, msg);
    }
    return w;
}

/* ---- commands -------------------------------------------------------------- */

static int at_assignment(const ps_t *s) {
    const char *p = s->p;

    if (!is_name_start(*p)) {
        return 0;
    }
    while (is_name(*p)) {
        p++;
    }
    return p[0] == '=' && p[1] != '=';
}

static int at_redirect(const ps_t *s) {
    const char *p = s->p;

    if (is_digit(*p)) {
        while (is_digit(*p)) {
            p++;
        }
    }
    return *p == '>' || *p == '<';
}

static vsh_redir_t *parse_redirect(ps_t *s) {
    vsh_redir_t *r = (vsh_redir_t *)vsh_alloc(s->a, sizeof(*r));
    int fd = -1;

    if (is_digit(*s->p)) {
        fd = 0;
        while (is_digit(*s->p)) {
            if (fd > 1000) {
                fail(s, "descriptor too large");
                return r;
            }
            fd = fd * 10 + (*s->p++ - '0');
        }
    }
    if (*s->p == '<') {
        s->p++;
        r->kind = VSH_R_IN;
        r->fd = fd < 0 ? 0 : fd;
    } else if (s->p[1] == '&') {
        s->p += 2;
        r->kind = VSH_R_DUP;
        r->fd = fd < 0 ? 1 : fd;
        if (!is_digit(*s->p)) {
            fail(s, "a descriptor expected after >&");
            return r;
        }
        while (is_digit(*s->p)) {
            if (r->dup_to > 1000) {
                fail(s, "descriptor too large");
                return r;
            }
            r->dup_to = r->dup_to * 10 + (*s->p++ - '0');
        }
        return r;
    } else if (s->p[1] == '>') {
        s->p += 2;
        r->kind = VSH_R_APPEND;
        r->fd = fd < 0 ? 1 : fd;
    } else {
        s->p++;
        r->kind = VSH_R_OUT;
        r->fd = fd < 0 ? 1 : fd;
    }
    r->target = need_word(s, 0, "a file name expected after the redirection");
    return r;
}

static vsh_node_t *parse_command(ps_t *s) {
    vsh_node_t *n = node(s, VSH_N_COMMAND);
    vsh_assign_t *alast = 0;
    vsh_word_t *wlast = 0;
    vsh_redir_t *rlast = 0;

    skip_blank(s);
    while (at_assignment(s)) {
        vsh_assign_t *as = (vsh_assign_t *)vsh_alloc(s->a, sizeof(*as));

        as->name = take_name(s);
        s->p++;
        as->value = parse_word(s, 0);
        if (s->failed) {
            return 0;
        }
        if (!as->value) {     /* X= cmd: empty, and still an assignment */
            as->value = (vsh_word_t *)vsh_alloc(s->a, sizeof(*as->value));
            add_part(s, as->value, VSH_P_LIT, 1)->text = "";
        }
        if (alast) {
            alast->next = as;
        } else {
            n->assigns = as;
        }
        alast = as;
        skip_blank(s);
    }
    for (;;) {
        skip_blank(s);
        if (at_close(s)) {
            break;
        }
        if (at_redirect(s)) {
            vsh_redir_t *r = parse_redirect(s);

            if (s->failed) {
                return 0;
            }
            if (rlast) {
                rlast->next = r;
            } else {
                n->redirs = r;
            }
            rlast = r;
        } else {
            vsh_word_t *w = parse_word(s, 0);

            if (s->failed) {
                return 0;
            }
            if (!w) {
                break;
            }
            if (wlast) {
                wlast->next = w;
            } else {
                n->words = w;
            }
            wlast = w;
        }
    }
    if (!n->words) {
        fail(s, n->assigns ? "a bare assignment: write  set x = 1" : "a command expected");
        return 0;
    }
    return n;
}

static vsh_node_t *parse_pipeline(ps_t *s) {
    vsh_node_t *n = node(s, VSH_N_PIPELINE);
    vsh_node_t *last = 0;

    skip_blank(s);
    n->flag = take_keyword(s, "try");
    for (;;) {
        vsh_node_t *c = parse_command(s);

        if (!c) {
            return 0;
        }
        if (last) {
            last->next = c;
        } else {
            n->a = c;
        }
        last = c;
        skip_blank(s);
        if (s->p[0] != '|' || s->p[1] == '|') {
            return n;
        }
        s->p++;
        skip_lines(s);
    }
}

static vsh_node_t *parse_chain(ps_t *s) {
    vsh_node_t *left = parse_pipeline(s);
    int links = 0;

    while (left) {
        vsh_node_t *n;
        vsh_node_kind_t k;

        skip_blank(s);
        if (s->p[0] == '&' && s->p[1] == '&') {
            k = VSH_N_AND;
        } else if (s->p[0] == '|' && s->p[1] == '|') {
            k = VSH_N_OR;
        } else {
            break;
        }
        if (++links > VSH_MAX_CHAIN) {
            fail(s, "chain too long");
            return 0;
        }
        s->p += 2;
        skip_lines(s);
        n = node(s, k);
        n->a = left;
        n->b = parse_pipeline(s);
        if (!n->b) {
            return 0;
        }
        left = n;
    }
    return left;
}

/* ---- conditions ------------------------------------------------------------ */

static int take_cmp(ps_t *s, vsh_cmp_t *out) {
    static const struct {
        const char *t;
        vsh_cmp_t c;
    } ops[] = {
        /* The longer spelling of a prefix first. */
        {"==", VSH_C_EQ}, {"!=", VSH_C_NE}, {"<=", VSH_C_LE}, {">=", VSH_C_GE},
        {"~~", VSH_C_IMATCH}, {"<", VSH_C_LT}, {">", VSH_C_GT}, {"~", VSH_C_MATCH},
    };
    size_t i;

    for (i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) {
        size_t n = strlen(ops[i].t);

        if (strncmp(s->p, ops[i].t, n) == 0) {
            s->p += n;
            *out = ops[i].c;
            return 1;
        }
    }
    return 0;
}

static vsh_node_t *parse_atom(ps_t *s) {
    vsh_node_t *n;
    vsh_word_t *left;
    vsh_cmp_t cmp;

    skip_blank(s);
    if (*s->p == '(') {
        s->p++;
        n = node(s, VSH_N_CONDCMD);
        n->a = parse_chain(s);
        if (!n->a) {
            return 0;
        }
        skip_lines(s);
        if (*s->p != ')') {
            fail(s, "missing ) after the command");
            return 0;
        }
        s->p++;
        return n;
    }
    if (at_open(s)) {
        fail(s, "a condition expected before {");
        return 0;
    }
    left = need_word(s, 1, "a condition expected");
    if (!left) {
        return 0;
    }
    skip_blank(s);
    if (!take_cmp(s, &cmp)) {
        n = node(s, VSH_N_TRUTH);
        n->words = left;
        return n;
    }
    n = node(s, VSH_N_CMP);
    n->cmp = cmp;
    n->words = left;
    left->next = need_word(s, 1, "a value expected after the comparison");
    return left->next ? n : 0;
}

static vsh_node_t *parse_not(ps_t *s) {
    skip_blank(s);
    if (s->p[0] == '!' && s->p[1] != '=') {
        vsh_node_t *n = node(s, VSH_N_NOT);

        s->p++;
        n->a = parse_atom(s);
        return n->a ? n : 0;
    }
    return parse_atom(s);
}

static vsh_node_t *parse_cond_level(ps_t *s, int is_or) {
    vsh_node_t *left = is_or ? parse_cond_level(s, 0) : parse_not(s);
    const char *op = is_or ? "||" : "&&";
    int links = 0;

    while (left) {
        vsh_node_t *n;

        skip_blank(s);
        if (strncmp(s->p, op, 2) != 0) {
            break;
        }
        if (++links > VSH_MAX_CHAIN) {
            fail(s, "condition too long");
            return 0;
        }
        s->p += 2;
        skip_lines(s);
        n = node(s, is_or ? VSH_N_OR : VSH_N_AND);
        n->a = left;
        n->b = is_or ? parse_cond_level(s, 0) : parse_not(s);
        if (!n->b) {
            return 0;
        }
        left = n;
    }
    return left;
}

/* ---- statements ------------------------------------------------------------ */

static vsh_node_t *parse_block(ps_t *s) {
    vsh_node_t *body;

    skip_blank(s);
    if (!at_open(s)) {
        fail(s, "{ expected");
        return 0;
    }
    s->p++;
    body = parse_script(s, '}');
    if (!s->failed) {
        s->p++;
    }
    return body;
}

static void parse_value(ps_t *s, vsh_node_t *n) {
    vsh_word_t *last = 0;

    skip_blank(s);
    if (*s->p != '(') {
        n->words = need_word(s, 0, "a value expected after =");
        return;
    }
    s->p++;
    n->is_list = 1;
    for (;;) {
        vsh_word_t *w;

        skip_lines(s);
        if (*s->p == ')') {
            s->p++;
            return;
        }
        w = parse_word(s, 0);
        if (!w) {
            fail(s, "missing ) after the list");
            return;
        }
        if (last) {
            last->next = w;
        } else {
            n->words = w;
        }
        last = w;
    }
}

static vsh_node_t *parse_assignment(ps_t *s, vsh_node_kind_t k) {
    vsh_node_t *n = node(s, k);

    skip_blank(s);
    n->name = take_name(s);
    if (!n->name) {
        fail(s, "a variable name expected");
        return 0;
    }
    skip_blank(s);
    if (*s->p != '=') {
        if (k == VSH_N_EXPORT) {
            return n;
        }
        fail(s, "= expected");
        return 0;
    }
    s->p++;
    n->flag = 1;
    parse_value(s, n);
    return s->failed ? 0 : n;
}

static vsh_node_t *parse_set(ps_t *s) {
    skip_blank(s);
    if (s->p[0] == '-' && s->p[1] == 'e' && is_delim(s->p[2])) {
        vsh_node_t *n = node(s, VSH_N_ERRORS);

        s->p += 2;
        skip_blank(s);
        if (take_keyword(s, "on")) {
            n->flag = 1;
        } else if (!take_keyword(s, "off")) {
            fail(s, "set -e takes on or off");
            return 0;
        }
        return n;
    }
    return parse_assignment(s, VSH_N_SET);
}

static vsh_node_t *parse_if(ps_t *s) {
    vsh_node_t *first = 0, *last = 0;
    int arms = 0;

    /* A loop and a bound: `else if` a hundred thousand times is a line of text,
     * and every evaluator walks the same chain. */
    for (;;) {
        vsh_node_t *n = node(s, VSH_N_IF);
        const char *mark;

        if (++arms > VSH_MAX_CHAIN) {
            fail(s, "too many else if");
            return 0;
        }
        n->a = parse_cond_level(s, 1);
        if (!n->a) {
            return 0;
        }
        n->b = parse_block(s);
        if (s->failed) {
            return 0;
        }
        if (last) {
            last->otherwise = n;
        } else {
            first = n;
        }
        last = n;

        /* else may start the next line; anything else there is not ours. */
        mark = s->p;
        skip_lines(s);
        if (!take_keyword(s, "else")) {
            s->p = mark;
            return first;
        }
        skip_blank(s);
        if (take_keyword(s, "if")) {
            continue;
        }
        last->otherwise = parse_block(s);
        return s->failed ? 0 : first;
    }
}

static vsh_node_t *parse_for(ps_t *s) {
    vsh_node_t *n = node(s, VSH_N_FOR);
    vsh_word_t *last = 0;

    skip_blank(s);
    n->name = take_name(s);
    skip_blank(s);
    if (!n->name || !take_keyword(s, "in")) {
        fail(s, "for takes: for name in values { ... }");
        return 0;
    }
    for (;;) {
        vsh_word_t *w;

        skip_blank(s);
        if (at_open(s)) {
            break;
        }
        w = parse_word(s, 0);
        if (!w) {
            fail(s, "{ expected");
            return 0;
        }
        if (last) {
            last->next = w;
        } else {
            n->words = w;
        }
        last = w;
    }
    n->b = parse_block(s);
    return s->failed ? 0 : n;
}

static vsh_node_t *parse_func(ps_t *s) {
    vsh_node_t *n = node(s, VSH_N_FUNC);
    vsh_word_t *last = 0;

    skip_blank(s);
    n->name = take_name(s);
    if (!n->name) {
        fail(s, "a function name expected");
        return 0;
    }
    for (;;) {
        vsh_word_t *w;
        const char *param;

        skip_blank(s);
        if (at_open(s)) {
            break;
        }
        param = take_name(s);
        if (!param) {
            fail(s, "a parameter name expected");
            return 0;
        }
        w = (vsh_word_t *)vsh_alloc(s->a, sizeof(*w));
        add_part(s, w, VSH_P_LIT, 0)->text = param;
        if (last) {
            last->next = w;
        } else {
            n->words = w;
        }
        last = w;
    }
    n->b = parse_block(s);
    return s->failed ? 0 : n;
}

static vsh_node_t *parse_statement(ps_t *s) {
    vsh_node_t *n;

    if (take_keyword(s, "set")) {
        return parse_set(s);
    }
    if (take_keyword(s, "global")) {
        return parse_assignment(s, VSH_N_GLOBAL);
    }
    if (take_keyword(s, "export")) {
        return parse_assignment(s, VSH_N_EXPORT);
    }
    if (take_keyword(s, "if")) {
        return parse_if(s);
    }
    if (take_keyword(s, "for")) {
        return parse_for(s);
    }
    if (take_keyword(s, "while")) {
        n = node(s, VSH_N_WHILE);
        n->a = parse_cond_level(s, 1);
        if (!n->a) {
            return 0;
        }
        n->b = parse_block(s);
        return s->failed ? 0 : n;
    }
    if (take_keyword(s, "func")) {
        return parse_func(s);
    }
    if (take_keyword(s, "return")) {
        n = node(s, VSH_N_RETURN);
        skip_blank(s);
        if (!is_delim(*s->p) && !at_close(s) && *s->p != ')') {
            n->words = need_word(s, 0, "a value expected after return");
        }
        return s->failed ? 0 : n;
    }
    if (take_keyword(s, "break")) {
        return node(s, VSH_N_BREAK);
    }
    if (take_keyword(s, "continue")) {
        return node(s, VSH_N_CONTINUE);
    }
    if (at_keyword(s, "else")) {
        fail(s, "else without if");
        return 0;
    }
    n = parse_chain(s);
    if (!n) {
        return 0;
    }
    skip_blank(s);
    if (s->p[0] == '&' && s->p[1] != '&') {
        vsh_node_t *bg = node(s, VSH_N_BACKGROUND);

        s->p++;
        bg->a = n;
        return bg;
    }
    return n;
}

/* Stops on the closer without taking it. */
static vsh_node_t *parse_script(ps_t *s, char closer) {
    vsh_node_t *first = 0, *last = 0;

    if (++s->depth > VSH_MAX_DEPTH) {
        fail(s, "nested too deeply");
        s->depth--;
        return 0;
    }
    for (;;) {
        vsh_node_t *n;

        for (;;) {
            skip_lines(s);
            if (*s->p != ';') {
                break;
            }
            s->p++;
        }
        if (!*s->p) {
            if (closer) {
                fail(s, closer == '}' ? "missing }" : "missing )");
            }
            break;
        }
        if ((closer == '}' && at_close(s)) || (closer == ')' && *s->p == ')')) {
            break;
        }
        if (at_close(s)) {
            fail(s, "} without {");
            break;
        }
        n = parse_statement(s);
        if (s->failed) {
            break;
        }
        if (!n) {
            fail(s, "a statement expected");
            break;
        }
        if (last) {
            last->next = n;
        } else {
            first = n;
        }
        last = n;
        skip_blank(s);
        if (*s->p && *s->p != '\n' && *s->p != ';' && !(closer == '}' && at_close(s)) &&
            !(closer == ')' && *s->p == ')')) {
            fail(s, "unexpected text after the statement");
            break;
        }
    }
    s->depth--;
    return s->failed ? 0 : first;
}

vsh_node_t *vsh_parse(vsh_arena_t *a, const char *src, char *err, size_t errcap, size_t *errpos) {
    ps_t s;
    vsh_node_t *script;

    memset(&s, 0, sizeof(s));
    s.a = a;
    s.src = s.p = src;
    s.err = err;
    s.errcap = errcap;
    if (errcap) {
        err[0] = 0;
    }
    script = parse_script(&s, 0);
    if (errpos) {
        *errpos = s.errpos;
    }
    return s.failed ? 0 : script;
}

/* ---- the tree as text ------------------------------------------------------ */

static void dump_list(const vsh_node_t *n, vsh_sb_t *o);
static void dump_node(const vsh_node_t *n, vsh_sb_t *o);

static void dump_word(const vsh_word_t *w, vsh_sb_t *o) {
    const vsh_part_t *p;
    char num[32];

    for (p = w->parts; p; p = p->next) {
        const char *q = p->quoted ? "\"" : "";

        switch (p->kind) {
            case VSH_P_LIT:
                vsh_sb_puts(o, p->quoted ? "'" : "");
                vsh_sb_puts(o, p->text);
                vsh_sb_puts(o, p->quoted ? "'" : "");
                break;
            case VSH_P_VAR:
                vsh_sb_puts(o, q);
                vsh_sb_putc(o, '$');
                vsh_sb_puts(o, p->text);
                if (p->index >= 0) {
                    snprintf(num, sizeof(num), "[%ld]", p->index);
                    vsh_sb_puts(o, num);
                }
                vsh_sb_puts(o, q);
                break;
            case VSH_P_PERCENT:
                vsh_sb_puts(o, q);
                vsh_sb_putc(o, '%');
                vsh_sb_puts(o, p->text);
                vsh_sb_putc(o, '%');
                vsh_sb_puts(o, q);
                break;
            case VSH_P_CMD:
                vsh_sb_puts(o, q);
                vsh_sb_puts(o, "$(");
                dump_list(p->script, o);
                vsh_sb_putc(o, ')');
                vsh_sb_puts(o, q);
                break;
            case VSH_P_ARITH:
                vsh_sb_puts(o, q);
                vsh_sb_puts(o, "$((");
                vsh_sb_puts(o, p->text);
                vsh_sb_puts(o, "))");
                vsh_sb_puts(o, q);
                break;
            case VSH_P_STATUS:
                vsh_sb_puts(o, q);
                vsh_sb_puts(o, "$?");
                vsh_sb_puts(o, q);
                break;
        }
    }
}

static void dump_words(const vsh_word_t *w, vsh_sb_t *o) {
    for (; w; w = w->next) {
        dump_word(w, o);
        if (w->next) {
            vsh_sb_putc(o, ' ');
        }
    }
}

static void dump_block(const vsh_node_t *body, vsh_sb_t *o) {
    vsh_sb_puts(o, "{ ");
    dump_list(body, o);
    vsh_sb_puts(o, " }");
}

static void dump_command(const vsh_node_t *n, vsh_sb_t *o) {
    const vsh_assign_t *as;
    const vsh_redir_t *r;
    char num[32];

    vsh_sb_puts(o, "cmd[");
    for (as = n->assigns; as; as = as->next) {
        vsh_sb_puts(o, as->name);
        vsh_sb_putc(o, '=');
        dump_word(as->value, o);
        vsh_sb_putc(o, ' ');
    }
    dump_words(n->words, o);
    for (r = n->redirs; r; r = r->next) {
        static const char *const ops[] = {">", ">>", "<", ">&"};

        snprintf(num, sizeof(num), " %d%s", r->fd, ops[r->kind]);
        vsh_sb_puts(o, num);
        if (r->kind == VSH_R_DUP) {
            snprintf(num, sizeof(num), "%d", r->dup_to);
            vsh_sb_puts(o, num);
        } else {
            dump_word(r->target, o);
        }
    }
    vsh_sb_putc(o, ']');
}

static void dump_node(const vsh_node_t *n, vsh_sb_t *o) {
    static const char *const cmps[] = {"==", "!=", "<", "<=", ">", ">=", "~", "~~"};
    const vsh_node_t *c;

    switch (n->kind) {
        case VSH_N_SET:
        case VSH_N_GLOBAL:
        case VSH_N_EXPORT:
            vsh_sb_puts(o, n->kind == VSH_N_SET ? "set " : n->kind == VSH_N_GLOBAL ? "global " : "export ");
            vsh_sb_puts(o, n->name);
            if (n->flag) {
                vsh_sb_puts(o, " = ");
                vsh_sb_puts(o, n->is_list ? "(" : "");
                dump_words(n->words, o);
                vsh_sb_puts(o, n->is_list ? ")" : "");
            }
            break;
        case VSH_N_ERRORS:
            vsh_sb_puts(o, n->flag ? "errors on" : "errors off");
            break;
        case VSH_N_IF:
            vsh_sb_puts(o, "if ");
            dump_node(n->a, o);
            vsh_sb_putc(o, ' ');
            dump_block(n->b, o);
            if (n->otherwise) {
                vsh_sb_puts(o, " else ");
                if (n->otherwise->kind == VSH_N_IF && !n->otherwise->next) {
                    dump_node(n->otherwise, o);
                } else {
                    dump_block(n->otherwise, o);
                }
            }
            break;
        case VSH_N_FOR:
            vsh_sb_puts(o, "for ");
            vsh_sb_puts(o, n->name);
            vsh_sb_puts(o, " in ");
            dump_words(n->words, o);
            vsh_sb_putc(o, ' ');
            dump_block(n->b, o);
            break;
        case VSH_N_WHILE:
            vsh_sb_puts(o, "while ");
            dump_node(n->a, o);
            vsh_sb_putc(o, ' ');
            dump_block(n->b, o);
            break;
        case VSH_N_FUNC:
            vsh_sb_puts(o, "func ");
            vsh_sb_puts(o, n->name);
            vsh_sb_puts(o, "(");
            dump_words(n->words, o);
            vsh_sb_puts(o, ") ");
            dump_block(n->b, o);
            break;
        case VSH_N_RETURN:
            vsh_sb_puts(o, "return");
            if (n->words) {
                vsh_sb_putc(o, ' ');
                dump_word(n->words, o);
            }
            break;
        case VSH_N_BREAK:
            vsh_sb_puts(o, "break");
            break;
        case VSH_N_CONTINUE:
            vsh_sb_puts(o, "continue");
            break;
        case VSH_N_BACKGROUND:
            dump_node(n->a, o);
            vsh_sb_puts(o, " &");
            break;
        case VSH_N_AND:
        case VSH_N_OR:
            vsh_sb_putc(o, '(');
            dump_node(n->a, o);
            vsh_sb_puts(o, n->kind == VSH_N_AND ? " && " : " || ");
            dump_node(n->b, o);
            vsh_sb_putc(o, ')');
            break;
        case VSH_N_PIPELINE:
            if (n->flag) {
                vsh_sb_puts(o, "try ");
            }
            for (c = n->a; c; c = c->next) {
                dump_command(c, o);
                if (c->next) {
                    vsh_sb_puts(o, " | ");
                }
            }
            break;
        case VSH_N_COMMAND:
            dump_command(n, o);
            break;
        case VSH_N_NOT:
            vsh_sb_putc(o, '!');
            dump_node(n->a, o);
            break;
        case VSH_N_CMP:
            vsh_sb_putc(o, '(');
            dump_word(n->words, o);
            vsh_sb_putc(o, ' ');
            vsh_sb_puts(o, cmps[n->cmp]);
            vsh_sb_putc(o, ' ');
            dump_word(n->words->next, o);
            vsh_sb_putc(o, ')');
            break;
        case VSH_N_TRUTH:
            dump_word(n->words, o);
            break;
        case VSH_N_CONDCMD:
            vsh_sb_puts(o, "(? ");
            dump_node(n->a, o);
            vsh_sb_putc(o, ')');
            break;
    }
}

static void dump_list(const vsh_node_t *n, vsh_sb_t *o) {
    for (; n; n = n->next) {
        dump_node(n, o);
        if (n->next) {
            vsh_sb_puts(o, " ; ");
        }
    }
}

void vsh_dump(const vsh_node_t *script, vsh_sb_t *out) {
    vsh_sb_putn(out, "", 0u);
    dump_list(script, out);
}
