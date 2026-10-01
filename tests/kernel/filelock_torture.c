/* Randomised torture for the file-lock table (kernel/fs/filelock.c, docs/abi/
 * L1 step 6), checked against a model that shares nothing with it.
 *
 *     vibeos_filelock_torture <seed> <rounds>
 *
 * The table keeps ranges: it splits one when the middle is unlocked, joins two
 * when the gap between them is filled, and decides a conflict by comparing
 * ends. Every one of those is arithmetic on boundaries, which is where such
 * code is wrong. The model has no ranges at all. A file is seventeen cells -
 * bytes 0 to 15, and one cell for "everything after" - and each owner has, per
 * cell, nothing, a shared lock or an exclusive one. A request is refused if any
 * cell it covers holds something of another owner's it cannot coexist with,
 * and otherwise it is painted on.
 *
 * Checked every round: the answer to the request (and that the owner named as
 * being in the way really is); the answer to a probe of every cell for every
 * owner and both types; and the number of locks the table holds, which has to
 * be the number of runs of equal cells in the model - one more means a join
 * was missed, one fewer means two locks became one that should not have.
 *
 * Two files and both kinds of lock run side by side, so a request that leaked
 * from one into the other shows up as a wrong answer in the other. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vibeos/filelock.h"
#include "vibeos/abi_linux.h"

#define CELLS 17          /* 0..15, and 16 = everything after 15 */
#define OWNERS 3
#define FILES 2
#define SPACES 2

static uint8_t g_m[SPACES][FILES][OWNERS][CELLS];
static uint64_t g_seed;
static unsigned g_round;
static int g_fs;          /* one filesystem: its address is its name */

static uint32_t rnd(void) {
    g_seed = g_seed * 6364136223846793005ull + 1442695040888963407ull;
    return (uint32_t)(g_seed >> 33);
}

static void die(const char *what) {
    printf("FAIL:filelock_torture round %u: %s\n", g_round, what);
    exit(1);
}

static void nolock(void) {
}

static uint64_t owner_id(int o) {
    return VIBEOS_FLK_OWNER_PROC(100 + o);
}

/* The first and last cell a range covers. */
static void cells(uint64_t s, uint64_t e, int *c0, int *c1) {
    *c0 = (int)s;
    *c1 = e == VIBEOS_FLK_END ? CELLS - 1 : (int)e;
}

static int m_conflict(int sp, int f, int o, uint32_t type, int c0, int c1, int *who) {
    int p, c;
    for (p = 0; p < OWNERS; p++) {
        if (p == o) {
            continue;
        }
        for (c = c0; c <= c1; c++) {
            uint8_t have = g_m[sp][f][p][c];
            if (have && (type == VIBEOS_FLK_EXCL || have == VIBEOS_FLK_EXCL)) {
                if (who) {
                    *who = p;
                }
                return 1;
            }
        }
    }
    return 0;
}

static uint32_t m_count(void) {
    uint32_t n = 0;
    int sp, f, o, c;
    for (sp = 0; sp < SPACES; sp++) {
        for (f = 0; f < FILES; f++) {
            for (o = 0; o < OWNERS; o++) {
                for (c = 0; c < CELLS; c++) {
                    uint8_t t = g_m[sp][f][o][c];
                    if (t && (c == 0 || g_m[sp][f][o][c - 1] != t)) {
                        n++;
                    }
                }
            }
        }
    }
    return n;
}

