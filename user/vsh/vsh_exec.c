/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Running vsh: variables, functions, builtins, pipelines, redirections and the
 * program itself (docs/shell/s4.md step 2). The language is in vsh_parse.c and
 * vsh_expand.c; this is the part that needs a kernel. */

#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "vsh.h"

extern char **environ;

#define VSH_MAX_CALLS 200     /* a function calling itself is a refusal */
#define VSH_MAX_REDIRS 16

typedef struct var {
    char *name;
    char **items;
    size_t count;
    int is_list, exported;
    struct var *next;
} var_t;

typedef struct frame {
    var_t *vars;
} frame_t;

typedef struct func {
    const char *name;
    const vsh_node_t *node;
    struct func *next;
} func_t;

typedef struct alias {
    char *name, *text;
    struct alias *next;
} alias_t;

/* F_ABORT: Ctrl+C or Ctrl+Z took the line's program; the rest of the line goes. */
enum { F_NONE, F_BREAK, F_CONTINUE, F_RETURN, F_EXIT, F_ABORT };

static struct {
    vsh_arena_t arena;     /* parsed text that outlives its statement */
    frame_t globals;
    frame_t *top;          /* the running function's, or 0 */
    func_t *funcs;
    alias_t *aliases;
    int status;
    int flow;
    int errexit;
    int in_cond;
    int loops;
    int calls;
    int cap_status;
    int jc;                /* a terminal, and this shell runs its jobs on it */
    int tty;
    pid_t shell_pgid;
    struct termios tmodes;
} G;

static int run_list(const vsh_node_t *n);

/* ---- small things ---------------------------------------------------------- */

static void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1u);

    if (!p) {
        (void)!write(2, "vsh: out of memory\n", 19);
        _exit(2);
    }
    return p;
}

static char *xstrdup(const char *s) {
    size_t n = strlen(s) + 1u;

    return (char *)memcpy(xmalloc(n), s, n);
}

/* One write per message: two writers on one console must not split a line. */
static void put_all(int fd, const char *s, size_t n) {
    while (n > 0u) {
        ssize_t w = write(fd, s, n);

        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            return;
        }
        s += w;
        n -= (size_t)w;
    }
}

static void say_err(const char *a, const char *b) {
    vsh_sb_t o = {0, 0, 0};

    vsh_sb_puts(&o, "vsh: ");
    vsh_sb_puts(&o, a);
    if (b) {
        vsh_sb_puts(&o, ": ");
        vsh_sb_puts(&o, b);
    }
    vsh_sb_putc(&o, '\n');
    put_all(2, o.b, o.n);
    vsh_sb_free(&o);
}

static int same_nocase(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        char x = *a >= 'A' && *a <= 'Z' ? (char)(*a + 32) : *a;
        char y = *b >= 'A' && *b <= 'Z' ? (char)(*b + 32) : *b;

        if (x != y) {
            return 0;
        }
    }
    return *a == *b;
}

static int wait_status(pid_t pid) {
    int st = 0;

    while (waitpid(pid, &st, 0) < 0) {
        if (errno != EINTR) {
            return 127;
        }
    }
    return WIFSIGNALED(st) ? 128 + WTERMSIG(st) : WEXITSTATUS(st);
}

/* ---- jobs ------------------------------------------------------------------ */

#define VSH_MAX_JOBS 32
#define VSH_JOB_PIDS 64

enum { J_FREE, J_RUNNING, J_STOPPED, J_DONE };

typedef struct {
    int state;
    pid_t pgid;
    int own_group;        /* a script's job shares the shell's group */
    int n;
    pid_t pids[VSH_JOB_PIDS];
    char finished[VSH_JOB_PIDS];
    int st[VSH_JOB_PIDS];
    char text[120];
    struct termios modes;
    int has_modes;
    unsigned long seq;
} job_t;

static job_t g_jobs[VSH_MAX_JOBS];
static unsigned long g_job_seq;

static int decode_status(int st) {
    return WIFSIGNALED(st) ? 128 + WTERMSIG(st) : WEXITSTATUS(st);
}

static void job_forget(void) {
    memset(g_jobs, 0, sizeof(g_jobs));
}

static int job_result(const job_t *j) {
    int i, r = 0;

    for (i = 0; i < j->n; i++) {
        /* `yes | head`: the writer told to stop is not a failure. */
        if (j->st[i] != 0 && j->st[i] != 128 + SIGPIPE) {
            r = j->st[i];
        }
    }
    return r;
}

static void job_update(job_t *j, pid_t pid, int st) {
    int i, left = 0;

    for (i = 0; i < j->n; i++) {
        if (j->pids[i] == pid) {
            if (WIFSTOPPED(st)) {
                j->state = J_STOPPED;
                return;
            }
            if (WIFCONTINUED(st)) {
                j->state = J_RUNNING;
                return;
            }
            j->finished[i] = 1;
            j->st[i] = decode_status(st);
        }
        left += !j->finished[i];
    }
    if (!left) {
        j->state = J_DONE;
    }
}

/* Until it ends or stops. */
static void job_wait(job_t *j) {
    while (j->state == J_RUNNING) {
        int i, st = 0;
        pid_t got;

        for (i = 0; i < j->n && j->finished[i]; i++) {
        }
        if (i == j->n) {
            j->state = J_DONE;
            return;
        }
        got = waitpid(j->pids[i], &st, G.jc ? WUNTRACED : 0);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            j->finished[i] = 1;
            j->st[i] = 127;
            continue;
        }
        job_update(j, got, st);
    }
}

static void job_signal(const job_t *j, int sig) {
    int i;

    if (j->own_group) {
        (void)kill(-j->pgid, sig);
        return;
    }
    for (i = 0; i < j->n; i++) {
        if (!j->finished[i]) {
            (void)kill(j->pids[i], sig);
        }
    }
}

/* A free entry; in a script nobody collects a finished job unless it waits, so
 * the oldest of those goes when the table is full. */
static job_t *job_slot(void) {
    job_t *old = 0;
    int i;

    for (i = 0; i < VSH_MAX_JOBS; i++) {
        if (g_jobs[i].state == J_FREE) {
            return &g_jobs[i];
        }
        if (g_jobs[i].state == J_DONE && (!old || g_jobs[i].seq < old->seq)) {
            old = &g_jobs[i];
        }
    }
    return old;
}

static int job_id(const job_t *j) {
    return (int)(j - g_jobs) + 1;
}

static int job_in_table(const job_t *j) {
    return j >= g_jobs && j < g_jobs + VSH_MAX_JOBS;
}

static job_t *job_current(void) {
    job_t *best = 0;
    int i;

    for (i = 0; i < VSH_MAX_JOBS; i++) {
        job_t *j = &g_jobs[i];

        if ((j->state == J_RUNNING || j->state == J_STOPPED) && (!best || j->seq > best->seq)) {
            best = j;
        }
    }
    return best;
}

static job_t *job_from_spec(const char *spec) {
    long n;
    char *end = 0;

    if (!spec || strcmp(spec, "%%") == 0 || strcmp(spec, "%+") == 0) {
        return job_current();
    }
    if (spec[0] != '%') {
        return 0;
    }
    n = strtol(spec + 1, &end, 10);
    if (end == spec + 1 || *end || n < 1 || n > VSH_MAX_JOBS || g_jobs[n - 1].state == J_FREE) {
        return 0;
    }
    return &g_jobs[n - 1];
}

