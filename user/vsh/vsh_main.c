/* SPDX-License-Identifier: GPL-3.0-or-later */
/* /usr/bin/vsh (docs/shell/s4.md).
 *
 *     vsh                  a terminal: a prompt and a line at a time
 *     vsh script [args]    the file; its arguments are the list $args
 *     vsh -c text [args]   the text
 *
 * On a terminal that can move its cursor the line is vsh_edit.c's; on TERM=dumb
 * or an unset TERM the terminal's own line discipline edits it. */

#define _GNU_SOURCE 1

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include "vsh.h"
#include "vsh_edit.h"

int vsh_run_text(const char *name, const char *src);
int vsh_run_file(const char *path);
int vsh_run_stdin(void);
int vsh_finished(void);
void vsh_next_line(void);
void vsh_reap(void);
void vsh_job_init(void);
void vsh_job_end(void);
void vsh_init(int stop_on_error, int argc, char **argv);
const char *vsh_builtin_name(size_t i);

static vsh_hist_t g_hist;
static char g_hist_path[PATH_MAX];

/* ---- history, kept between sessions ------------------------------------------- */

/* Commands can hold things their owner would not show: 0600, and never through
 * a link somebody else left at that name. No HOME, no file. */
static void hist_open(void) {
    const char *home = getenv("HOME");
    FILE *f;
    char *line = 0;
    size_t cap = 0;
    ssize_t n;
    int fd;

    if (!home || !home[0] ||
        snprintf(g_hist_path, sizeof(g_hist_path), "%s/.vsh_history", home) >= (int)sizeof(g_hist_path)) {
        g_hist_path[0] = 0;
        return;
    }
    fd = open(g_hist_path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0 || !(f = fdopen(fd, "r"))) {
        if (fd >= 0) {
            close(fd);
        }
        return;
    }
    while ((n = getline(&line, &cap, f)) > 0) {
        if (line[n - 1] == '\n') {
            line[n - 1] = 0;
        }
        (void)vsh_hist_add(&g_hist, line);
    }
    free(line);
    fclose(f);
}

static void hist_keep(const char *line) {
    int fd;
    size_t n = strlen(line);

    if (!vsh_hist_add(&g_hist, line) || !g_hist_path[0]) {
        return;
    }
    fd = open(g_hist_path, O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd >= 0) {
        (void)!write(fd, line, n);
        (void)!write(fd, "\n", 1);
        close(fd);
    }
}

/* history        what is remembered, numbered
 * history -c     forget it, here and in the file */
int vsh_history_cmd(int argc, char **argv) {
    size_t i;

    if (argc > 1 && strcmp(argv[1], "-c") == 0) {
        vsh_hist_clear(&g_hist);
        if (g_hist_path[0] && unlink(g_hist_path) != 0 && errno != ENOENT) {
            dprintf(2, "vsh: history: %s\n", strerror(errno));
            return 1;
        }
        return 0;
    }
    if (argc > 1) {
        dprintf(2, "vsh: history: only -c is an option\n");
        return 2;
    }
    for (i = 0; i < g_hist.n; i++) {
        dprintf(1, "%5zu  %s\n", i + 1u, g_hist.items[i]);
    }
    return 0;
}

/* ---- Tab ------------------------------------------------------------------------ */

