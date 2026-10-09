/* Paths. See include/vibeos/path.h. */

#include "vibeos/path.h"
#include "vibeos/cred.h"
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

/* ---- the walk (L1) -------------------------------------------------------------- */

/* Which mount an absolute path is on, what is left of it inside, and the node.
 * A path that is the mount point itself leaves nothing inside the mount:
 * drivers spell their root "/", not every one takes the empty string. */
static int walk_lookup(const char *abs, vibeos_fsmount_t **mnt, const char **tail,
                       vibeos_fs_node_t *node) {
    if (vibeos_fs_resolve(abs, mnt, tail) != 0 || !*mnt) {
        return -VIBEOS_ENOENT;
    }
    while (**tail == '/') {
        (*tail)++;
    }
    return vibeos_fs_lookup(*mnt, **tail ? *tail : "/", node) == 0 ? 0 : -VIBEOS_ENOENT;
}

/* What is left to walk is kept as a string; a link's target goes in front of
 * it. Twice a path, because a link can be a path long and name more path. */
#define WALK_PENDING (2u * VIBEOS_PATH_MAX)

int vibeos_path_walk(const char *root, const char *base, const char *path,
                     uint32_t flags, vibeos_path_t *out) {
    return vibeos_path_walk_as(root, base, path, flags, 0, out);
}