static void job_line(const job_t *j, const char *state) {
    vsh_sb_t o = {0, 0, 0};
    char head[32];

    snprintf(head, sizeof(head), "[%d]%c ", job_id(j), j == job_current() ? '+' : ' ');
    vsh_sb_puts(&o, head);
    vsh_sb_puts(&o, state);
    vsh_sb_puts(&o, "  ");
    vsh_sb_puts(&o, j->text);
    vsh_sb_putc(&o, '\n');
    put_all(1, o.b, o.n);
    vsh_sb_free(&o);
}

static void word_text(const vsh_word_t *w, vsh_sb_t *o) {
    const vsh_part_t *p;

    for (p = w->parts; p; p = p->next) {
        if (p->kind == VSH_P_LIT) {
            vsh_sb_puts(o, p->text);
        } else if (p->kind == VSH_P_VAR) {
            vsh_sb_putc(o, '$');
            vsh_sb_puts(o, p->text);
        } else {
            vsh_sb_puts(o, "...");
        }
    }
}

/* What `jobs` calls it. */
static void node_text(const vsh_node_t *n, vsh_sb_t *o) {
    const vsh_node_t *c;
    const vsh_word_t *w;

    if (n->kind == VSH_N_AND || n->kind == VSH_N_OR) {
        node_text(n->a, o);
        vsh_sb_puts(o, n->kind == VSH_N_AND ? " && " : " || ");
        node_text(n->b, o);
    } else if (n->kind == VSH_N_PIPELINE) {
        for (c = n->a; c; c = c->next) {
            node_text(c, o);
            if (c->next) {
                vsh_sb_puts(o, " | ");
            }
        }
    } else if (n->kind == VSH_N_COMMAND) {
        for (w = n->words; w; w = w->next) {
            word_text(w, o);
            if (w->next) {
                vsh_sb_putc(o, ' ');
            }
        }
    }
}

static void job_name(job_t *j, const vsh_node_t *n) {
    vsh_sb_t o = {0, 0, 0};

    vsh_sb_putn(&o, "", 0u);
    node_text(n, &o);
    snprintf(j->text, sizeof(j->text), "%s", o.b);
    vsh_sb_free(&o);
}

/* A child that becomes a job. The signals first: once it is in the foreground
 * a Ctrl+Z is meant for it, and one it still ignores is lost. SIGTTOU last,
 * because taking the terminal from the background raises it. */
static void job_child(pid_t pgid, int foreground) {
    if (G.jc) {
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGTSTP, SIG_DFL);
        signal(SIGTTIN, SIG_DFL);
        (void)setpgid(0, pgid);
        if (foreground) {
            (void)tcsetpgrp(G.tty, getpgrp());
        }
        signal(SIGTTOU, SIG_DFL);
        G.jc = 0;
    }
    job_forget();
}

/* A $(...): part of the shell's own line, in the shell's group. Ctrl+Z stays
 * ignored there, since nobody could resume it from a prompt that is waiting. */
static void subshell_child(void) {
    if (G.jc) {
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGTTIN, SIG_DFL);
        signal(SIGTTOU, SIG_DFL);
        G.jc = 0;
    }
    job_forget();
}

static int job_finish_foreground(job_t *j) {
    int r;

    job_wait(j);
    if (G.jc) {
        if (j->state == J_STOPPED) {
            j->has_modes = tcgetattr(G.tty, &j->modes) == 0;
        }
        (void)tcsetpgrp(G.tty, G.shell_pgid);
        (void)tcsetattr(G.tty, TCSANOW, &G.tmodes);
    }
    if (j->state == J_STOPPED) {
        job_t *t = job_in_table(j) ? j : job_slot();

        if (!t) {
            say_err("too many jobs", "ending this one");
            job_signal(j, SIGKILL);
            job_signal(j, SIGCONT);
            j->state = J_RUNNING;
            job_wait(j);
            G.flow = F_ABORT;
            return 137;
        }
        if (t != j) {
            *t = *j;
        }
        t->seq = ++g_job_seq;
        (void)!write(1, "\n", 1);
        job_line(t, "Stopped");
        G.flow = F_ABORT;
        return 128 + SIGTSTP;
    }
    r = job_result(j);
    if (job_in_table(j)) {
        j->state = J_FREE;
    }
    if (G.jc && r == 128 + SIGINT) {
        (void)!write(1, "\n", 1);
        G.flow = F_ABORT;   /* a loop around it must not carry on */
    }
    return r;
}

/* ---- variables ------------------------------------------------------------- */

static var_t *var_find(frame_t *f, const char *name) {
    var_t *v;

    for (v = f->vars; v; v = v->next) {
        if (strcmp(v->name, name) == 0) {
            return v;
        }
    }
    return 0;
}

static void var_clear(var_t *v) {
    size_t i;

    for (i = 0; i < v->count; i++) {
        free(v->items[i]);
    }
    free(v->items);
    v->items = 0;
    v->count = 0;
}

static var_t *var_put(frame_t *f, const char *name, const char *const *items, size_t count, int is_list) {
    var_t *v = var_find(f, name);
    char **copy = (char **)xmalloc((count + 1u) * sizeof(*copy));
    size_t i;

    /* Copied before the old value goes: `set x = ($x a)` reads what it frees. */
    for (i = 0; i < count; i++) {
        copy[i] = xstrdup(items[i]);
    }
    if (!v) {
        v = (var_t *)xmalloc(sizeof(*v));
        memset(v, 0, sizeof(*v));
        v->name = xstrdup(name);
        v->next = f->vars;
        f->vars = v;
    } else {
        var_clear(v);
    }
    v->items = copy;
    v->count = count;
    v->is_list = is_list;
    return v;
}

static void frame_free(frame_t *f) {
    while (f->vars) {
        var_t *v = f->vars;

        f->vars = v->next;
        var_clear(v);
        free(v->name);
        free(v);
    }
}

static int var_remove(frame_t *f, const char *name) {
    var_t **at;

    for (at = &f->vars; *at; at = &(*at)->next) {
        if (strcmp((*at)->name, name) == 0) {
            var_t *v = *at;

            *at = v->next;
            var_clear(v);
            free(v->name);
            free(v);
            return 1;
        }
    }
    return 0;
}

static var_t *var_lookup(const char *name) {
    var_t *v = G.top ? var_find(G.top, name) : 0;

    return v ? v : var_find(&G.globals, name);
}

static void set_scalar(frame_t *f, const char *name, const char *value) {
    const char *one[1];

    one[0] = value;
    (void)var_put(f, name, one, 1u, 0);
}

static int ctx_lookup(void *user, const char *name, vsh_value_t *out) {
    var_t *v = var_lookup(name);

    (void)user;
    if (!v) {
        return -1;
    }
    out->count = v->count;
    out->items = (const char *const *)v->items;
    out->is_list = v->is_list;
    return 0;
}

/* ---- running a script for its output --------------------------------------- */

static int ctx_capture(void *user, const vsh_node_t *script, char **out) {
    vsh_ctx_t *c = (vsh_ctx_t *)user;
    vsh_sb_t text = {0, 0, 0};
    char buf[4096];
    int fds[2];
    pid_t pid;

    if (pipe(fds) != 0) {
        return -1;
    }
    pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    if (pid == 0) {
        subshell_child();
        close(fds[0]);
        if (fds[1] != 1) {
            dup2(fds[1], 1);
            close(fds[1]);
        }
        G.flow = F_NONE;
        (void)run_list(script);
        _exit(G.status & 0xff);
    }
    close(fds[1]);
    vsh_sb_putn(&text, "", 0u);
    for (;;) {
        ssize_t n = read(fds[0], buf, sizeof(buf));

        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            break;
        }
        vsh_sb_putn(&text, buf, (size_t)n);
    }
    close(fds[0]);
    G.cap_status = wait_status(pid);
    *out = vsh_strndup(c->arena, text.b, text.n);
    vsh_sb_free(&text);
    return 0;
}

