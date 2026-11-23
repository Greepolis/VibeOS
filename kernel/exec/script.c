/* SPDX-License-Identifier: GPL-3.0-or-later */
/* See vibeos/script.h. No libc: this is linked into the freestanding image. */

#include "vibeos/script.h"

static int is_blank(char c) {
    return c == ' ' || c == '\t';
}

int vibeos_script_parse(const char *head, size_t n, char *interp, size_t icap,
                        char *arg, size_t acap) {
    size_t end = 0, i, k, last;
    int whole;

    if (n < 2u || head[0] != '#' || head[1] != '!') {
        return 0;
    }
    whole = n <= VIBEOS_SCRIPT_LINE;
    if (!whole) {
        n = VIBEOS_SCRIPT_LINE;
    }
    while (end < n && head[end] != '\n' && head[end] != 0) {
        end++;
    }
    /* A line cut by the limit is usable only if the name ended before the cut:
     * running a truncated path would run some other program. */
    whole = whole || end < n;
    i = 2u;
    while (i < end && is_blank(head[i])) {
        i++;
    }
    k = 0;
    while (i < end && !is_blank(head[i])) {
        if (k + 1u >= icap) {
            return -1;
        }
        interp[k++] = head[i++];
    }
    interp[k] = 0;
    if (k == 0u || (i == end && !whole)) {
        return -1;
    }
    while (i < end && is_blank(head[i])) {
        i++;
    }
    last = end;
    while (last > i && (is_blank(head[last - 1u]) || head[last - 1u] == '\r')) {
        last--;
    }
    /* A file saved on Windows: the CR is not part of the interpreter's name. */
    if (interp[k - 1u] == '\r') {
        interp[k - 1u] = 0;
        if (k == 1u) {
            return -1;
        }
    }
    k = 0;
    while (i < last) {
        if (k + 1u >= acap) {
            return -1;
        }
        arg[k++] = head[i++];
    }
    arg[k] = 0;
    return 1;
}

static char lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

int vibeos_binfmt_find(const char *table, size_t n, const char *path, char *interp, size_t icap) {
    const char *ext = 0, *p;
    size_t at = 0, elen;

    /* The last dot of the last component, and not its first character: `.bashrc`
     * has no extension. */
    for (p = path; *p; p++) {
        if (*p == '/') {
            ext = 0;
        } else if (*p == '.' && p > path && p[-1] != '/') {
            ext = p;
        }
    }
    if (!ext || !ext[1]) {
        return 0;
    }
    elen = (size_t)(p - ext);
    while (at < n) {
        size_t line = at, end = at, k = 0, w;

        while (end < n && table[end] != '\n') {
            end++;
        }
        at = end + 1u;
        while (line < end && is_blank(table[line])) {
            line++;
        }
        if (line == end || table[line] != '.') {
            continue;       /* a blank line, a comment, or not an entry */
        }
        w = line;
        while (w < end && !is_blank(table[w])) {
            w++;
        }
        if (w - line != elen) {
            continue;
        }
        while (k < elen && lower(table[line + k]) == lower(ext[k])) {
            k++;
        }
        if (k != elen) {
            continue;
        }
        while (w < end && is_blank(table[w])) {
            w++;
        }
        while (end > w && (is_blank(table[end - 1u]) || table[end - 1u] == '\r')) {
            end--;
        }
        if (w == end || end - w + 1u > icap) {
            return 0;
        }
        for (k = 0; w + k < end; k++) {
            interp[k] = table[w + k];
        }
        interp[k] = 0;
        return 1;
    }
    return 0;
}
