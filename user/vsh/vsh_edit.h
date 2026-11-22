/* SPDX-License-Identifier: GPL-3.0-or-later */
/* vsh's line editor (docs/shell/s4.md step 4). Bytes in, a state and the bytes
 * to print out: no terminal and no file in it, which is what lets the host test
 * drive it key by key against a model of a screen. */
#ifndef VIBEOS_VSH_EDIT_H
#define VIBEOS_VSH_EDIT_H

#include <stddef.h>

#include "vsh.h"

#define VSH_HIST_MAX 1000u

typedef struct {
    char *items[VSH_HIST_MAX];
    size_t n;
} vsh_hist_t;

/* Not an empty line, and not the line before it again. 1 if it was kept. */
int vsh_hist_add(vsh_hist_t *h, const char *line);
void vsh_hist_clear(vsh_hist_t *h);

typedef struct {
    char **items;
    size_t n, cap;
} vsh_names_t;

void vsh_names_add(vsh_names_t *l, const char *name);

/* What could follow `prefix`: whole names, a directory ending in '/'. */
typedef void (*vsh_complete_fn)(void *user, const char *prefix, int first_word, vsh_names_t *out);

typedef enum {
    VSH_ED_MORE,     /* keep feeding */
    VSH_ED_LINE,     /* Enter: the line is in `b` */
    VSH_ED_CANCEL,   /* Ctrl+C */
    VSH_ED_EOF       /* Ctrl+D on an empty line */
} vsh_ed_result_t;

typedef struct {
    const char *prompt;
    char *b;
    size_t len, cap, pos;
    size_t cols;
    size_t cursor_row;      /* rows below the prompt's first the cursor is on */
    vsh_sb_t out;           /* for the terminal; the caller writes and empties it */

    vsh_hist_t *hist;
    size_t hist_at;         /* hist->n: the line being typed */
    char *draft;            /* that line, while history is showing */

    int searching;
    char query[128];
    size_t query_len;
    size_t found;           /* the history entry showing; hist->n: none */

    int esc;                /* 0, 1 after ESC, 2 inside a sequence */
    char seq[16];
    size_t seq_len;

    int tabbed;             /* the last key was a Tab that could not finish */
    vsh_complete_fn complete;
    void *complete_user;
} vsh_edit_t;

/* Starts a line and prints the prompt. */
void vsh_edit_begin(vsh_edit_t *e, const char *prompt, vsh_hist_t *hist, size_t cols,
                    vsh_complete_fn complete, void *user);
vsh_ed_result_t vsh_edit_feed(vsh_edit_t *e, unsigned char c);
/* Nothing followed an ESC: it was the key itself. */
void vsh_edit_esc_alone(vsh_edit_t *e);
void vsh_edit_end(vsh_edit_t *e);

#endif