static int ctx_run(void *user, const vsh_node_t *chain, int *status) {
    (void)user;
    G.in_cond++;
    /* A chain is a statement of one: run_list would walk its next, and a
     * condition's chain has none. */
    (void)run_list(chain);
    G.in_cond--;
    *status = G.status;
    return 0;
}

static void ctx_init(vsh_ctx_t *c, vsh_arena_t *a) {
    memset(c, 0, sizeof(*c));
    vsh_arena_init(a);
    c->arena = a;
    c->user = c;
    c->status = G.status;
    c->lookup = ctx_lookup;
    c->capture = ctx_capture;
    c->run = ctx_run;
}

/* ---- pathname expansion ---------------------------------------------------- */

static int has_magic(const char *p, size_t n) {
    size_t i = 0;

    while (i < n) {
        if (p[i] == '\\' && i + 1u < n) {
            i += 2u;      /* an escaped character is not magic */
            continue;
        }
        if (p[i] == '*' || p[i] == '?' || p[i] == '[') {
            return 1;
        }
        i++;
    }
    return 0;
}

static int cmp_str(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static void list_add(vsh_ctx_t *c, vsh_list_t *l, const char *s) {
    if (l->count == l->cap) {
        size_t cap = l->cap ? l->cap * 2u : 16u;
        const char **items = (const char **)vsh_alloc(c->arena, cap * sizeof(*items));

        if (l->count) {
            memcpy(items, l->items, l->count * sizeof(*items));
        }
        l->items = items;
        l->cap = cap;
    }
    l->items[l->count++] = s;
}

static void glob_walk(vsh_ctx_t *c, const char *base, const char *pat, vsh_list_t *out) {
    size_t n = 0;
    int last;
    char *comp;

    while (pat[n] && pat[n] != '/') {
        n += pat[n] == '\\' && pat[n + 1] ? 2u : 1u;
    }
    last = !pat[n];
    comp = vsh_strndup(c->arena, pat, n);

    if (!has_magic(comp, n)) {
        vsh_sb_t path = {0, 0, 0};
        struct stat st;
        size_t i = 0;

        vsh_sb_puts(&path, base);
        while (i < n) {
            i += comp[i] == '\\' && i + 1u < n ? 1u : 0u;
            vsh_sb_putc(&path, comp[i]);
            i++;
        }
        if (last) {
            if (lstat(path.b, &st) == 0) {
                list_add(c, out, vsh_strndup(c->arena, path.b, path.n));
            }
        } else {
            vsh_sb_putc(&path, '/');
            glob_walk(c, path.b, pat + n + 1u, out);
        }
        vsh_sb_free(&path);
        return;
    }
    {
        DIR *d = opendir(*base ? base : ".");
        struct dirent *e;

        if (!d) {
            return;
        }
        while ((e = readdir(d)) != 0) {
            vsh_sb_t path = {0, 0, 0};
            struct stat st;

            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
                continue;
            }
            /* A hidden file is named on purpose. */
            if (e->d_name[0] == '.' && comp[0] != '.') {
                continue;
            }
            if (!vsh_match(comp, e->d_name, 0)) {
                continue;
            }
            vsh_sb_puts(&path, base);
            vsh_sb_puts(&path, e->d_name);
            if (last) {
                list_add(c, out, vsh_strndup(c->arena, path.b, path.n));
            } else if (stat(path.b, &st) == 0 && S_ISDIR(st.st_mode)) {
                vsh_sb_putc(&path, '/');
                glob_walk(c, path.b, pat + n + 1u, out);
            }
            vsh_sb_free(&path);
        }
        closedir(d);
    }
}

/* A word, then the files it names if it is a pattern; a pattern naming nothing
 * stays what was written. */
static int expand_glob(vsh_ctx_t *c, const vsh_word_t *w, vsh_list_t *out) {
    const vsh_part_t *p;
    int literal_magic = 0;

    for (p = w->parts; p; p = p->next) {
        if (p->kind == VSH_P_LIT && !p->quoted && strpbrk(p->text, "*?[")) {
            literal_magic = 1;
        }
    }
    if (literal_magic) {
        char *pat;
        size_t before = out->count;

        if (vsh_word_glob(c, w, &pat) != 0) {
            return -1;
        }
        glob_walk(c, pat[0] == '/' ? "/" : "", pat[0] == '/' ? pat + 1 : pat, out);
        if (out->count > before) {
            qsort(out->items + before, out->count - before, sizeof(*out->items), cmp_str);
            return 0;
        }
    }
    return vsh_expand_word(c, w, out);
}

static int expand_words(vsh_ctx_t *c, const vsh_word_t *w, vsh_list_t *out) {
    for (; w; w = w->next) {
        if (expand_glob(c, w, out) != 0) {
            return -1;
        }
    }
    return 0;
}

/* ---- redirections ---------------------------------------------------------- */

typedef struct {
    int fd, saved;
} saved_fd_t;

/* With `saved`, what each descriptor was goes there, for a builtin that runs
 * in the shell and must leave its descriptors as it found them. */
static int apply_redirs(vsh_ctx_t *c, const vsh_redir_t *r, saved_fd_t *saved, int *nsaved) {
    for (; r; r = r->next) {
        int src;

        if (saved) {
            if (*nsaved >= VSH_MAX_REDIRS) {
                say_err("too many redirections", 0);
                return -1;
            }
            saved[*nsaved].fd = r->fd;
            saved[*nsaved].saved = fcntl(r->fd, F_DUPFD_CLOEXEC, 10);
            (*nsaved)++;
        }
        if (r->kind == VSH_R_DUP) {
            if (dup2(r->dup_to, r->fd) < 0) {
                say_err("bad descriptor in redirection", strerror(errno));
                return -1;
            }
            continue;
        }
        {
            char *name;
            int flags = r->kind == VSH_R_IN ? O_RDONLY
                      : r->kind == VSH_R_APPEND ? O_WRONLY | O_CREAT | O_APPEND
                                                : O_WRONLY | O_CREAT | O_TRUNC;

            if (vsh_word_text(c, r->target, &name) != 0) {
                say_err(c->err, 0);
                return -1;
            }
            src = open(name, flags, 0666);
            if (src < 0) {
                say_err(name, strerror(errno));
                return -1;
            }
            if (src != r->fd) {
                if (dup2(src, r->fd) < 0) {
                    say_err(name, strerror(errno));
                    close(src);
                    return -1;
                }
                close(src);
            }
        }
    }
    return 0;
}

static void restore_redirs(const saved_fd_t *saved, int n) {
    while (n-- > 0) {
        if (saved[n].saved >= 0) {
            dup2(saved[n].saved, saved[n].fd);
            close(saved[n].saved);
        } else {
            close(saved[n].fd);
        }
    }
}

/* ---- finding a program ----------------------------------------------------- */

