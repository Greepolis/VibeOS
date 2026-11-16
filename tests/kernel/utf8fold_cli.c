/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The kernel's name folding, one name per line of hex on stdin, its folded
 * form in hex on stdout ("-" when it does not fold): what
 * scripts/dev/verify-utf8fold.py holds against Python's own normalisation
 * (E4-030). */

#include <stdio.h>
#include <string.h>

#include "vibeos/utf8fold.h"

int main(void) {
    static char line[4096];
    static char name[2048];
    static uint8_t out[8192];

    while (fgets(line, sizeof(line), stdin)) {
        size_t n = 0, i;
        int32_t r;

        for (i = 0; line[i] && line[i + 1] && line[i] != '\n' && n < sizeof(name); i += 2) {
            unsigned b;

            sscanf(line + i, "%2x", &b);
            name[n++] = (char)b;
        }
        r = vibeos_utf8_fold(name, (uint32_t)n, out, sizeof(out));
        if (r < 0) {
            printf("-\n");
            continue;
        }
        for (i = 0; i < (size_t)r; i++) {
            printf("%02x", out[i]);
        }
        printf("\n");
    }
    return 0;
}
