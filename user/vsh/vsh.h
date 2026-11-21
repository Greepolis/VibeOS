/* SPDX-License-Identifier: GPL-3.0-or-later */
/* vsh's language: the parser, and what a word and a condition evaluate to
 * (docs/shell/s4.md). Portable and without a process in it: running a command
 * is the caller's, through vsh_ctx_t, which is what lets the host test it. */
#ifndef VIBEOS_VSH_H
#define VIBEOS_VSH_H

#include <stddef.h>
#include <stdint.h>

/* Bounds on what a text can make the parser and the evaluators recurse over.
 * A script is input: a thousand nested $( must be a refusal, not a stack
 * overflow. A chain is a left-leaning tree, so it counts too. */
#define VSH_MAX_DEPTH 64
#define VSH_MAX_CHAIN 256

/* ---- memory: everything of a parse lives and dies together ---------------- */

typedef struct vsh_chunk vsh_chunk_t;
typedef struct {
    vsh_chunk_t *head;
} vsh_arena_t;

void vsh_arena_init(vsh_arena_t *a);
void vsh_arena_free(vsh_arena_t *a);
/* Zeroed. Out of memory ends the shell with a message: there is no state a
 * half-built tree could usefully be returned in. */
void *vsh_alloc(vsh_arena_t *a, size_t n);
char *vsh_strndup(vsh_arena_t *a, const char *s, size_t n);

typedef struct {
    char *b;
    size_t n, cap;
} vsh_sb_t;

void vsh_sb_putn(vsh_sb_t *s, const char *t, size_t n);
void vsh_sb_puts(vsh_sb_t *s, const char *t);
void vsh_sb_putc(vsh_sb_t *s, char c);
void vsh_sb_free(vsh_sb_t *s);

/* ---- the tree -------------------------------------------------------------- */

typedef enum {
    VSH_P_LIT,      /* text */
    VSH_P_VAR,      /* $name, ${name}, ${name[index]} */
    VSH_P_PERCENT,  /* %name%: the variable if it exists, these bytes if not */
    VSH_P_CMD,      /* $(script) */
    VSH_P_ARITH,    /* $((text)) */
    VSH_P_STATUS    /* $? */
} vsh_part_kind_t;

typedef struct vsh_node vsh_node_t;

typedef struct vsh_part {
    vsh_part_kind_t kind;
    int quoted;
    const char *text;     /* the literal, the name, or the arithmetic */
    long index;           /* -1: the whole variable */
    vsh_node_t *script;
    struct vsh_part *next;
} vsh_part_t;

typedef struct vsh_word {
    vsh_part_t *parts, *tail;
    struct vsh_word *next;
} vsh_word_t;

typedef enum { VSH_R_OUT, VSH_R_APPEND, VSH_R_IN, VSH_R_DUP } vsh_redir_kind_t;

typedef struct vsh_redir {
    vsh_redir_kind_t kind;
    int fd;
    int dup_to;           /* VSH_R_DUP */
    vsh_word_t *target;
    struct vsh_redir *next;
} vsh_redir_t;

typedef struct vsh_assign {
    const char *name;
    vsh_word_t *value;
    struct vsh_assign *next;
} vsh_assign_t;

typedef enum {
    VSH_N_SET, VSH_N_GLOBAL, VSH_N_EXPORT, VSH_N_ERRORS,
    VSH_N_IF, VSH_N_FOR, VSH_N_WHILE, VSH_N_FUNC,
    VSH_N_RETURN, VSH_N_BREAK, VSH_N_CONTINUE,
    VSH_N_BACKGROUND, VSH_N_AND, VSH_N_OR, VSH_N_PIPELINE, VSH_N_COMMAND,
    /* conditions; VSH_N_AND and VSH_N_OR serve them too */
    VSH_N_NOT, VSH_N_CMP, VSH_N_TRUTH, VSH_N_CONDCMD
} vsh_node_kind_t;

typedef enum {
    VSH_C_EQ, VSH_C_NE, VSH_C_LT, VSH_C_LE, VSH_C_GT, VSH_C_GE,
    VSH_C_MATCH, VSH_C_IMATCH
} vsh_cmp_t;

struct vsh_node {
    vsh_node_kind_t kind;
    const char *name;     /* set, global, export, for, func */
    vsh_word_t *words;    /* value, for's list, func's parameters, a command's
                           * words, return's word, a comparison's two sides */
    int is_list;          /* set: the value was ( ... ) */
    int flag;             /* errors: on; pipeline: try; export: has a value */
    vsh_cmp_t cmp;
    vsh_assign_t *assigns;
    vsh_redir_t *redirs;
    vsh_node_t *a, *b;    /* condition and body; two sides; a pipeline's first */
    vsh_node_t *otherwise;
    vsh_node_t *next;     /* the next statement, or the pipeline's next stage */
};

/* The script, or 0 with `err` saying what and *errpos where. An empty script is
 * 0 with an empty err. */
vsh_node_t *vsh_parse(vsh_arena_t *a, const char *src, char *err, size_t errcap, size_t *errpos);

/* The tree as text, for the tests: one spelling for one meaning. */
void vsh_dump(const vsh_node_t *script, vsh_sb_t *out);

/* ---- evaluating -------------------------------------------------------------- */

typedef struct {
    size_t count;
    const char *const *items;
    int is_list;
} vsh_value_t;

typedef struct {
    vsh_arena_t *arena;   /* what an expansion returns lives here */
    void *user;
    int status;           /* $? */
    /* 0 and the value, or nonzero if there is no such variable. */
    int (*lookup)(void *user, const char *name, vsh_value_t *out);
    /* $(...): 0 and what it printed. */
    int (*capture)(void *user, const vsh_node_t *script, char **out);
    /* A command used as a condition: 0 and its exit status. */
    int (*run)(void *user, const vsh_node_t *chain, int *status);
    char err[128];
} vsh_ctx_t;

typedef struct {
    const char **items;
    size_t count, cap;
} vsh_list_t;

/* Appends what the word expands to: one word, or a whole unquoted list's. */
int vsh_expand_word(vsh_ctx_t *c, const vsh_word_t *w, vsh_list_t *out);
/* One string whatever the word holds; a list is joined by spaces. */
int vsh_word_text(vsh_ctx_t *c, const vsh_word_t *w, char **out);
/* The word as a file pattern: only what was written unquoted can act as one,
 * so a variable holding `*` names a file called `*`. */
int vsh_word_glob(vsh_ctx_t *c, const vsh_word_t *w, char **out);
int vsh_arith(vsh_ctx_t *c, const char *text, int64_t *out);
int vsh_cond(vsh_ctx_t *c, const vsh_node_t *cond, int *out);
/* * ? [a-z] [!a] and \x for a literal x. */
int vsh_match(const char *pattern, const char *text, int ignore_case);

#endif
