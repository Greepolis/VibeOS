/* Randomised torture for tmpfs (kernel/fs/tmpfs.c, docs/abi/ L1 step 2),
 * checked against a model kept in plain arrays.
 *
 *     vibeos_tmpfs_torture <seed> <rounds>
 *
 * Every module gets an intensive test in the nightly (CLAUDE.md), and the
 * reason is the one the two before this gave: a filesystem asked whether it is
 * right answers from the structures it used to decide, so the defects that
 * survive are the self-consistent ones - a directory record that names the
 * wrong inode, a page freed while another name still reaches it, a link count
 * that drifts. The model here shares no code with tmpfs: its own inodes, its
 * own directories as small arrays, its own file contents as sparse pages, and
 * its own account of how many pages tmpfs ought to be holding.
 *
 * Each round is one operation on a small namespace - eleven paths across three
 * directories, so names collide, directories fill and empty, and renames land
 * on things that exist. Every return value is compared, every read compares
 * bytes, and after each round every path's lookup, type, size and link count
 * is compared, and the page and inode counts. A failure prints the seed, the
 * round and the operation, and replays exactly. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vibeos/tmpfs.h"
#include "vibeos/path.h"
#include "vibeos/abi_linux.h"

#define PAGE VIBEOS_TMPFS_PAGE
#define DIRECT VIBEOS_TMPFS_DIRECT
#define PTRS (PAGE / sizeof(void *))
#define MAXPG ((uint64_t)DIRECT + PTRS + (uint64_t)PTRS * PTRS)
#define M_INODES 64
#define M_NAMES 16
#define DIRENTS_PER_PAGE 15u   /* tmpfs's record is 264 bytes */

/* ---- the model -------------------------------------------------------------------- */

enum { M_FREE = 0, M_REG, M_DIR, M_LNK };

typedef struct {
    uint64_t idx;
    uint8_t *data;
} m_page_t;

typedef struct {
    int type;
    int nlink;
    int parent;
    uint64_t size;
    /* a file's pages, sparse */
    m_page_t *pages;
    int npages;
    /* a directory's names */
    char names[M_NAMES][8];
    int inos[M_NAMES];
    int nnames;
    int max_names;              /* tmpfs keeps a directory's pages until it goes */
    /* which pointer pages tmpfs should be holding for this file */
    int ind;
    int dind;
    uint8_t mid[PTRS];
    char target[64];
    uint32_t gen;               /* moves on every free, as tmpfs's must */
} m_inode_t;

static m_inode_t M[M_INODES];

static int m_alloc(int type) {
    int i;
    for (i = 1; i < M_INODES; i++) {
        if (M[i].type == M_FREE) {
            uint32_t gen = M[i].gen;
            memset(&M[i], 0, sizeof(M[i]));
            M[i].gen = gen;
            M[i].type = type;
            M[i].nlink = type == M_DIR ? 2 : 1;
            return i;
        }
    }
    return -1;
}

static void m_free(int i) {
    int k;
    for (k = 0; k < M[i].npages; k++) {
        free(M[i].pages[k].data);
    }
    free(M[i].pages);
    {
        uint32_t gen = M[i].gen + 1u;
        memset(&M[i], 0, sizeof(M[i]));
        M[i].gen = gen;
    }
}

static uint8_t *m_page(int i, uint64_t idx, int create) {
    int k;
    for (k = 0; k < M[i].npages; k++) {
        if (M[i].pages[k].idx == idx) {
            return M[i].pages[k].data;
        }
    }
    if (!create) {
        return 0;
    }
    M[i].pages = realloc(M[i].pages, sizeof(m_page_t) * (size_t)(M[i].npages + 1));
    M[i].pages[M[i].npages].idx = idx;
    M[i].pages[M[i].npages].data = calloc(1, PAGE);
    if (idx >= DIRECT && idx < DIRECT + PTRS) {
        M[i].ind = 1;
    } else if (idx >= DIRECT + PTRS) {
        M[i].dind = 1;
        M[i].mid[(idx - DIRECT - PTRS) / PTRS] = 1;
    }
    return M[i].pages[M[i].npages++].data;
}