static int find_program(const char *name, vsh_sb_t *out) {
    const char *path = 0;
    var_t *v;
    struct stat st;

    if (strchr(name, '/')) {
        vsh_sb_puts(out, name);
        return 0;
    }
    v = var_lookup("PATH");
    path = v && v->count ? v->items[0] : "/usr/bin";
    while (*path) {
        const char *end = strchr(path, ':');
        size_t n = end ? (size_t)(end - path) : strlen(path);

        out->n = 0;
        vsh_sb_putn(out, path, n);
        if (n) {
            vsh_sb_putc(out, '/');
        }
        vsh_sb_puts(out, name);
        if (stat(out->b, &st) == 0 && S_ISREG(st.st_mode) && access(out->b, X_OK) == 0) {
            return 0;
        }
        if (!end) {
            break;
        }
        path = end + 1;
    }
    return -1;
}

static char **build_env(vsh_ctx_t *c, const vsh_assign_t *as) {
    size_t n = 0, at = 0;
    const var_t *v;
    const vsh_assign_t *a;
    char **env;

    for (v = G.globals.vars; v; v = v->next) {
        n += v->exported ? 1u : 0u;
    }
    for (a = as; a; a = a->next) {
        n++;
    }
    env = (char **)vsh_alloc(c->arena, (n + 1u) * sizeof(*env));
    for (a = as; a; a = a->next) {
        vsh_sb_t e = {0, 0, 0};
        char *val;
        const vsh_assign_t *later;
        int shadowed = 0;

        for (later = a->next; later; later = later->next) {
            shadowed |= strcmp(later->name, a->name) == 0;
        }
        if (vsh_word_text(c, a->value, &val) != 0) {
            return 0;
        }
        if (!shadowed) {
            vsh_sb_puts(&e, a->name);
            vsh_sb_putc(&e, '=');
            vsh_sb_puts(&e, val);
            env[at++] = vsh_strndup(c->arena, e.b, e.n);
        }
        vsh_sb_free(&e);
    }
    for (v = G.globals.vars; v; v = v->next) {
        vsh_sb_t e = {0, 0, 0};
        size_t i;
        int over = 0;

        if (!v->exported) {
            continue;
        }
        for (a = as; a; a = a->next) {
            over |= strcmp(a->name, v->name) == 0;
        }
        if (over) {
            continue;
        }
        vsh_sb_puts(&e, v->name);
        vsh_sb_putc(&e, '=');
        for (i = 0; i < v->count; i++) {
            if (i) {
                vsh_sb_putc(&e, ' ');
            }
            vsh_sb_puts(&e, v->items[i]);
        }
        env[at++] = vsh_strndup(c->arena, e.b, e.n);
        vsh_sb_free(&e);
    }
    env[at] = 0;
    return env;
}

/* Does not return. */
static void exec_program(vsh_ctx_t *c, const vsh_node_t *cmd, char **argv) {
    vsh_sb_t path = {0, 0, 0};
    char **env = build_env(c, cmd->assigns);

    if (!env) {
        say_err(c->err, 0);
        _exit(1);
    }
    if (find_program(argv[0], &path) != 0) {
        say_err(argv[0], "command not found");
        _exit(127);
    }
    execve(path.b, argv, env);
    {
        int e = errno;

        say_err(argv[0], strerror(e));
        _exit(e == ENOENT ? 127 : 126);
    }
}

/* ---- builtins -------------------------------------------------------------- */

static int run_file(const char *path);

static int b_true(int argc, char **argv) {
    (void)argc;
    (void)argv;
    return 0;
}

static int b_false(int argc, char **argv) {
    (void)argc;
    (void)argv;
    return 1;
}

static int b_echo(int argc, char **argv) {
    vsh_sb_t o = {0, 0, 0};
    int i = 1, nl = 1;

    if (i < argc && strcmp(argv[i], "-n") == 0) {
        nl = 0;
        i++;
    }
    vsh_sb_putn(&o, "", 0u);
    for (; i < argc; i++) {
        vsh_sb_puts(&o, argv[i]);
        if (i + 1 < argc) {
            vsh_sb_putc(&o, ' ');
        }
    }
    if (nl) {
        vsh_sb_putc(&o, '\n');
    }
    put_all(1, o.b, o.n);
    vsh_sb_free(&o);
    return 0;
}

static int b_pwd(int argc, char **argv) {
    char buf[PATH_MAX + 1];
    size_t n;

    (void)argc;
    (void)argv;
    if (!getcwd(buf, sizeof(buf) - 1u)) {
        say_err("pwd", strerror(errno));
        return 1;
    }
    n = strlen(buf);
    buf[n++] = '\n';
    put_all(1, buf, n);
    return 0;
}

static int b_cd(int argc, char **argv) {
    const char *to = argc > 1 ? argv[1] : 0;
    char buf[PATH_MAX];

    if (argc > 2) {
        say_err("cd", "too many arguments");
        return 1;
    }
    if (!to) {
        var_t *h = var_lookup("HOME");

        if (!h || !h->count) {
            say_err("cd", "HOME is not set");
            return 1;
        }
        to = h->items[0];
    }
    if (chdir(to) != 0) {
        say_err(to, strerror(errno));
        return 1;
    }
    if (getcwd(buf, sizeof(buf))) {
        var_t *v = var_find(&G.globals, "PWD");
        int exported = v ? v->exported : 1;

        set_scalar(&G.globals, "PWD", buf);
        set_scalar(&G.globals, "CD", buf);    /* %CD%, as cmd has it */
        var_find(&G.globals, "PWD")->exported = exported;
    }
    return 0;
}

static int b_unset(int argc, char **argv) {
    int i;

    for (i = 1; i < argc; i++) {
        if (!(G.top && var_remove(G.top, argv[i]))) {
            (void)var_remove(&G.globals, argv[i]);
        }
    }
    return 0;
}

static int b_alias(int argc, char **argv) {
    vsh_sb_t text = {0, 0, 0};
    alias_t *a;
    int i;

    if (argc == 1) {
        for (a = G.aliases; a; a = a->next) {
            vsh_sb_puts(&text, a->name);
            vsh_sb_puts(&text, " = ");
            vsh_sb_puts(&text, a->text);
            vsh_sb_putc(&text, '\n');
        }
        if (text.n) {
            put_all(1, text.b, text.n);
        }
        vsh_sb_free(&text);
        return 0;
    }
    if (argc < 4 || strcmp(argv[2], "=") != 0) {
        say_err("alias", "takes: alias name = command");
        return 1;
    }
    for (i = 3; i < argc; i++) {
        vsh_sb_puts(&text, argv[i]);
        if (i + 1 < argc) {
            vsh_sb_putc(&text, ' ');
        }
    }
    for (a = G.aliases; a; a = a->next) {
        if (same_nocase(a->name, argv[1])) {
            free(a->text);
            a->text = xstrdup(text.b);
            vsh_sb_free(&text);
            return 0;
        }
    }
    a = (alias_t *)xmalloc(sizeof(*a));
    a->name = xstrdup(argv[1]);
    a->text = xstrdup(text.b);
    a->next = G.aliases;
    G.aliases = a;
    vsh_sb_free(&text);
    return 0;
}

static int b_unalias(int argc, char **argv) {
    int i, rc = 0;

    for (i = 1; i < argc; i++) {
        alias_t **at = &G.aliases;

        while (*at && !same_nocase((*at)->name, argv[i])) {
            at = &(*at)->next;
        }
        if (*at) {
            alias_t *a = *at;

            *at = a->next;
            free(a->name);
            free(a->text);
            free(a);
        } else {
            say_err(argv[i], "no such alias");
            rc = 1;
        }
    }
    return rc;
}

static int parse_status(const char *t, int *out) {
    char *end = 0;
    long v;

    errno = 0;
    v = strtol(t, &end, 10);
    if (!*t || *end || errno) {
        return -1;
    }
    *out = (int)(v & 0xff);
    return 0;
}