static void complete_files(const char *prefix, int only_programs, vsh_names_t *out) {
    const char *slash = strrchr(prefix, '/');
    size_t dirlen = slash ? (size_t)(slash - prefix) + 1u : 0u;
    const char *base = prefix + dirlen;
    size_t baselen = strlen(base);
    char dir[PATH_MAX];
    DIR *d;
    struct dirent *ent;

    if (dirlen >= sizeof(dir)) {
        return;
    }
    memcpy(dir, prefix, dirlen);
    dir[dirlen] = 0;
    d = opendir(dirlen ? dir : ".");
    if (!d) {
        return;
    }
    while ((ent = readdir(d)) != 0) {
        char full[PATH_MAX + 2];
        struct stat st;
        int isdir;

        if (strncmp(ent->d_name, base, baselen) != 0 ||
            (ent->d_name[0] == '.' && base[0] != '.') ||
            strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        if (snprintf(full, sizeof(full), "%s%s", dir, ent->d_name) >= PATH_MAX) {
            continue;
        }
        isdir = stat(dirlen ? full : ent->d_name, &st) == 0 && S_ISDIR(st.st_mode);
        if (only_programs && !isdir && access(dirlen ? full : ent->d_name, X_OK) != 0) {
            continue;
        }
        if (isdir) {
            strcat(full, "/");
        }
        vsh_names_add(out, full);
    }
    closedir(d);
}

static void complete_programs(const char *prefix, vsh_names_t *out) {
    const char *path = getenv("PATH");
    size_t n = strlen(prefix), i;
    const char *name;

    for (i = 0; (name = vsh_builtin_name(i)) != 0; i++) {
        if (strncmp(name, prefix, n) == 0) {
            vsh_names_add(out, name);
        }
    }
    while (path && *path) {
        const char *end = strchr(path, ':');
        size_t len = end ? (size_t)(end - path) : strlen(path);
        char dir[PATH_MAX];

        if (len > 0u && len < sizeof(dir)) {
            DIR *d;

            memcpy(dir, path, len);
            dir[len] = 0;
            if ((d = opendir(dir)) != 0) {
                struct dirent *ent;

                while ((ent = readdir(d)) != 0) {
                    char full[PATH_MAX * 2];
                    struct stat st;

                    if (ent->d_name[0] == '.' || strncmp(ent->d_name, prefix, n) != 0) {
                        continue;
                    }
                    snprintf(full, sizeof(full), "%s/%s", dir, ent->d_name);
                    if (stat(full, &st) == 0 && S_ISREG(st.st_mode) && access(full, X_OK) == 0) {
                        vsh_names_add(out, ent->d_name);
                    }
                }
                closedir(d);
            }
        }
        path = end ? end + 1 : 0;
    }
}

static void complete(void *user, const char *prefix, int first_word, vsh_names_t *out) {
    (void)user;
    if (first_word && !strchr(prefix, '/')) {
        complete_programs(prefix, out);
    } else {
        complete_files(prefix, first_word, out);
    }
}

/* ---- reading a line --------------------------------------------------------------- */

static void put_all(const char *s, size_t n) {
    while (n > 0u) {
        ssize_t w = write(1, s, n);

        if (w < 0 && errno == EINTR) {
            continue;
        }
        if (w <= 0) {
            return;
        }
        s += w;
        n -= (size_t)w;
    }
}

static int can_edit(void) {
    const char *term = getenv("TERM");

    return isatty(0) && isatty(1) && term && term[0] && strcmp(term, "dumb") != 0;
}

static size_t columns(void) {
    struct winsize w;

    return ioctl(1, TIOCGWINSZ, &w) == 0 && w.ws_col >= 8 ? w.ws_col : 80u;
}

/* 1 and the line with its newline, 0 at end of input, -1 for Ctrl+C. The
 * terminal is the editor's only while it reads: a program started from the line
 * has to find it as it always was. */
static int edit_line(const char *prompt, vsh_sb_t *out) {
    struct termios cooked, raw;
    vsh_edit_t e;
    int rc = 0;

    if (tcgetattr(0, &cooked) != 0) {
        return 0;
    }
    raw = cooked;
    raw.c_lflag &= ~(tcflag_t)(ECHO | ICANON | ISIG | IEXTEN);
    raw.c_iflag &= ~(tcflag_t)(IXON | ICRNL | INLCR | IGNCR);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(0, TCSADRAIN, &raw) != 0) {
        return 0;
    }
    vsh_edit_begin(&e, prompt, &g_hist, columns(), complete, 0);
    for (;;) {
        unsigned char c;
        ssize_t n;
        vsh_ed_result_t r;

        put_all(e.out.b, e.out.n);
        e.out.n = 0;
        if (e.esc == 1) {
            struct pollfd p = {0, POLLIN, 0};

            /* A sequence arrives whole; nothing after a moment is the key. */
            if (poll(&p, 1, 50) == 0) {
                vsh_edit_esc_alone(&e);
            }
        }
        n = read(0, &c, 1);
        if (n < 0 && errno == EINTR) {
            e.cols = columns();     /* SIGWINCH is what usually lands here */
            continue;
        }
        if (n <= 0) {
            rc = e.len > 0u;
            if (rc) {
                vsh_sb_putn(out, e.b, e.len);
                vsh_sb_putc(out, '\n');
            }
            put_all("\r\n", 2u);
            break;
        }
        r = vsh_edit_feed(&e, c);
        if (r == VSH_ED_MORE) {
            continue;
        }
        put_all(e.out.b, e.out.n);
        if (r == VSH_ED_LINE) {
            hist_keep(e.b);
            vsh_sb_putn(out, e.b, e.len);
            vsh_sb_putc(out, '\n');
            rc = 1;
        } else if (r == VSH_ED_CANCEL) {
            rc = -1;
        } else {
            put_all("\r\n", 2u);
        }
        break;
    }
    vsh_edit_end(&e);
    (void)tcsetattr(0, TCSADRAIN, &cooked);
    return rc;
}