/* The pages tmpfs should hold for inode i: data, pointer and directory ones. */
static uint64_t m_pages_of(int i) {
    uint64_t n = 0;
    uint32_t k;
    if (M[i].type == M_DIR) {
        return (uint64_t)((M[i].max_names + (int)DIRENTS_PER_PAGE - 1) / (int)DIRENTS_PER_PAGE);
    }
    n = (uint64_t)M[i].npages + (uint64_t)M[i].ind + (uint64_t)M[i].dind;
    for (k = 0; k < PTRS; k++) {
        n += M[i].mid[k];
    }
    return n;
}

static void m_truncate(int i, uint64_t size) {
    uint64_t keep = (size + PAGE - 1u) / PAGE;
    int k;
    uint32_t j;
    if (size < M[i].size) {
        for (k = 0; k < M[i].npages;) {
            if (M[i].pages[k].idx >= keep) {
                free(M[i].pages[k].data);
                M[i].pages[k] = M[i].pages[--M[i].npages];
            } else {
                k++;
            }
        }
        if (size % PAGE) {
            uint8_t *last = m_page(i, size / PAGE, 0);
            if (last) {
                memset(last + size % PAGE, 0, PAGE - size % PAGE);
            }
        }
        /* The pointer pages go by the same rule tmpfs states for them. */
        if (keep <= DIRECT) {
            M[i].ind = 0;
        }
        for (j = 0; j < PTRS; j++) {
            if (keep <= DIRECT + PTRS + (uint64_t)j * PTRS) {
                M[i].mid[j] = 0;
            }
        }
        if (keep <= DIRECT + PTRS) {
            M[i].dind = 0;
        }
    }
    M[i].size = size;
}

/* Resolve a path of names in the model. 0 and *out, or a negated errno. */
static int m_step(int *cur, const char *name) {
    int k;
    if (M[*cur].type != M_DIR) {
        return -VIBEOS_ENOTDIR;
    }
    for (k = 0; k < M[*cur].nnames; k++) {
        if (strcmp(M[*cur].names[k], name) == 0) {
            *cur = M[*cur].inos[k];
            return 0;
        }
    }
    return -VIBEOS_ENOENT;
}

/* By hand, not strtok_r: that is POSIX, and the Windows job builds this too. */
static int m_split(const char *path, int *dir, char *last) {
    char comp[16];
    const char *p = path, *slash;
    int cur = 0, r;

    while ((slash = strchr(p, '/')) != 0) {
        size_t n = (size_t)(slash - p);
        memcpy(comp, p, n);
        comp[n] = 0;
        r = m_step(&cur, comp);
        if (r != 0) {
            return r;
        }
        p = slash + 1;
    }
    strcpy(comp, p);
    if (M[cur].type != M_DIR) {
        return -VIBEOS_ENOTDIR;
    }
    *dir = cur;
    strcpy(last, comp);
    return 0;
}

static int m_find(int dir, const char *name) {
    int k;
    for (k = 0; k < M[dir].nnames; k++) {
        if (strcmp(M[dir].names[k], name) == 0) {
            return k;
        }
    }
    return -1;
}

static int m_resolve(const char *path, int *out) {
    int dir, k;
    char last[16];
    int r = m_split(path, &dir, last);
    if (r != 0) {
        return r;
    }
    k = m_find(dir, last);
    if (k < 0) {
        return -VIBEOS_ENOENT;
    }
    *out = M[dir].inos[k];
    return 0;
}

static void m_add(int dir, const char *name, int ino) {
    strcpy(M[dir].names[M[dir].nnames], name);
    M[dir].inos[M[dir].nnames] = ino;
    M[dir].nnames++;
    if (M[dir].nnames > M[dir].max_names) {
        M[dir].max_names = M[dir].nnames;
    }
}

static void m_remove(int dir, int k) {
    M[dir].nnames--;
    if (k != M[dir].nnames) {   /* removing the last: nothing moves, and strcpy
                                 * onto itself is undefined (ASan said so) */
        strcpy(M[dir].names[k], M[dir].names[M[dir].nnames]);
        M[dir].inos[k] = M[dir].inos[M[dir].nnames];
    }
}

static void m_unref(int ino) {
    if (M[ino].type == M_DIR || M[ino].nlink <= 1) {
        m_free(ino);
    } else {
        M[ino].nlink--;
    }
}

/* ---- the filesystem under test ----------------------------------------------------- */

static int g_pages_out;
static int g_locked;

