/* SPDX-License-Identifier: GPL-3.0-or-later */
/* /usr/bin/vsh (docs/shell/s4.md).
 *
 *     vsh                  a terminal: a prompt and a line at a time
 *     vsh script [args]    the file; its arguments are the list $args
 *     vsh -c text [args]   the text
 *
 * The line editor is step 4; until then a terminal's own line discipline edits
 * the line. */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "vsh.h"

int vsh_run_text(const char *name, const char *src);
int vsh_run_file(const char *path);
int vsh_run_stdin(void);
int vsh_finished(void);
void vsh_next_line(void);
void vsh_reap(void);
void vsh_init(int stop_on_error, int argc, char **argv);

static int read_line(vsh_sb_t *out) {
    size_t before = out->n;

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

    for (;;) {
        vsh_sb_t text = {0, 0, 0};

        vsh_reap();
        (void)!write(1, "vsh$ ", 5);
        vsh_sb_putn(&text, "", 0u);
        if (!read_line(&text)) {
            vsh_sb_free(&text);
            (void)!write(1, "\n", 1);
            return status;
        }
        while (is_incomplete(text.b)) {
            (void)!write(1, "> ", 2);
            if (!read_line(&text)) {
                break;
            }
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
        rc = interactive();
    } else {
        vsh_init(1, 0, argv + 1);
        rc = vsh_run_stdin();
    }
    return rc & 0xff;
}
