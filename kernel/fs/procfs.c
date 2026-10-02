/* /proc. See include/vibeos/procfs.h. */

#include "vibeos/procfs.h"
#include "vibeos/abi_linux.h"

/* ---- writing a file's text ----------------------------------------------------- */

typedef struct {
    char *buf;
    uint32_t cap;
    uint32_t len;
} pf_out_t;

static void pf_char(pf_out_t *o, char c) {
    if (o->len < o->cap) {
        o->buf[o->len] = c;
    }
    o->len++;   /* counted past the end too: the length is what was wanted */
}

/* "Name:" padded to sixteen columns, the number right-aligned in eight, " kB":
 * Linux's own line, which programs read with scanf and with awk both. The
 * width is fixed, so a file's length does not change between the lookup that
 * reports it and the read that follows. */
static void pf_kb(pf_out_t *o, const char *name, uint64_t kb) {
    char digits[20];
    uint32_t n = 0, col = 0, i;

    for (; *name; name++, col++) {
        pf_char(o, *name);
    }
    pf_char(o, ':');
    for (col++; col < 16u; col++) {
        pf_char(o, ' ');
    }
    do {
        digits[n++] = (char)('0' + kb % 10u);
        kb /= 10u;
    } while (kb != 0u);
    for (i = n; i < 8u; i++) {
        pf_char(o, ' ');
    }
    while (n > 0u) {
        pf_char(o, digits[--n]);
    }
    pf_char(o, ' ');
    pf_char(o, 'k');
    pf_char(o, 'B');
    pf_char(o, '\n');
}

static uint32_t pf_meminfo(const vibeos_procfs_t *pf, char *buf, uint32_t cap) {
    vibeos_procfs_mem_t m = {0, 0, 0, 0, 0, 0};
    pf_out_t o = {buf, cap, 0};

    if (pf && pf->mem) {
        pf->mem(&m);
    }
    pf_kb(&o, "MemTotal", m.total_kb);
    pf_kb(&o, "MemFree", m.free_kb);
    pf_kb(&o, "MemAvailable", m.available_kb);
    pf_kb(&o, "Buffers", 0);
    pf_kb(&o, "Cached", m.cached_kb);
    pf_kb(&o, "SwapTotal", m.swap_total_kb);
    pf_kb(&o, "SwapFree", m.swap_free_kb);
    return o.len;
}