static void *t_page(void) {
    void *p;
    if (g_locked) {
        fprintf(stderr, "FAIL:tmpfs_torture allocated under its own lock\n");
        exit(1);
    }
    p = malloc(PAGE);
    memset(p, 0x5A, PAGE);
    g_pages_out++;
    return p;
}

static void t_page_free(void *p) {
    g_pages_out--;
    free(p);
}

static void t_lock(void) {
    g_locked++;
}

static void t_unlock(void) {
    g_locked--;
}

static vibeos_tmpfs_t g_t;
static vibeos_fsmount_t g_m;

/* ---- the run ---------------------------------------------------------------------- */

static const char *const PATHS[] = {
    "a", "b", "c", "d0", "d1", "d0/a", "d0/b", "d0/e", "d0/e/a", "d1/a", "d1/b",
};
#define NPATHS ((int)(sizeof(PATHS) / sizeof(PATHS[0])))

static uint64_t g_seed;
static long g_round;
static const char *g_op;

static uint64_t rnd(void) {
    g_seed ^= g_seed << 13;
    g_seed ^= g_seed >> 7;
    g_seed ^= g_seed << 17;
    return g_seed;
}

static void fail(const char *what, long got, long want) {
    printf("FAIL:tmpfs_torture round=%ld op=%s: %s (got %ld, want %ld)\n", g_round, g_op, what,
           got, want);
    exit(1);
}

static const char *pick(void) {
    return PATHS[rnd() % (uint64_t)NPATHS];
}

static uint64_t pick_off(void) {
    uint64_t r = rnd() % 10u;
    if (r < 6u) {
        return rnd() % (3u * PAGE);
    }
    if (r < 8u) {
        return (DIRECT + rnd() % 8u) * PAGE + rnd() % PAGE;   /* single-indirect */
    }
    return (DIRECT + PTRS + rnd() % (3u * PTRS)) * PAGE + rnd() % PAGE;   /* double */
}

static void op_create(void) {
    const char *p = pick();
    int dir, r, want;
    char last[16];
    vibeos_fs_node_t n;

    g_op = "create";
    want = m_split(p, &dir, last);
    if (want == 0 && m_find(dir, last) >= 0) {
        want = -VIBEOS_EEXIST;
    }
    r = vibeos_fs_create(&g_m, p, 0644u, &n);
    if (r != want) {
        fail(p, r, want);
    }
    if (r == 0) {
        m_add(dir, last, m_alloc(M_REG));
    }
}

static void op_mkdir(void) {
    const char *p = pick();
    int dir, r, want, ino;
    char last[16];

    g_op = "mkdir";
    want = m_split(p, &dir, last);
    if (want == 0 && m_find(dir, last) >= 0) {
        want = -VIBEOS_EEXIST;
    }
    r = vibeos_fs_mkdir(&g_m, p);
    if (r != want) {
        fail(p, r, want);
    }
    if (r == 0) {
        ino = m_alloc(M_DIR);
        M[ino].parent = dir;
        m_add(dir, last, ino);
        M[dir].nlink++;
    }
}

static void op_symlink(void) {
    const char *p = pick();
    int dir, r, want, ino;
    char last[16], target[32];

    g_op = "symlink";
    snprintf(target, sizeof(target), "t%u/%u", (unsigned)(rnd() % 100u), (unsigned)(rnd() % 7u));
    want = m_split(p, &dir, last);
    if (want == 0 && m_find(dir, last) >= 0) {
        want = -VIBEOS_EEXIST;
    }
    r = vibeos_fs_symlink(&g_m, target, p);
    if (r != want) {
        fail(p, r, want);
    }
    if (r == 0) {
        ino = m_alloc(M_LNK);
        strcpy(M[ino].target, target);
        M[ino].size = strlen(target);
        memcpy(m_page(ino, 0, 1), target, strlen(target));
        m_add(dir, last, ino);
    }
}

static void op_unlink(void) {
    const char *p = pick();
    int dir, r, want, k = -1;
    char last[16];

    g_op = "unlink";
    want = m_split(p, &dir, last);
    if (want == 0) {
        k = m_find(dir, last);
        if (k < 0) {
            want = -VIBEOS_ENOENT;
        } else if (M[M[dir].inos[k]].type == M_DIR) {
            want = -VIBEOS_EISDIR;
        }
    }
    r = vibeos_fs_unlink(&g_m, p);
    if (r != want) {
        fail(p, r, want);
    }
    if (r == 0) {
        int ino = M[dir].inos[k];
        m_remove(dir, k);
        m_unref(ino);
    }
}