int vibeos_path_walk_as(const char *root, const char *base, const char *path, uint32_t flags,
                        const void *who_v, vibeos_path_t *out) {
    const vibeos_cred_t *who = (const vibeos_cred_t *)who_v;
    char pending[WALK_PENDING];
    char target[VIBEOS_PATH_MAX];
    uint32_t n, floor, links = 0, i, plen;
    const char *p;
    int looked = 0, r;

    if (!path || !out) {
        return -VIBEOS_EINVAL;
    }
    if (path[0] == 0) {
        return -VIBEOS_ENOENT;   /* Linux: an empty path names nothing */
    }
    out->exists = 0;
    out->mnt = 0;
    out->tail = out->path;
    out->trailing_slash = 0;
    out->by_node = 0;
    for (plen = 0; path[plen]; plen++) {
    }
    if (plen >= VIBEOS_PATH_MAX) {
        return -VIBEOS_ENAMETOOLONG;
    }
    for (i = plen; i > 0u && path[i - 1u] == '/'; i--) {
    }
    out->trailing_slash = (i > 0u && i < plen);

    /* Where the walk starts: the root for an absolute path, the base directory
     * (already resolved - a working directory or a directory descriptor's path)
     * for a relative one. Both lexical, and both already real. */
    /* The root's own length first - the floor ".." stops at - measured in the
     * output buffer rather than a second one: kernel stacks are small, and the
     * walk already holds two path-sized buffers. */
    r = vibeos_path_normalize(root, root, "/", out->path, VIBEOS_PATH_MAX);
    if (r != 0) {
        return r;
    }
    for (floor = 0; out->path[floor]; floor++) {
    }
    r = vibeos_path_normalize(root, base, path[0] == '/' ? "/" : ".", out->path,
                              VIBEOS_PATH_MAX);
    if (r != 0) {
        return r;
    }
    for (n = 0; out->path[n]; n++) {
    }
    for (i = 0; i <= plen; i++) {
        pending[i] = path[i];
    }

    p = pending;
    for (;;) {
        const char *start;
        uint32_t len, saved;
        int last;
        vibeos_fs_node_t node;
        vibeos_fsmount_t *mnt;
        const char *tail;

        while (*p == '/') {
            p++;
        }
        if (*p == 0) {
            break;
        }
        start = p;
        while (*p && *p != '/') {
            p++;
        }
        len = (uint32_t)(p - start);
        {
            const char *q = p;
            while (*q == '/') {
                q++;
            }
            last = (*q == 0);
        }
        if (len == 1u && start[0] == '.') {
            continue;
        }
        if (len == 2u && start[0] == '.' && start[1] == '.') {
            /* The path so far is real - every component in it was looked up,
             * and links in it were replaced by what they point at - so removing
             * one is going to the parent of what it names. */
            r = path_push(out->path, &n, VIBEOS_PATH_MAX, floor, start, len);
            if (r != 0) {
                return r;
            }
            looked = 0;
            continue;
        }
        if (who && who->fsuid != 0u) {
            /* Search permission on the directory this component is looked up
             * in: what out->path names right now. Looked up again for the
             * purpose, and only for somebody who can be refused. */
            vibeos_fs_node_t dir;
            vibeos_fsmount_t *dm = 0;
            const char *dt = out->path;

            if (walk_lookup(out->path, &dm, &dt, &dir) == 0 &&
                vibeos_cred_may(who, 0, dir.mode, dir.uid, dir.gid, VIBEOS_MAY_EXEC) != 0) {
                return -VIBEOS_EACCES;
            }
        }
        saved = n;
        r = path_push(out->path, &n, VIBEOS_PATH_MAX, floor, start, len);
        if (r != 0) {
            return r;
        }
        mnt = 0;
        tail = out->path;
        r = walk_lookup(out->path, &mnt, &tail, &node);
        if (r != 0) {
            if (last && (flags & VIBEOS_PATH_CREATE) && mnt) {
                out->mnt = mnt;
                out->tail = tail;
                return 0;   /* exists = 0: where it would be */
            }
            return r;
        }
        looked = 1;
        if ((node.mode & VIBEOS_S_IFMT) == VIBEOS_S_IFLNK &&
            (!last || !(flags & VIBEOS_PATH_NOFOLLOW) || out->trailing_slash)) {
            long t;
            uint32_t rest, k;

            if (++links > VIBEOS_PATH_SYMLINK_MAX) {
                return -VIBEOS_ELOOP;
            }
            t = vibeos_fs_readlink(mnt, *tail ? tail : "/", target, sizeof(target));
            if (t < 0) {
                return (int)t;
            }
            if ((uint32_t)t >= sizeof(target)) {
                return -VIBEOS_ENAMETOOLONG;   /* not cut short and followed */
            }
            if (t == 0) {
                return -VIBEOS_ENOENT;   /* an empty link names nothing */
            }
            /* The link is replaced by its target: an absolute one starts again
             * from the root, a relative one from the directory holding it. */
            n = (target[0] == '/') ? floor : saved;
            out->path[n] = 0;
            if (n == 0u) {
                out->path[n++] = '/';
                out->path[n] = 0;
            }
            for (rest = 0; p[rest]; rest++) {
            }
            if ((uint32_t)t + 1u + rest + 1u > WALK_PENDING) {
                return -VIBEOS_ENAMETOOLONG;
            }
            /* In place: what is left moves to make room, then the target goes
             * in front of it. Up or down depending on whether the target is
             * longer than what the walk has consumed, and in the direction that
             * reads each byte before it is overwritten. */
            {
                uint32_t from = (uint32_t)(p - pending);
                uint32_t to = (uint32_t)t + 1u;
                if (to > from) {
                    for (k = rest + 1u; k > 0u; k--) {
                        pending[to + k - 1u] = pending[from + k - 1u];
                    }
                } else {
                    for (k = 0; k <= rest; k++) {
                        pending[to + k] = pending[from + k];
                    }
                }
                for (k = 0; k < (uint32_t)t; k++) {
                    pending[k] = target[k];
                }
                pending[t] = '/';
            }
            p = pending;
            looked = 0;
            continue;
        }
        if (!last && !node.is_dir) {
            return -VIBEOS_ENOTDIR;
        }
        out->node = node;
        out->mnt = mnt;
        out->tail = tail;
    }

    /* Nothing was looked up since the last change - a path of "/", ".", or
     * one that ended in "..": ask about where the walk stands. */
    if (!looked) {
        r = walk_lookup(out->path, &out->mnt, &out->tail, &out->node);
        if (r != 0) {
            return r;
        }
    }
    out->exists = 1;
    if (out->trailing_slash && !out->node.is_dir) {
        return -VIBEOS_ENOTDIR;
    }
    return 0;
}