static int b_exit(int argc, char **argv) {
    int code = G.status;

    if (argc > 1 && parse_status(argv[1], &code) != 0) {
        say_err("exit: not a number", argv[1]);
        code = 2;
    }
    G.flow = F_EXIT;
    return code;
}

static int b_source(int argc, char **argv) {
    if (argc != 2) {
        say_err("source", "takes one file");
        return 1;
    }
    return run_file(argv[1]);
}

static void jobs_collect(void) {
    int st = 0, i;
    pid_t pid;

    while ((pid = waitpid(-1, &st, WNOHANG | WUNTRACED | WCONTINUED)) > 0) {
        for (i = 0; i < VSH_MAX_JOBS; i++) {
            if (g_jobs[i].state != J_FREE) {
                job_update(&g_jobs[i], pid, st);
            }
        }
    }
}

static int wait_one(job_t *j) {
    int r;

    if (j->state == J_STOPPED) {
        say_err(j->text, "the job is stopped");
        return 128 + SIGTSTP;
    }
    job_wait(j);
    if (j->state == J_STOPPED) {
        return 128 + SIGTSTP;
    }
    r = job_result(j);
    j->state = J_FREE;
    return r;
}

static int b_wait(int argc, char **argv) {
    int i, rc = 0;

    if (argc == 1) {
        for (i = 0; i < VSH_MAX_JOBS; i++) {
            if (g_jobs[i].state == J_RUNNING || g_jobs[i].state == J_DONE) {
                (void)wait_one(&g_jobs[i]);
            }
        }
        return 0;
    }
    for (i = 1; i < argc; i++) {
        if (argv[i][0] == '%') {
            job_t *j = job_from_spec(argv[i]);

            if (!j) {
                say_err(argv[i], "no such job");
                rc = 127;
            } else {
                rc = wait_one(j);
            }
        } else {
            pid_t pid = (pid_t)atol(argv[i]);
            int k, found = 0;

            for (k = 0; k < VSH_MAX_JOBS && !found; k++) {
                if (g_jobs[k].state != J_FREE && g_jobs[k].pids[0] == pid) {
                    rc = wait_one(&g_jobs[k]);
                    found = 1;
                }
            }
            if (!found) {
                rc = wait_status(pid);
            }
        }
    }
    return rc;
}

static int b_jobs(int argc, char **argv) {
    int i;

    (void)argc;
    (void)argv;
    jobs_collect();
    for (i = 0; i < VSH_MAX_JOBS; i++) {
        job_t *j = &g_jobs[i];

        if (j->state == J_RUNNING) {
            job_line(j, "Running");
        } else if (j->state == J_STOPPED) {
            job_line(j, "Stopped");
        } else if (j->state == J_DONE) {
            job_line(j, "Done");
            j->state = J_FREE;
        }
    }
    return 0;
}

static job_t *job_argument(const char *who, int argc, char **argv) {
    job_t *j;

    if (!G.jc) {
        say_err(who, "no job control here");
        return 0;
    }
    jobs_collect();
    j = job_from_spec(argc > 1 ? argv[1] : 0);
    if (!j || j->state == J_DONE) {
        say_err(who, "no such job");
        return 0;
    }
    return j;
}

static int b_fg(int argc, char **argv) {
    job_t *j = job_argument("fg", argc, argv);
    vsh_sb_t o = {0, 0, 0};

    if (!j) {
        return 1;
    }
    vsh_sb_puts(&o, j->text);
    vsh_sb_putc(&o, '\n');
    put_all(1, o.b, o.n);
    vsh_sb_free(&o);
    (void)tcsetpgrp(G.tty, j->pgid);
    if (j->has_modes) {
        (void)tcsetattr(G.tty, TCSANOW, &j->modes);
    }
    j->state = J_RUNNING;
    job_signal(j, SIGCONT);
    return job_finish_foreground(j);
}

static int b_bg(int argc, char **argv) {
    job_t *j = job_argument("bg", argc, argv);
    vsh_sb_t o = {0, 0, 0};
    char head[24];

    if (!j) {
        return 1;
    }
    j->state = J_RUNNING;
    j->seq = ++g_job_seq;
    job_signal(j, SIGCONT);
    snprintf(head, sizeof(head), "[%d] ", job_id(j));
    vsh_sb_puts(&o, head);
    vsh_sb_puts(&o, j->text);
    vsh_sb_puts(&o, " &\n");
    put_all(1, o.b, o.n);
    vsh_sb_free(&o);
    return 0;
}

static int signal_number(const char *t) {
    static const struct {
        const char *n;
        int s;
    } names[] = {
        {"HUP", SIGHUP},   {"INT", SIGINT},   {"QUIT", SIGQUIT}, {"KILL", SIGKILL},
        {"USR1", SIGUSR1}, {"USR2", SIGUSR2}, {"PIPE", SIGPIPE}, {"ALRM", SIGALRM},
        {"TERM", SIGTERM}, {"CHLD", SIGCHLD}, {"CONT", SIGCONT}, {"STOP", SIGSTOP},
        {"TSTP", SIGTSTP},
    };
    size_t i;
    char *end = 0;
    long v;

    if (same_nocase(t, "0")) {
        return 0;
    }
    if (strncmp(t, "SIG", 3) == 0 || strncmp(t, "sig", 3) == 0) {
        t += 3;
    }
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (same_nocase(names[i].n, t)) {
            return names[i].s;
        }
    }
    v = strtol(t, &end, 10);
    return end != t && !*end && v > 0 && v < 65 ? (int)v : -1;
}

static int b_kill(int argc, char **argv) {
    int sig = SIGTERM, i = 1, rc = 0;

    if (i < argc && argv[i][0] == '-' && argv[i][1]) {
        sig = signal_number(argv[i] + 1);
        if (sig < 0) {
            say_err("kill: no such signal", argv[i] + 1);
            return 2;
        }
        i++;
    }
    if (i >= argc) {
        say_err("kill", "takes a pid or %job");
        return 2;
    }
    for (; i < argc; i++) {
        if (argv[i][0] == '%') {
            job_t *j = job_from_spec(argv[i]);

            if (!j) {
                say_err(argv[i], "no such job");
                rc = 1;
                continue;
            }
            job_signal(j, sig);
            /* A stopped job acts on a signal only once it runs. */
            if (j->state == J_STOPPED && sig != SIGSTOP && sig != SIGTSTP && sig != 0) {
                job_signal(j, SIGCONT);
                j->state = J_RUNNING;
            }
        } else {
            char *end = 0;
            long pid = strtol(argv[i], &end, 10);

            if (end == argv[i] || *end || kill((pid_t)pid, sig) != 0) {
                say_err(argv[i], *end ? "not a pid" : strerror(errno));
                rc = 1;
            }
        }
    }
    return rc;
}

static int b_type(int argc, char **argv);
int vsh_history_cmd(int argc, char **argv);

static const struct {
    const char *name;
    int (*fn)(int argc, char **argv);
} g_builtins[] = {
    {"alias", b_alias}, {"bg", b_bg},       {"cd", b_cd},           {"echo", b_echo},
    {"exit", b_exit},   {"false", b_false}, {"fg", b_fg},           {"history", vsh_history_cmd}, {"jobs", b_jobs},
    {"kill", b_kill},   {"pwd", b_pwd},     {"source", b_source},   {"true", b_true},
    {"type", b_type},   {"unalias", b_unalias}, {"unset", b_unset}, {"wait", b_wait},
};