static void op_rmdir(void) {
    const char *p = pick();
    int dir, r, want, k = -1, ino = 0;
    char last[16];

    g_op = "rmdir";
    want = m_split(p, &dir, last);
    if (want == 0) {
        k = m_find(dir, last);
        if (k < 0) {
            want = -VIBEOS_ENOENT;
        } else {
            ino = M[dir].inos[k];
            if (M[ino].type != M_DIR) {
                want = -VIBEOS_ENOTDIR;
            } else if (M[ino].nnames) {
                want = -VIBEOS_ENOTEMPTY;
            }
        }
    }
    r = vibeos_fs_rmdir(&g_m, p);
    if (r != want) {
        fail(p, r, want);
    }
    if (r == 0) {
        m_remove(dir, k);
        M[dir].nlink--;
        m_free(ino);
    }
}

static void op_link(void) {
    const char *from = pick(), *to = pick();
    int src = 0, dir, r, want;
    char last[16];

    g_op = "link";
    want = m_resolve(from, &src);
    if (want == 0 && M[src].type == M_DIR) {
        want = -VIBEOS_EPERM;
    }
    if (want == 0) {
        want = m_split(to, &dir, last);
    }
    if (want == 0 && m_find(dir, last) >= 0) {
        want = -VIBEOS_EEXIST;
    }
    r = vibeos_fs_link(&g_m, from, to);
    if (r != want) {
        fail(from, r, want);
    }
    if (r == 0) {
        m_add(dir, last, src);
        M[src].nlink++;
    }
}

static void op_rename(void) {
    const char *from = pick(), *to = pick();
    uint32_t flags = (rnd() % 5u == 0u) ? VIBEOS_RENAME_NOREPLACE : 0u;
    int fdir, tdir, src = 0, dst = 0, fk, tk = -1, r, want, x;
    char fl[16], tl[16];

    g_op = "rename";
    want = m_split(from, &fdir, fl);
    if (want == 0) {
        fk = m_find(fdir, fl);
        if (fk < 0) {
            want = -VIBEOS_ENOENT;
        } else {
            src = M[fdir].inos[fk];
        }
    }
    if (want == 0) {
        want = m_split(to, &tdir, tl);
    }
    if (want == 0) {
        tk = m_find(tdir, tl);
        if (tk >= 0) {
            dst = M[tdir].inos[tk];
        }
        if (tk >= 0 && (flags & VIBEOS_RENAME_NOREPLACE)) {
            want = -VIBEOS_EEXIST;
        } else if (tk >= 0 && dst == src) {
            want = 1;   /* nothing happens */
        } else if (M[src].type == M_DIR) {
            for (x = tdir;; x = M[x].parent) {
                if (x == src) {
                    want = -VIBEOS_EINVAL;
                    break;
                }
                if (x == 0) {
                    break;
                }
            }
            if (want == 0 && tk >= 0 && M[dst].type != M_DIR) {
                want = -VIBEOS_ENOTDIR;
            } else if (want == 0 && tk >= 0 && M[dst].nnames) {
                want = -VIBEOS_ENOTEMPTY;
            }
        } else if (tk >= 0 && M[dst].type == M_DIR) {
            want = -VIBEOS_EISDIR;
        }
    }
    r = vibeos_fs_rename(&g_m, from, to, flags);
    if (r != (want == 1 ? 0 : want)) {
        fail(from, r, want);
    }
    if (want == 0) {
        if (tk >= 0) {
            m_remove(tdir, tk);
            if (M[dst].type == M_DIR) {
                M[tdir].nlink--;
            }
            m_unref(dst);
        }
        fk = m_find(fdir, fl);
        m_remove(fdir, fk);
        m_add(tdir, tl, src);
        if (M[src].type == M_DIR && fdir != tdir) {
            M[fdir].nlink--;
            M[tdir].nlink++;
            M[src].parent = tdir;
        }
    }
}