static uint32_t pf_pid_max(const vibeos_procfs_t *pf, char *buf, uint32_t cap) {
    pf_out_t o = {buf, cap, 0};
    uint64_t v = pf ? pf->pid_max : 0u;
    char digits[20];
    uint32_t n = 0;

    do {
        digits[n++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v != 0u);
    while (n > 0u) {
        pf_char(&o, digits[--n]);
    }
    pf_char(&o, '\n');
    return o.len;
}

/* ---- the files ------------------------------------------------------------------- */

typedef struct {
    const char *path;                         /* under /proc, no leading slash */
    uint32_t (*text)(const vibeos_procfs_t *pf, char *buf, uint32_t cap);
} pf_file_t;

#define PF_TEXT_MAX 512u   /* the longest file here, with room */

static const pf_file_t g_files[] = {
    {"meminfo", pf_meminfo},
    {"sys/kernel/pid_max", pf_pid_max},
};
#define PF_FILES ((uint32_t)(sizeof(g_files) / sizeof(g_files[0])))

/* There are no directories stored anywhere: one exists because a file's path
 * runs through it. `path` names a directory when it is empty or is the start
 * of some file's path up to a slash; this returns how much of that file's path
 * it covers, slash included (0 for the root), or -1. */
static int pf_dir_prefix(const char *path, uint32_t file) {
    const char *f = g_files[file].path;
    uint32_t i = 0;

    if (*path == 0) {
        return 0;
    }
    while (path[i] && path[i] == f[i]) {
        i++;
    }
    while (path[i] == '/') {
        path++;          /* trailing slashes */
    }
    return (path[i] == 0 && f[i] == '/') ? (int)(i + 1u) : -1;
}

static int pf_same(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == 0 && *b == 0;
}

/* Node ids: a file is 2 + its place in the table; a directory is 1, whichever
 * it is - nothing is ever done to one by id. */
static int pf_lookup(void *fs, const char *path, vibeos_fs_node_t *out) {
    char text[PF_TEXT_MAX];
    uint32_t i;

    while (*path == '/') {
        path++;
    }
    for (i = 0; i < PF_FILES; i++) {
        if (pf_same(path, g_files[i].path)) {
            out->id = 2u + i;
            out->size = g_files[i].text((const vibeos_procfs_t *)fs, text, PF_TEXT_MAX);
            out->is_dir = 0;
            out->mode = VIBEOS_S_IFREG | 0444u;
            out->nlink = 1u;
            return 0;
        }
    }
    for (i = 0; i < PF_FILES; i++) {
        if (pf_dir_prefix(path, i) >= 0) {
            out->id = 1u;
            out->size = 0;
            out->is_dir = 1;
            out->mode = VIBEOS_S_IFDIR | 0555u;
            out->nlink = 2u;
            return 0;
        }
    }
    return -VIBEOS_ENOENT;
}

static long pf_read_at(void *fs, const vibeos_fs_node_t *node, uint64_t off, void *buf, uint32_t len) {
    char text[PF_TEXT_MAX];
    uint32_t n, i;

    if (node->id < 2u || node->id - 2u >= PF_FILES) {
        return -VIBEOS_EISDIR;
    }
    n = g_files[node->id - 2u].text((const vibeos_procfs_t *)fs, text, PF_TEXT_MAX);
    if (n > PF_TEXT_MAX) {
        n = PF_TEXT_MAX;
    }
    if (off >= n) {
        return 0;
    }
    if (len > n - (uint32_t)off) {
        len = n - (uint32_t)off;
    }
    for (i = 0; i < len; i++) {
        ((char *)buf)[i] = text[(uint32_t)off + i];
    }
    return (long)len;
}

/* The entries of a directory are the next component of every file's path that
 * runs through it, each once: the first file that has a component names it,
 * and a later file with the same one is passed over. */
static int pf_list(void *fs, const char *path, uint32_t index, char *name, uint32_t cap,
                   uint64_t *out_size, int *out_is_dir) {
    char text[PF_TEXT_MAX];
    uint32_t i, seen = 0;
    int any = 0;

    while (*path == '/') {
        path++;
    }
    for (i = 0; i < PF_FILES; i++) {
        int at = pf_dir_prefix(path, i);
        const char *c;
        uint32_t len = 0, j, k;
        int dup = 0;

        if (at < 0) {
            continue;
        }
        any = 1;
        c = g_files[i].path + at;
        while (c[len] && c[len] != '/') {
            len++;
        }
        for (j = 0; j < i && !dup; j++) {
            int at2 = pf_dir_prefix(path, j);
            if (at2 == at) {
                const char *d = g_files[j].path + at2;
                for (k = 0; k < len && d[k] == c[k]; k++) {
                }
                dup = k == len && (d[len] == 0 || d[len] == '/');
            }
        }
        if (dup) {
            continue;
        }
        if (seen++ != index) {
            continue;
        }
        if (cap == 0u) {
            return -1;
        }
        for (k = 0; k + 1u < cap && k < len; k++) {
            name[k] = c[k];
        }
        name[k] = 0;
        if (out_is_dir) {
            *out_is_dir = c[len] == '/';
        }
        if (out_size) {
            *out_size = c[len] == '/' ? 0u
                                      : g_files[i].text((const vibeos_procfs_t *)fs, text, PF_TEXT_MAX);
        }
        return 0;
    }
    return any ? -1 : -VIBEOS_ENOTDIR;
}

/* No lock in this file, because there is nothing to hold one over: it keeps no
 * state. The only data it defines is the table above and the operations below,
 * both constant; every number it prints is asked of the mount's owner at the
 * moment of the read. */
static const vibeos_fs_ops_t g_procfs_ops = {
    .lookup = pf_lookup,
    .read_at = pf_read_at,
    .list = pf_list,
};

const vibeos_fs_ops_t *vibeos_procfs_ops(void) {
    return &g_procfs_ops;
}