static int (*find_builtin(const char *name))(int, char **) {
    size_t i;

    for (i = 0; i < sizeof(g_builtins) / sizeof(g_builtins[0]); i++) {
        if (same_nocase(g_builtins[i].name, name)) {
            return g_builtins[i].fn;
        }
    }
    return 0;
}

/* For the line editor's Tab. 0 past the last. */
const char *vsh_builtin_name(size_t i) {
    return i < sizeof(g_builtins) / sizeof(g_builtins[0]) ? g_builtins[i].name : 0;
}

static func_t *find_func(const char *name) {
    func_t *f;

    for (f = G.funcs; f; f = f->next) {
        if (strcmp(f->name, name) == 0) {
            return f;
        }
    }
    return 0;
}

static alias_t *find_alias(const char *name) {
    alias_t *a;

    for (a = G.aliases; a; a = a->next) {
        if (same_nocase(a->name, name)) {
            return a;
        }
    }
    return 0;
}

static int b_type(int argc, char **argv) {
    int i, rc = 0;

    for (i = 1; i < argc; i++) {
        vsh_sb_t o = {0, 0, 0}, path = {0, 0, 0};

        vsh_sb_puts(&o, argv[i]);
        if (find_alias(argv[i])) {
            vsh_sb_puts(&o, " is an alias");
        } else if (find_func(argv[i])) {
            vsh_sb_puts(&o, " is a function");
        } else if (find_builtin(argv[i])) {
            vsh_sb_puts(&o, " is a builtin");
        } else if (find_program(argv[i], &path) == 0) {
            vsh_sb_puts(&o, " is ");
            vsh_sb_puts(&o, path.b);
        } else {
            vsh_sb_puts(&o, " not found");
            rc = 1;
        }
        vsh_sb_putc(&o, '\n');
        put_all(1, o.b, o.n);
        vsh_sb_free(&o);
        vsh_sb_free(&path);
    }
    return rc;
}

/* ---- commands -------------------------------------------------------------- */

static int call_func(const func_t *f, int argc, char **argv) {
    frame_t *frame;
    frame_t *caller = G.top;
    const vsh_word_t *p;
    int i = 1, loops = G.loops;

    if (G.calls >= VSH_MAX_CALLS) {
        say_err(f->name, "function calls nested too deeply");
        return 1;
    }
    /* On the heap: G.top must never hold the address of a local. */
    frame = (frame_t *)calloc(1u, sizeof(*frame));
    if (!frame) {
        say_err(f->name, "out of memory");
        return 1;
    }
    for (p = f->node->words; p; p = p->next, i++) {
        set_scalar(frame, p->parts->text, i < argc ? argv[i] : "");
    }
    (void)var_put(frame, "args", (const char *const *)(argv + 1), (size_t)(argc - 1), 1);
    G.top = frame;
    G.calls++;
    G.loops = 0;
    G.status = 0;
    (void)run_list(f->node->b);
    if (G.flow == F_RETURN) {
        G.flow = F_NONE;
    }
    G.loops = loops;
    G.calls--;
    G.top = caller;
    frame_free(frame);
    free(frame);
    return G.status;
}

/* The command's words, its alias applied, ending in a null. 0, or -1 with the
 * status the command ends with in *rc. */
static int command_words(vsh_ctx_t *c, const vsh_node_t *cmd, vsh_arena_t *arena,
                         vsh_list_t *words, int *rc) {
    const alias_t *al;
    size_t i;

    G.cap_status = 0;
    *rc = 1;
    if (expand_words(c, cmd->words, words) != 0) {
        say_err(c->err, 0);
        return -1;
    }
    if (words->count == 0u) {       /* an empty list and nothing else */
        *rc = G.cap_status;
        return -1;
    }
    /* One round, so that `alias ls = ls -l` is not a loop. */
    al = find_alias(words->items[0]);
    if (al) {
        char err[128];
        vsh_list_t rest = {0, 0, 0};
        const vsh_node_t *parsed = vsh_parse(arena, al->text, err, sizeof(err), 0);

        if (!parsed || parsed->kind != VSH_N_PIPELINE || parsed->a->next) {
            say_err(al->name, "the alias is not one command");
            return -1;
        }
        if (expand_words(c, parsed->a->words, &rest) != 0) {
            say_err(c->err, 0);
            return -1;
        }
        for (i = 1; i < words->count; i++) {
            list_add(c, &rest, words->items[i]);
        }
        *words = rest;
    }
    list_add(c, words, 0);
    return 0;
}

/* A function or a builtin, in this process: its redirections are undone after
 * unless this process is a pipeline's child and ends with it. */
static int run_inside(vsh_ctx_t *c, const vsh_node_t *cmd, const func_t *fn,
                      int (*bi)(int, char **), int argc, char **argv, int forked) {
    saved_fd_t saved[VSH_MAX_REDIRS];
    int nsaved = 0, rc;

    if (cmd->assigns) {
        say_err(argv[0], "X=1 is for a program, not a builtin or a function");
        return 1;
    }
    if (apply_redirs(c, cmd->redirs, forked ? 0 : saved, &nsaved) != 0) {
        restore_redirs(saved, forked ? 0 : nsaved);
        return 1;
    }
    rc = fn ? call_func(fn, argc, argv) : bi(argc, argv);
    if (!forked) {
        restore_redirs(saved, nsaved);
    }
    return rc;
}

/* A program in the foreground, as a job of one. */
static int run_program(vsh_ctx_t *c, const vsh_node_t *cmd, char **argv) {
    job_t j;
    pid_t pid = fork();

    if (pid < 0) {
        say_err("fork", strerror(errno));
        return 126;
    }
    if (pid == 0) {
        job_child(0, 1);
        if (apply_redirs(c, cmd->redirs, 0, 0) != 0) {
            _exit(1);
        }
        exec_program(c, cmd, argv);
    }
    memset(&j, 0, sizeof(j));
    j.state = J_RUNNING;
    j.pgid = pid;
    j.own_group = G.jc;
    j.n = 1;
    j.pids[0] = pid;
    job_name(&j, cmd);
    if (G.jc) {
        (void)setpgid(pid, pid);
    }
    return job_finish_foreground(&j);
}

/* `forked`: this is already a child of a pipeline, so a program replaces it. */
static int run_command(const vsh_node_t *cmd, int forked) {
    vsh_arena_t arena;
    vsh_ctx_t c;
    vsh_list_t words = {0, 0, 0};
    const func_t *fn;
    int (*bi)(int, char **);
    char **argv;
    int rc;

    ctx_init(&c, &arena);
    if (command_words(&c, cmd, &arena, &words, &rc) != 0) {
        vsh_arena_free(&arena);
        return rc;
    }
    argv = (char **)words.items;
    fn = find_func(argv[0]);
    bi = fn ? 0 : find_builtin(argv[0]);
    if (fn || bi) {
        rc = run_inside(&c, cmd, fn, bi, (int)words.count - 1, argv, forked);
    } else if (forked) {
        if (apply_redirs(&c, cmd->redirs, 0, 0) != 0) {
            _exit(1);
        }
        exec_program(&c, cmd, argv);
        rc = 127;       /* exec_program does not come back */
    } else {
        rc = run_program(&c, cmd, argv);
    }
    vsh_arena_free(&arena);
    return rc;
}