static void check_all(void) {
    int sp, f, o, c;
    uint32_t type;

    if (vibeos_flk_count() != m_count()) {
        printf("  table holds %u locks, the model %u runs\n", vibeos_flk_count(), m_count());
        die("the number of locks is not the number of runs");
    }
    for (sp = 0; sp < SPACES; sp++) {
        for (f = 0; f < FILES; f++) {
            for (o = 0; o < OWNERS; o++) {
                for (c = 0; c < CELLS; c++) {
                    for (type = VIBEOS_FLK_SHARED; type <= VIBEOS_FLK_EXCL; type++) {
                        /* The last cell is probed far out: "everything after". */
                        uint64_t at = c == CELLS - 1 ? 1000000ull + (rnd() & 0xFFFFu) : (uint64_t)c;
                        vibeos_flk_info_t info;
                        int want = m_conflict(sp, f, o, type, c, c, 0);
                        int got = vibeos_flk_test((uint32_t)sp, &g_fs, (uint64_t)f + 1u, owner_id(o),
                                                  type, at, at, &info);
                        if (got != want) {
                            printf("  space %d file %d owner %d cell %d type %u: table %d, model %d\n",
                                   sp, f, o, c, type, got, want);
                            die("a probe disagrees with the model");
                        }
                        if (got && (info.start > at || info.end < at || info.owner == owner_id(o))) {
                            die("the lock reported in the way does not cover the byte, or is the asker's");
                        }
                    }
                }
            }
        }
    }
}

int main(int argc, char **argv) {
    unsigned rounds = argc > 2 ? (unsigned)strtoul(argv[2], 0, 10) : 2000u;

    g_seed = argc > 1 ? strtoull(argv[1], 0, 10) : 1u;
    printf("filelock_torture seed=%llu rounds=%u\n", (unsigned long long)g_seed, rounds);
    g_seed = g_seed * 2654435761ull + 12345u;
    setvbuf(stdout, 0, _IONBF, 0);
    vibeos_flk_set_lock(nolock, nolock);
    vibeos_flk_reset();
    memset(g_m, 0, sizeof(g_m));

    for (g_round = 0; g_round < rounds; g_round++) {
        int sp = (int)(rnd() % SPACES), f = (int)(rnd() % FILES), o = (int)(rnd() % OWNERS);
        uint32_t op = rnd() % 20u;

        if (op == 0u) {
            vibeos_flk_drop_owner(owner_id(o));
            for (sp = 0; sp < SPACES; sp++) {
                for (f = 0; f < FILES; f++) {
                    memset(g_m[sp][f][o], 0, CELLS);
                }
            }
        } else if (op == 1u) {
            vibeos_flk_drop_file(owner_id(o), &g_fs, (uint64_t)f + 1u);
            for (sp = 0; sp < SPACES; sp++) {
                memset(g_m[sp][f][o], 0, CELLS);
            }
        } else {
            uint32_t type = rnd() % 3u;   /* unlock, shared, exclusive */
            uint64_t s = rnd() % 16u, e;
            int c0, c1, c, who = -1, want, got;
            uint64_t blocker = 0;

            e = (rnd() % 4u) == 0u ? VIBEOS_FLK_END : s + rnd() % (16u - (uint32_t)s);
            cells(s, e, &c0, &c1);
            want = type != VIBEOS_FLK_UNLOCK && m_conflict(sp, f, o, type, c0, c1, &who) ? -VIBEOS_EAGAIN : 0;
            got = vibeos_flk_set((uint32_t)sp, &g_fs, (uint64_t)f + 1u, owner_id(o), (uint32_t)(100 + o),
                                 type, s, e, &blocker);
            if (got != want) {
                printf("  space %d file %d owner %d type %u [%llu, %llu]: table %d, model %d\n", sp, f, o,
                       type, (unsigned long long)s, (unsigned long long)e, got, want);
                die("a request was answered differently");
            }
            if (got == 0) {
                for (c = c0; c <= c1; c++) {
                    g_m[sp][f][o][c] = (uint8_t)type;
                }
            } else {
                int p, real = 0;
                for (p = 0; p < OWNERS; p++) {
                    if (blocker == owner_id(p) && p != o) {
                        int q = -1;
                        /* Is p really in the way? Ask the model about p alone. */
                        for (c = c0; c <= c1 && q < 0; c++) {
                            uint8_t have = g_m[sp][f][p][c];
                            if (have && (type == VIBEOS_FLK_EXCL || have == VIBEOS_FLK_EXCL)) {
                                q = c;
                            }
                        }
                        real = q >= 0;
                    }
                }
                if (!real) {
                    die("the owner named as being in the way is not");
                }
            }
        }
        check_all();
    }
    printf("filelock_torture ok locks=%u\n", vibeos_flk_count());
    return 0;
}
