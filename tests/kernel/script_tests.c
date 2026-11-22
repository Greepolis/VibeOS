/* SPDX-License-Identifier: GPL-3.0-or-later */
/* What runs a script (vibeos/script.h). The `#!` rules are Linux's, and
 * tests/linux/musl_script.c holds the same answers against a real Linux. */

#include <stdio.h>
#include <string.h>

#include "vibeos/script.h"

int test_script(void);

#define NEED(cond, what)                     \
    do {                                     \
        if (!(cond)) {                       \
            printf("FAIL:script %s\n", what); \
            return 1;                        \
        }                                    \
    } while (0)

static int parsed(const char *s, int want, const char *wi, const char *wa) {
    char i[64], a[64];
    int r = vibeos_script_parse(s, strlen(s), i, sizeof(i), a, sizeof(a));

    return r == want && (want != 1 || (strcmp(i, wi) == 0 && strcmp(a, wa) == 0));
}

static int found(const char *table, const char *path, const char *want) {
    char i[32];
    int r = vibeos_binfmt_find(table, strlen(table), path, i, sizeof(i));

    return want ? (r == 1 && strcmp(i, want) == 0) : r == 0;
}

int test_script(void) {
    static const char t[] = "# comment\n\n.sh /bin/sh\n  .VSH\t/usr/bin/vsh  \r\n.bat /c\n.none\nbad /x\n";
    char line[400], i[64], a[64];

    NEED(parsed("#!/bin/sh\necho", 1, "/bin/sh", ""), "plain");
    NEED(parsed("#!/bin/sh", 1, "/bin/sh", ""), "no newline");
    NEED(parsed("#!  \t/bin/sh\n", 1, "/bin/sh", ""), "blanks first");
    NEED(parsed("#!/bin/sh -e\n", 1, "/bin/sh", "-e"), "one argument");
    NEED(parsed("#!/usr/bin/env a b  c  \n", 1, "/usr/bin/env", "a b  c"), "the rest is one argument");
    NEED(parsed("#!/bin/sh\r\n", 1, "/bin/sh", ""), "a Windows line ending");
    NEED(parsed("#!/bin/sh -e\r\n", 1, "/bin/sh", "-e"), "and after an argument");
    NEED(parsed("echo\n", 0, "", ""), "no mark");
    NEED(parsed("#", 0, "", ""), "half a mark");
    NEED(parsed("#!\n", -1, "", ""), "a mark naming nothing");
    NEED(parsed("#!   \n", -1, "", ""), "only blanks");

    /* Cut by the limit inside the name: not a shorter path. */
    memset(line, 'x', sizeof(line));
    line[0] = '#';
    line[1] = '!';
    NEED(vibeos_script_parse(line, sizeof(line), i, sizeof(i), a, sizeof(a)) == -1, "a name too long");
    {
        char big[VIBEOS_SCRIPT_LINE + 8u];

        memset(big, 'x', sizeof(big));
        memcpy(big, "#!/i ", 5);
        NEED(vibeos_script_parse(big, sizeof(big), i, sizeof(i), line, sizeof(line)) == 1 &&
             strcmp(i, "/i") == 0 && strlen(line) == VIBEOS_SCRIPT_LINE - 5u,
             "an argument cut by the limit is still the interpreter's");
    }

    {
        /* Room for the whole name: only the limit can refuse it. */
        static char roomy[VIBEOS_SCRIPT_LINE + 16u], rest[16];

        memset(line, 'x', sizeof(line));    /* the case above wrote its argument here */
        line[0] = '#';
        line[1] = '!';
        NEED(vibeos_script_parse(line, sizeof(line), roomy, sizeof(roomy), rest, sizeof(rest)) == -1,
             "a name cut by the limit is not a shorter path");
    }
    NEED(found(".sh/file /x\n", "/d.sh/file", 0), "a directory's dot is not an extension");
    NEED(found(t, "/a/b.sh", "/bin/sh"), "by extension");
    NEED(found(t, "x.SH", "/bin/sh"), "ignoring case");
    NEED(found(t, "/a/x.vsh", "/usr/bin/vsh"), "table's case and blanks");
    NEED(found(t, "a.b.bat", "/c"), "the last dot");
    NEED(found(t, "a.sh.txt", 0), "not an inner extension");
    NEED(found(t, "/d.sh/file", 0), "a directory's dot");
    NEED(found(t, "/home/.sh", 0), "a hidden file has no extension");
    NEED(found(t, "noext", 0), "no extension");
    NEED(found(t, "x.", 0), "an empty extension");
    NEED(found(t, "x.none", 0), "an entry with no interpreter");
    NEED(found(t, "x.s", 0), "a prefix of an extension");
    NEED(found(t, "x.shh", 0), "an extension longer than the entry");
    {
        char tiny[4];

        NEED(vibeos_binfmt_find(t, strlen(t), "x.vsh", tiny, sizeof(tiny)) == 0, "a path that does not fit");
    }
    return 0;
}