static int run_pipeline(const vsh_node_t *p) {
    const vsh_node_t *c;
    job_t j;
    int input = -1, result = 0, rc;

    if (!p->a->next) {
        return run_command(p->a, 0);
    }
    memset(&j, 0, sizeof(j));
    j.state = J_RUNNING;
    j.own_group = G.jc;
    job_name(&j, p);
    for (c = p->a; c; c = c->next) {
        int fds[2] = {-1, -1};
        pid_t pid;

        if (j.n == VSH_JOB_PIDS) {
            say_err("pipeline too long", 0);
            result = 1;
            break;
        }
        if (c->next && pipe(fds) != 0) {
            say_err("pipe", strerror(errno));
            result = 1;
            break;
        }
        pid = fork();
        if (pid < 0) {
            say_err("fork", strerror(errno));
            if (c->next) {
                close(fds[0]);
                close(fds[1]);
            }
            result = 126;
            break;
        }
        if (pid == 0) {
            job_child(j.pgid, 1);
            if (input >= 0) {
                dup2(input, 0);
                close(input);
            }
            if (c->next) {
                close(fds[0]);
                dup2(fds[1], 1);
                close(fds[1]);
            }
            G.flow = F_NONE;
            _exit(run_command(c, 1) & 0xff);
        }
        /* From both sides: whichever runs first, the next stage joins a group
         * that exists. */
        if (!j.pgid) {
            j.pgid = pid;
        }
        if (G.jc) {
            (void)setpgid(pid, j.pgid);
        }
        j.pids[j.n++] = pid;
        if (input >= 0) {
            close(input);
        }
        input = -1;
        if (c->next) {
            close(fds[1]);
            input = fds[0];
        }
    }
    if (input >= 0) {
        close(input);
    }
    rc = job_finish_foreground(&j);
    return result ? result : rc;
}

/* *exempt: a nonzero result here does not stop a script. */
static int run_chain(const vsh_node_t *n, int *exempt) {
    int st;

    switch (n->kind) {
        case VSH_N_AND:
            st = run_chain(n->a, exempt);
            if (G.flow) {
                return st;
            }
            if (st != 0) {
                *exempt = 1;       /* `a && b`: asked about a, and answered */
                return st;
            }
            G.status = st;
            return run_chain(n->b, exempt);
        case VSH_N_OR:
            st = run_chain(n->a, exempt);
            if (G.flow || st == 0) {
                return st;
            }
            G.status = st;
            *exempt = 0;
            return run_chain(n->b, exempt);
        default:
            st = run_pipeline(n);
            *exempt = n->flag;
            return st;
    }
}

/* ---- statements ------------------------------------------------------------ */

static int eval_cond(const vsh_node_t *cond, int *out) {
    vsh_arena_t arena;
    vsh_ctx_t c;
    int r;

    ctx_init(&c, &arena);
    G.in_cond++;
    r = vsh_cond(&c, cond, out);
    G.in_cond--;
    if (r != 0) {
        say_err(c.err, 0);
    }
    vsh_arena_free(&arena);
    return r;
}

static int run_assign(const vsh_node_t *n) {
    vsh_arena_t arena;
    vsh_ctx_t c;
    frame_t *f = n->kind == VSH_N_SET && G.top ? G.top : &G.globals;
    var_t *v;
    int rc = 0;

    ctx_init(&c, &arena);
    G.cap_status = 0;
    if (n->kind == VSH_N_EXPORT && !n->flag) {
        v = var_find(&G.globals, n->name);
        if (!v) {
            say_err(n->name, "no such variable");
            rc = 1;
        } else {
            v->exported = 1;
        }
    } else if (n->is_list) {
        vsh_list_t l = {0, 0, 0};

        if (expand_words(&c, n->words, &l) != 0) {
            say_err(c.err, 0);
            rc = 1;
        } else {
            v = var_put(f, n->name, l.items, l.count, 1);
            v->exported |= n->kind == VSH_N_EXPORT;
            rc = G.cap_status;
        }
    } else {
        char *text;

        if (vsh_word_text(&c, n->words, &text) != 0) {
            say_err(c.err, 0);
            rc = 1;
        } else {
            v = var_put(f, n->name, (const char *const *)&text, 1u, 0);
            v->exported |= n->kind == VSH_N_EXPORT;
            rc = G.cap_status;     /* set x = $(false) failed */
        }
    }
    vsh_arena_free(&arena);
    return rc;
}

static void run_loop_body(const vsh_node_t *body, int *stop) {
    G.loops++;
    (void)run_list(body);
    G.loops--;
    if (G.flow == F_BREAK) {
        G.flow = F_NONE;
        *stop = 1;
    } else if (G.flow == F_CONTINUE) {
        G.flow = F_NONE;
    } else if (G.flow) {
        *stop = 1;
    }
}

/* The statements below answer through *ran: a body ran and has already left
 * $? and the flow as they should be, so run_statement adds nothing. */

static int run_if(const vsh_node_t *n, int *ran) {
    int yes = 0;

    if (eval_cond(n->a, &yes) != 0) {
        return 1;
    }
    if (!yes && !n->otherwise) {
        return 0;
    }
    G.status = 0;
    (void)run_list(yes ? n->b : n->otherwise);
    *ran = 1;
    return G.status;
}

static int run_for(const vsh_node_t *n, int *ran) {
    vsh_arena_t arena;
    vsh_ctx_t c;
    vsh_list_t l = {0, 0, 0};
    frame_t *f = G.top ? G.top : &G.globals;
    size_t i;
    int stop = 0, st = 1;

    ctx_init(&c, &arena);
    if (expand_words(&c, n->words, &l) != 0) {
        say_err(c.err, 0);
    } else {
        G.status = 0;
        for (i = 0; i < l.count && !stop; i++) {
            set_scalar(f, n->name, l.items[i]);
            run_loop_body(n->b, &stop);
        }
        *ran = 1;
        st = G.status;
    }
    vsh_arena_free(&arena);
    return st;
}

static int run_while(const vsh_node_t *n, int *ran) {
    int stop = 0;

    G.status = 0;
    while (!stop) {
        int yes = 0;

        if (eval_cond(n->a, &yes) != 0) {
            return 1;
        }
        if (G.flow) {       /* Ctrl+C in the condition's command */
            *ran = 1;
            return G.status;
        }
        if (!yes) {
            break;
        }
        run_loop_body(n->b, &stop);
    }
    if (G.flow == F_NONE && !stop) {
        G.status = 0;
    }
    *ran = 1;
    return G.status;
}

static void define_func(const vsh_node_t *n) {
    func_t *f = find_func(n->name);

    if (!f) {
        f = (func_t *)xmalloc(sizeof(*f));
        f->name = xstrdup(n->name);
        f->next = G.funcs;
        G.funcs = f;
    }
    f->node = n;
}

static int run_return(const vsh_node_t *n, int *ran) {
    int st = G.status;

    if (!G.top) {
        say_err("return outside a function", 0);
        return 1;
    }
    if (n->words) {
        vsh_arena_t arena;
        vsh_ctx_t c;
        char *t;

        ctx_init(&c, &arena);
        if (vsh_word_text(&c, n->words, &t) != 0 || parse_status(t, &st) != 0) {
            say_err("return: not a number", 0);
            st = 2;
        }
        vsh_arena_free(&arena);
    }
    G.status = st;
    G.flow = F_RETURN;
    *ran = 1;
    return st;
}

static int run_jump(const vsh_node_t *n, int *ran) {
    if (!G.loops) {
        say_err(n->kind == VSH_N_BREAK ? "break outside a loop" : "continue outside a loop", 0);
        return 1;
    }
    G.flow = n->kind == VSH_N_BREAK ? F_BREAK : F_CONTINUE;
    *ran = 1;
    return G.status;
}