static int plain_line(const char *prompt, vsh_sb_t *out) {
    size_t before = out->n;

    put_all(prompt, strlen(prompt));
    for (;;) {
        char c;
        ssize_t n = read(0, &c, 1);

        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return out->n > before;
        }
        vsh_sb_putc(out, c);
        if (c == '\n') {
            return 1;
        }
    }
}

/* Text that only lacks its end goes on to the next line. */
static int is_incomplete(const char *text) {
    vsh_arena_t a;
    char err[128];
    size_t pos = 0;
    int more;

    vsh_arena_init(&a);
    (void)vsh_parse(&a, text, err, sizeof(err), &pos);
    more = err[0] && text[pos] == 0 && strncmp(err, "missing", 7) == 0;
    vsh_arena_free(&a);
    return more;
}

static int interactive(void) {
    int status = 0;
    int edit = can_edit();
    const char *prompt = geteuid() == 0 ? "vsh# " : "vsh$ ";

    if (edit) {
        hist_open();
    }
    for (;;) {
        vsh_sb_t text = {0, 0, 0};
        int got;

        vsh_reap();
        vsh_sb_putn(&text, "", 0u);
        got = edit ? edit_line(prompt, &text) : plain_line(prompt, &text);
        if (got < 0) {
            vsh_sb_free(&text);
            continue;
        }
        if (got == 0) {
            vsh_sb_free(&text);
            if (!edit) {
                put_all("\n", 1u);
            }
            return status;
        }
        while (got > 0 && is_incomplete(text.b)) {
            got = edit ? edit_line("> ", &text) : plain_line("> ", &text);
        }
        if (got < 0) {
            vsh_sb_free(&text);
            continue;
        }
        status = vsh_run_text("line", text.b);
        vsh_sb_free(&text);
        if (vsh_finished()) {
            return status;
        }
        vsh_next_line();
    }
}

int main(int argc, char **argv) {
    int rc;

    if (argc > 2 && strcmp(argv[1], "-c") == 0) {
        vsh_init(1, argc - 3, argv + 3);
        rc = vsh_run_text("-c", argv[2]);
    } else if (argc > 1) {
        vsh_init(1, argc - 2, argv + 2);
        rc = vsh_run_file(argv[1]);
    } else if (isatty(0)) {
        vsh_init(0, 0, argv + 1);
        vsh_job_init();
        rc = interactive();
        vsh_job_end();
    } else {
        vsh_init(1, 0, argv + 1);
        rc = vsh_run_stdin();
    }
    return rc & 0xff;
}
