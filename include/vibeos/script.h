/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef VIBEOS_SCRIPT_H
#define VIBEOS_SCRIPT_H

/* What runs a file that is not a program (docs/shell/s5.md): its `#!` line, or
 * failing that the table in /etc/vibeos/binfmt, by extension. Pure functions
 * over bytes the caller read, so every personality's exec can ask them. */

#include <stddef.h>

#define VIBEOS_SCRIPT_LINE 256u   /* how much of a `#!` line is read, as Linux */
#define VIBEOS_SCRIPT_DEPTH 4u    /* a script's interpreter may be a script */

/* 1: a `#!` line, with its interpreter and its one optional argument (all that
 * follows, spaces included, as Linux has it; "" if none). 0: no `#!`.
 * -1: a `#!` that names nothing, or a name too long to be whole. */
int vibeos_script_parse(const char *head, size_t n, char *interp, size_t icap,
                        char *arg, size_t acap);

/* The table's interpreter for `path`'s extension, compared ignoring case. Lines
 * are `.ext /path/to/interpreter`; `#` starts a comment. 1 if found, else 0. */
int vibeos_binfmt_find(const char *table, size_t n, const char *path, char *interp, size_t icap);

#endif