static int run_background(const vsh_node_t *n) {
    job_t *j = job_slot();
    int was_jc = G.jc;
    pid_t pid;

    if (!j) {
        say_err("too many jobs", 0);
        return 1;
    }
    pid = fork();
    if (pid < 0) {
        say_err("fork", strerror(errno));
        return 126;
    }
    if (pid == 0) {
        int ignored = 0;
        int st;

        job_child(0, 0);
        if (!was_jc) {
            /* No terminal to stop it for reading: it reads nothing, as a
             * script's background command does in every shell. */
            int null = open("/dev/null", O_RDONLY);

            if (null >= 0) {
                dup2(null, 0);
                close(null);
            }
        }
        G.flow = F_NONE;
        G.errexit = 0;
        st = run_chain(n->a, &ignored);
        _exit(st & 0xff);
    }
    memset(j, 0, sizeof(*j));
    j->state = J_RUNNING;
    j->pgid = pid;
    j->own_group = was_jc;
    j->n = 1;
    j->pids[0] = pid;
    j->seq = ++g_job_seq;
    job_name(j, n->a);
    if (was_jc) {
        char line[48];
        int len;

        (void)setpgid(pid, pid);
        len = snprintf(line, sizeof(line), "[%d] %ld\n", job_id(j), (long)pid);
        put_all(1, line, (size_t)len);
    }
    return 0;
}

static void run_statement(const vsh_node_t *n) {
    int exempt = 0, ran = 0, st = 0;

    switch (n->kind) {
        case VSH_N_SET:
        case VSH_N_GLOBAL:
        case VSH_N_EXPORT:
            st = run_assign(n);
            break;
        case VSH_N_ERRORS:
            G.errexit = n->flag;
            break;
        case VSH_N_IF:
            st = run_if(n, &ran);
            break;
        case VSH_N_FOR:
            st = run_for(n, &ran);
            break;
        case VSH_N_WHILE:
            st = run_while(n, &ran);
            break;
        case VSH_N_FUNC:
            define_func(n);
            break;
        case VSH_N_RETURN:
            st = run_return(n, &ran);
            break;
        case VSH_N_BREAK:
        case VSH_N_CONTINUE:
            st = run_jump(n, &ran);
            break;
        case VSH_N_BACKGROUND:
            st = run_background(n);
            break;
        default:
            st = run_chain(n, &exempt);
            break;
    }
    if (ran) {
        return;
    }
    G.status = st;
    /* The first failure ends a script: a condition asked and a `try` said it
     * may fail. */
    if (st != 0 && !exempt && G.errexit && !G.in_cond && G.flow == F_NONE) {
        G.flow = F_EXIT;
    }
}

static int run_list(const vsh_node_t *n) {
    for (; n && !G.flow; n = n->next) {
        run_statement(n);
    }
    return G.status;
}

/* ---- the entry points main.c uses ------------------------------------------ */

static char *read_fd(int fd) {
    vsh_sb_t o = {0, 0, 0};
    char buf[4096];

    vsh_sb_putn(&o, "", 0u);
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));

        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0) {
            vsh_sb_free(&o);
            return 0;
        }
        if (n == 0) {
            return o.b;
        }
        vsh_sb_putn(&o, buf, (size_t)n);
    }
}

static int line_of(const char *src, size_t pos) {
    int line = 1;
    size_t i;

    for (i = 0; i < pos && src[i]; i++) {
        line += src[i] == '\n';
    }
    return line;
}

int vsh_run_text(const char *name, const char *src) {
    char err[128];
    size_t pos = 0;
    /* Lives as long as the shell: a function defined here runs later. */
    char *copy = vsh_strndup(&G.arena, src, strlen(src));
    const vsh_node_t *script = vsh_parse(&G.arena, copy, err, sizeof(err), &pos);

    if (err[0]) {
        char where[64];

        snprintf(where, sizeof(where), "%s:%d", name, line_of(src, pos));
        say_err(where, err);
        G.status = 2;
        if (G.errexit) {
            G.flow = F_EXIT;
        }
        return 2;
    }
    (void)run_list(script);
    return G.status;
}

static int run_file(const char *path) {
    int fd = open(path, O_RDONLY);
    char *text;
    int rc;

    if (fd < 0) {
        say_err(path, strerror(errno));
        return 127;
    }
    text = read_fd(fd);
    close(fd);
    if (!text) {
        say_err(path, "cannot read it");
        return 1;
    }
    rc = vsh_run_text(path, text);
    free(text);
    return rc;
}

int vsh_run_file(const char *path) {
    return run_file(path);
}

int vsh_run_stdin(void) {
    char *text = read_fd(0);
    int rc;

    if (!text) {
        say_err("stdin", "cannot read it");
        return 1;
    }
    rc = vsh_run_text("stdin", text);
    free(text);
    return rc;
}

int vsh_finished(void) {
    return G.flow == F_EXIT;
}

/* A statement's break or return that reached the top ends with it. */
void vsh_next_line(void) {
    G.flow = G.flow == F_EXIT ? F_EXIT : F_NONE;
}

/* Before a prompt: what happened to the jobs while the last line ran. */
void vsh_reap(void) {
    int i;

    jobs_collect();
    for (i = 0; i < VSH_MAX_JOBS; i++) {
        if (g_jobs[i].state == J_DONE) {
            job_line(&g_jobs[i], "Done");
            g_jobs[i].state = J_FREE;
        }
    }
}

/* A shell on a terminal: its own group, in the foreground, deaf to what the
 * keyboard sends the foreground. If it is not in the foreground it waits to be
 * put there, as the terminal's SIGTTIN does to any reader. */
void vsh_job_init(void) {
    pid_t pgid;

    G.tty = 0;
    while ((pgid = getpgrp()) != tcgetpgrp(G.tty)) {
        if (tcgetpgrp(G.tty) < 0) {
            return;     /* no job control on this terminal */
        }
        (void)kill(-pgid, SIGTTIN);
    }
    signal(SIGINT, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);
    (void)setpgid(0, 0);      /* a session leader already leads its group */
    G.shell_pgid = getpgrp();
    if (tcsetpgrp(G.tty, G.shell_pgid) != 0 || tcgetattr(G.tty, &G.tmodes) != 0) {
        return;
    }
    G.jc = 1;
}

/* A stopped job would otherwise outlive its shell, stopped for ever. */
void vsh_job_end(void) {
    int i;

    for (i = 0; i < VSH_MAX_JOBS; i++) {
        if (g_jobs[i].state == J_RUNNING || g_jobs[i].state == J_STOPPED) {
            job_signal(&g_jobs[i], SIGHUP);
            job_signal(&g_jobs[i], SIGCONT);
        }
    }
}

void vsh_init(int stop_on_error, int argc, char **argv) {
    char **e;
    char cwd[PATH_MAX];

    memset(&G, 0, sizeof(G));
    vsh_arena_init(&G.arena);
    G.errexit = stop_on_error;
    for (e = environ; e && *e; e++) {
        const char *eq = strchr(*e, '=');

        if (eq && eq != *e) {
            char *name = vsh_strndup(&G.arena, *e, (size_t)(eq - *e));

            set_scalar(&G.globals, name, eq + 1);
            var_find(&G.globals, name)->exported = 1;
        }
    }
    if (getcwd(cwd, sizeof(cwd))) {
        set_scalar(&G.globals, "CD", cwd);
    }
    (void)var_put(&G.globals, "args", (const char *const *)argv, (size_t)argc, 1);
}