static void op_write(void) {
    const char *p = pick();
    static uint8_t buf[2 * PAGE + 17];
    vibeos_fs_node_t n;
    uint64_t off = pick_off();
    uint32_t len = (uint32_t)(rnd() % sizeof(buf)) + 1u, k;
    int ino, want;
    long r;

    g_op = "write";
    want = m_resolve(p, &ino);
    if (vibeos_fs_lookup(&g_m, p, &n) != (want == 0 ? 0 : want)) {
        fail(p, vibeos_fs_lookup(&g_m, p, &n), want);
    }
    if (want != 0) {
        return;
    }
    for (k = 0; k < len; k++) {
        buf[k] = (uint8_t)rnd();
    }
    r = vibeos_fs_write_at(&g_m, &n, off, buf, len);
    if (M[ino].type != M_REG) {
        if (r != -VIBEOS_EISDIR) {
            fail(p, r, -VIBEOS_EISDIR);
        }
        return;
    }
    if (r != (long)len) {
        fail(p, r, (long)len);
    }
    for (k = 0; k < len; k++) {
        uint64_t at = off + k;
        m_page(ino, at / PAGE, 1)[at % PAGE] = buf[k];
    }
    if (off + len > M[ino].size) {
        M[ino].size = off + len;
    }
}

static void op_truncate(void) {
    const char *p = pick();
    vibeos_fs_node_t n;
    uint64_t size = (rnd() % 3u == 0u) ? 0u : pick_off();
    int ino, want, r;

    g_op = "truncate";
    want = m_resolve(p, &ino);
    if (want != 0 || vibeos_fs_lookup(&g_m, p, &n) != 0) {
        return;
    }
    r = vibeos_fs_truncate(&g_m, &n, size);
    if (M[ino].type != M_REG) {
        if (r != -VIBEOS_EISDIR) {
            fail(p, r, -VIBEOS_EISDIR);
        }
        return;
    }
    if (r != 0) {
        fail(p, r, 0);
    }
    m_truncate(ino, size);
}

static void op_read(void) {
    const char *p = pick();
    static uint8_t buf[2 * PAGE + 17];
    vibeos_fs_node_t n;
    uint64_t off = (rnd() % 2u) ? pick_off() : rnd() % 64u;
    uint32_t len = (uint32_t)(rnd() % sizeof(buf)) + 1u, k;
    int ino;
    long r, want;

    g_op = "read";
    if (m_resolve(p, &ino) != 0 || vibeos_fs_lookup(&g_m, p, &n) != 0) {
        return;
    }
    r = vibeos_fs_read_at(&g_m, &n, off, buf, len);
    if (M[ino].type == M_DIR) {
        if (r != -VIBEOS_EISDIR) {
            fail(p, r, -VIBEOS_EISDIR);
        }
        return;
    }
    want = off >= M[ino].size ? 0 : (long)((M[ino].size - off) < len ? (M[ino].size - off) : len);
    if (r != want) {
        fail(p, r, want);
    }
    for (k = 0; k < (uint32_t)r; k++) {
        uint64_t at = off + k;
        uint8_t *pg = m_page(ino, at / PAGE, 0);
        uint8_t b = pg ? pg[at % PAGE] : 0u;
        if (buf[k] != b) {
            fail("a byte differs from the model", (long)k, b);
        }
    }
    if (M[ino].type == M_LNK) {
        char lb[64];
        long t = vibeos_fs_readlink(&g_m, p, lb, sizeof(lb));
        if (t != (long)strlen(M[ino].target) || memcmp(lb, M[ino].target, (size_t)t) != 0) {
            fail("readlink", t, (long)strlen(M[ino].target));
        }
    }
}

/* A node looked up earlier and kept, as an open description keeps one: read
 * through it later, it must reach the same file if that file still exists and
 * be refused if it was freed - never another file's bytes. */
static vibeos_fs_node_t g_kept;
static int g_kept_ino = -1;
static uint32_t g_kept_gen;

static void op_stale(void) {
    uint8_t b;
    long r;

    g_op = "stale";
    if (g_kept_ino >= 0) {
        int alive = M[g_kept_ino].type == M_REG && M[g_kept_ino].gen == g_kept_gen;
        r = vibeos_fs_read_at(&g_m, &g_kept, 0, &b, 1);
        if (!alive && r != -VIBEOS_ENOENT) {
            fail("a node of a freed file still reads", r, -VIBEOS_ENOENT);
        }
        if (alive && r < 0) {
            fail("a node of a live file is refused", r, 0);
        }
        g_kept_ino = -1;
        return;
    }
    {
        const char *p = pick();
        int ino;
        if (m_resolve(p, &ino) == 0 && M[ino].type == M_REG &&
            vibeos_fs_lookup(&g_m, p, &g_kept) == 0) {
            g_kept_ino = ino;
            g_kept_gen = M[ino].gen;
        }
    }
}

