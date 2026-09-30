/* Paths. See include/vibeos/path.h. */

#include "vibeos/path.h"
#include "vibeos/abi_linux.h"

static uint32_t path_len(const char *s) {
    uint32_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

/* Append one component to `out` (length *n), or drop the last one for "..",
 * never going shorter than `floor` - the root's own length. */
static int path_push(char *out, uint32_t *n, uint32_t cap, uint32_t floor,
                     const char *comp, uint32_t len) {
    uint32_t i;

    if (len == 0u || (len == 1u && comp[0] == '.')) {
        return 0;
    }
    if (len > VIBEOS_NAME_MAX) {
        return -VIBEOS_ENAMETOOLONG;
    }
    if (len == 2u && comp[0] == '.' && comp[1] == '.') {
        while (*n > floor && out[*n - 1u] != '/') {
            (*n)--;
        }
        if (*n > floor) {
            (*n)--;             /* the slash in front of the removed component */
        }
        if (*n == 0u) {
            out[(*n)++] = '/';  /* the root itself */
        }
        out[*n] = 0;
        return 0;
    }
    if (!(*n == 1u && out[0] == '/')) {
        if (*n + 1u >= cap) {
            return -VIBEOS_ENAMETOOLONG;
        }
        out[(*n)++] = '/';
    }
    if (*n + len >= cap) {
        return -VIBEOS_ENAMETOOLONG;
    }
    for (i = 0; i < len; i++) {
        out[(*n)++] = comp[i];
    }
    out[*n] = 0;
    return 0;
}

/* Push every component of `p` onto `out`. */
static int path_push_all(char *out, uint32_t *n, uint32_t cap, uint32_t floor,
                         const char *p) {
    while (*p) {
        const char *start;
        int r;

        while (*p == '/') {
            p++;
        }
        start = p;
        while (*p && *p != '/') {
            p++;
        }
        r = path_push(out, n, cap, floor, start, (uint32_t)(p - start));
        if (r != 0) {
            return r;
        }
    }
    return 0;
}

int vibeos_path_normalize(const char *root, const char *cwd, const char *path,
                          char *out, uint32_t cap) {
    uint32_t n = 0, floor;
    int r;

    if (!path || !out || cap < 2u) {
        return -VIBEOS_EINVAL;
    }
    if (path[0] == 0) {
        return -VIBEOS_ENOENT;   /* Linux: an empty path names nothing */
    }
    if (!root || root[0] != '/') {
        root = "/";
    }
    if (!cwd || cwd[0] != '/') {
        cwd = root;
    }
    /* The root first, and it is the floor ".." cannot go under. A root of "/"
     * is one character, and "/" is also what the floor leaves. */
    out[n++] = '/';
    out[n] = 0;
    r = path_push_all(out, &n, cap, 1u, root);
    if (r != 0) {
        return r;
    }
    floor = (n == 1u) ? 1u : n;
    if (path[0] != '/') {
        /* The working directory is already under the root, so it is taken
         * whole - but still walked, so a stale cwd with ".." in it cannot
         * climb out. */
        uint32_t rl = path_len(root);
        const char *rest = cwd;
        if (rl > 1u) {
            uint32_t i;
            for (i = 0; i < rl && cwd[i] == root[i]; i++) {
            }
            rest = (i == rl && (cwd[i] == '/' || cwd[i] == 0)) ? cwd + rl : "";
        }
        r = path_push_all(out, &n, cap, floor, rest);
        if (r != 0) {
            return r;
        }
    }
    return path_push_all(out, &n, cap, floor, path);
}

/* Look up the part of `tail` (inside `mnt`) that ends at `end`, as a directory. */
static int path_dir_prefix(vibeos_fsmount_t *mnt, const char *tail, uint32_t end) {
    char part[VIBEOS_PATH_MAX];
    vibeos_fs_node_t node;
    uint32_t i;

    if (end >= sizeof(part)) {
        return -VIBEOS_ENAMETOOLONG;
    }
    for (i = 0; i < end; i++) {
        part[i] = tail[i];
    }
    part[end] = 0;
    if (vibeos_fs_lookup(mnt, part, &node) != 0) {
        return -VIBEOS_ENOENT;
    }
    return node.is_dir ? 0 : -VIBEOS_ENOTDIR;
}

/* Every component of `tail` before the last must be a directory. */
static int path_check_parents(vibeos_fsmount_t *mnt, const char *tail) {
    uint32_t i;

    for (i = 0; tail[i]; i++) {
        if (tail[i] == '/' && i > 0u) {
            int r = path_dir_prefix(mnt, tail, i);
            if (r != 0) {
                return r;
            }
        }
    }
    return 0;
}

static int path_split(const char *abs, vibeos_fsmount_t **mnt, const char **tail) {
    if (!abs || abs[0] != '/') {
        return -VIBEOS_ENOENT;
    }
    if (vibeos_fs_resolve(abs, mnt, tail) != 0 || !*mnt) {
        return -VIBEOS_ENOENT;
    }
    while (**tail == '/') {
        (*tail)++;
    }
    return 0;
}

int vibeos_path_lookup(const char *abs, vibeos_fsmount_t **mnt, const char **tail,
                       vibeos_fs_node_t *node) {
    int r = path_split(abs, mnt, tail);

    if (r != 0) {
        return r;
    }
    r = path_check_parents(*mnt, *tail);
    if (r != 0) {
        return r;
    }
    /* A path that is the mount point itself leaves nothing inside the mount.
     * Drivers spell their root "/"; not every one takes the empty string. */
    return vibeos_fs_lookup(*mnt, **tail ? *tail : "/", node) == 0 ? 0 : -VIBEOS_ENOENT;
}

int vibeos_path_parent(const char *abs, vibeos_fsmount_t **mnt, const char **tail) {
    int r = path_split(abs, mnt, tail);

    if (r != 0) {
        return r;
    }
    if (**tail == 0) {
        return -VIBEOS_EEXIST;   /* the mount's own root: it is there already */
    }
    return path_check_parents(*mnt, *tail);
}