/* After every round: every path agrees, and so does the page and inode count. */
static void check_all(void) {
    uint64_t pages = 0;
    int i, used = 1;

    g_op = "check";
    for (i = 0; i < NPATHS; i++) {
        vibeos_fs_node_t n;
        int ino, want = m_resolve(PATHS[i], &ino);
        int r = vibeos_fs_lookup(&g_m, PATHS[i], &n);
        if (r != want) {
            fail(PATHS[i], r, want);
        }
        if (r != 0) {
            continue;
        }
        if ((int)n.nlink != M[ino].nlink) {
            fail("link count", (long)n.nlink, M[ino].nlink);
        }
        if (n.is_dir != (M[ino].type == M_DIR)) {
            fail("type", n.is_dir, M[ino].type);
        }
        if (M[ino].type != M_DIR && n.size != M[ino].size) {
            fail("size", (long)n.size, (long)M[ino].size);
        }
        if (M[ino].type == M_DIR) {
            /* every name the model has is listed, and nothing else */
            char nm[VIBEOS_NAME_MAX + 1u];
            uint32_t k, count = 0;
            for (k = 0; vibeos_fs_list(&g_m, PATHS[i], k, nm, sizeof(nm), 0, 0) == 0; k++) {
                if (strcmp(nm, ".") == 0 || strcmp(nm, "..") == 0) {
                    continue;
                }
                if (m_find(ino, nm) < 0) {
                    fail("listed a name the model does not have", (long)k, 0);
                }
                count++;
            }
            if ((int)count != M[ino].nnames) {
                fail("listed names", (long)count, M[ino].nnames);
            }
        }
    }
    for (i = 0; i < M_INODES; i++) {
        if (M[i].type != M_FREE) {
            pages += m_pages_of(i);
            if (i != 0) {
                used++;
            }
        }
    }
    if (g_t.pages_used != pages) {
        fail("pages held", (long)g_t.pages_used, (long)pages);
    }
    if ((uint64_t)g_pages_out != pages) {
        fail("pages the allocator has out", g_pages_out, (long)pages);
    }
    if ((int)g_t.inodes_used != used) {
        fail("inodes", (long)g_t.inodes_used, used);
    }
}

int main(int argc, char **argv) {
    uint64_t seed = argc > 1 ? strtoull(argv[1], 0, 0) : 1u;
    long rounds = argc > 2 ? strtol(argv[2], 0, 0) : 2000;

    printf("tmpfs_torture seed=%llu rounds=%ld\n", (unsigned long long)seed, rounds);
    g_seed = seed * 2654435761u + 1u;
    memset(M, 0, sizeof(M));
    M[0].type = M_DIR;
    M[0].nlink = 2;
    memset(&g_t, 0, sizeof(g_t));
    if (vibeos_tmpfs_init(&g_t, 1u << 20, t_page, t_page_free, t_lock, t_unlock) != 0 ||
        vibeos_fs_mount(&g_m, vibeos_tmpfs_ops(), &g_t, "tmpfs") != 0) {
        printf("FAIL:tmpfs_torture could not mount\n");
        return 1;
    }
    for (g_round = 0; g_round < rounds; g_round++) {
        switch (rnd() % 12u) {
            case 0: op_create(); break;
            case 1: op_mkdir(); break;
            case 2: op_symlink(); break;
            case 3: op_unlink(); break;
            case 4: op_rmdir(); break;
            case 5: op_link(); break;
            case 6: op_rename(); break;
            case 7: case 8: op_write(); break;
            case 9: op_truncate(); break;
            case 10: op_stale(); break;
            default: op_read(); break;
        }
        check_all();
    }
    vibeos_tmpfs_destroy(&g_t);
    if (g_pages_out != 0) {
        printf("FAIL:tmpfs_torture %d pages never given back\n", g_pages_out);
        return 1;
    }
    printf("tmpfs_torture ok\n");
    return 0;
}
