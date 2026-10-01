/* tmpfs. See include/vibeos/tmpfs.h. */

#include "vibeos/tmpfs.h"
#include "vibeos/path.h"
#include "vibeos/abi_linux.h"
#include "vibeos/mbz.h"

#define PTRS (VIBEOS_TMPFS_PAGE / sizeof(void *))       /* pointers in a page */
#define MAX_PAGES ((uint64_t)VIBEOS_TMPFS_DIRECT + PTRS + (uint64_t)PTRS * PTRS)

/* A directory is a file of these. Fixed size so a record's place is arithmetic;
 * the name is kept whole, NAME_MAX of it. */
typedef struct {
    uint32_t ino;
    uint16_t len;
    uint16_t pad;
    char name[VIBEOS_NAME_MAX + 1u];
} tf_dirent_t;

#define DIRENTS_PER_PAGE (VIBEOS_TMPFS_PAGE / sizeof(tf_dirent_t))

/* ---- small helpers ------------------------------------------------------------- */

static void tf_zero(void *p, uint64_t n) {
    uint8_t *b = (uint8_t *)p;
    uint64_t i;
    for (i = 0; i < n; i++) {
        b[i] = 0;
    }
}

static void tf_copy(void *dst, const void *src, uint64_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    uint64_t i;
    for (i = 0; i < n; i++) {
        d[i] = s[i];
    }
}

static int tf_is(const vibeos_tmpfs_inode_t *in, uint32_t type) {
    return (in->mode & VIBEOS_S_IFMT) == type;
}

/* Pages are allocated before the lock is taken and handed out under it.
 *
 * The allocator may reclaim - drop clean pages, write dirty ones to swap - and
 * the lock masks interrupts, so allocating under it would hold every core that
 * wants /tmp for as long as a disk write takes (CLAUDE.md: "spinlocks mask
 * interrupts"). So an operation fills a few spares first, the code under the
 * lock takes from them, and whatever it did not use is given back after. The
 * most one step needs is three: a data page and the two pointer pages above
 * it. */
#define TF_SPARES 3u

typedef struct {
    void *page[TF_SPARES];
    uint32_t n;
} tf_ctx_t;

static void tf_ctx_fill(vibeos_tmpfs_t *t, tf_ctx_t *cx, uint32_t want) {
    cx->n = 0;
    while (cx->n < want && cx->n < TF_SPARES) {
        void *pg = t->page_alloc();
        if (!pg) {
            break;
        }
        cx->page[cx->n++] = pg;
    }
}

static void tf_ctx_drain(vibeos_tmpfs_t *t, tf_ctx_t *cx) {
    while (cx->n > 0u) {
        t->page_free(cx->page[--cx->n]);
    }
}

/* A page for the filesystem, zeroed - a freed page keeps whatever it held, and
 * the kernel's free-page poison is exactly what a hole must not read as. 0 when
 * the filesystem is full or the spares ran out. */
static uint8_t *tf_page(vibeos_tmpfs_t *t, tf_ctx_t *cx) {
    uint8_t *pg;
    if (!cx || cx->n == 0u || t->pages_used >= t->pages_max) {
        return 0;
    }
    pg = (uint8_t *)cx->page[--cx->n];
    tf_zero(pg, VIBEOS_TMPFS_PAGE);
    t->pages_used++;
    return pg;
}

static void tf_unpage(vibeos_tmpfs_t *t, void *p) {
    if (p) {
        t->page_free(p);
        t->pages_used--;
    }
}

/* ---- a file's pages ------------------------------------------------------------ */

/* Where the pointer to page `idx` of a file lives, allocating pointer pages on
 * the way when `alloc`. 0 when it cannot exist (past the largest file, or out
 * of pages). */
static uint8_t **tf_slot(vibeos_tmpfs_t *t, vibeos_tmpfs_inode_t *in, uint64_t idx, tf_ctx_t *alloc) {
    if (idx < VIBEOS_TMPFS_DIRECT) {
        return &in->direct[idx];
    }
    idx -= VIBEOS_TMPFS_DIRECT;
    if (idx < PTRS) {
        if (!in->ind) {
            if (!alloc || !(in->ind = (uint8_t **)(void *)tf_page(t, alloc))) {
                return 0;
            }
        }
        return &in->ind[idx];
    }
    idx -= PTRS;
    if (idx < (uint64_t)PTRS * PTRS) {
        uint8_t ***top;
        if (!in->dind) {
            if (!alloc || !(in->dind = (uint8_t ***)(void *)tf_page(t, alloc))) {
                return 0;
            }
        }
        top = &in->dind[idx / PTRS];
        if (!*top) {
            if (!alloc || !(*top = (uint8_t **)(void *)tf_page(t, alloc))) {
                return 0;
            }
        }
        return &(*top)[idx % PTRS];
    }
    return 0;
}

/* The page holding byte `off`, or 0 for a hole (or, with alloc, for no room). */
static uint8_t *tf_block(vibeos_tmpfs_t *t, vibeos_tmpfs_inode_t *in, uint64_t off, tf_ctx_t *alloc) {
    uint8_t **slot = tf_slot(t, in, off / VIBEOS_TMPFS_PAGE, alloc);
    if (!slot) {
        return 0;
    }
    if (!*slot && alloc) {
        *slot = tf_page(t, alloc);
    }
    return *slot;
}

/* Drop every page at or past page index `keep`, and the pointer pages left
 * empty by that. */
static void tf_drop_pages(vibeos_tmpfs_t *t, vibeos_tmpfs_inode_t *in, uint64_t keep) {
    uint64_t i, j;

    for (i = 0; i < VIBEOS_TMPFS_DIRECT; i++) {
        if (i >= keep) {
            tf_unpage(t, in->direct[i]);
            in->direct[i] = 0;
        }
    }
    if (in->ind) {
        for (i = 0; i < PTRS; i++) {
            if (VIBEOS_TMPFS_DIRECT + i >= keep) {
                tf_unpage(t, in->ind[i]);
                in->ind[i] = 0;
            }
        }
        if (keep <= VIBEOS_TMPFS_DIRECT) {
            tf_unpage(t, in->ind);
            in->ind = 0;
        }
    }
    if (in->dind) {
        uint64_t base = VIBEOS_TMPFS_DIRECT + PTRS;
        for (i = 0; i < PTRS; i++) {
            uint8_t **mid = in->dind[i];
            if (!mid) {
                continue;
            }
            for (j = 0; j < PTRS; j++) {
                if (base + i * PTRS + j >= keep) {
                    tf_unpage(t, mid[j]);
                    mid[j] = 0;
                }
            }
            if (keep <= base + i * PTRS) {
                tf_unpage(t, mid);
                in->dind[i] = 0;
            }
        }
        if (keep <= base) {
            tf_unpage(t, in->dind);
            in->dind = 0;
        }
    }
}

static long tf_read(vibeos_tmpfs_t *t, vibeos_tmpfs_inode_t *in, uint64_t off, void *buf,
                    uint32_t len) {
    uint64_t done = 0;

    if (off >= in->size) {
        return 0;
    }
    if ((uint64_t)len > in->size - off) {
        len = (uint32_t)(in->size - off);
    }
    while (done < len) {
        uint64_t at = off + done;
        uint64_t in_page = at % VIBEOS_TMPFS_PAGE;
        uint64_t n = VIBEOS_TMPFS_PAGE - in_page;
        uint8_t *page = tf_block(t, in, at, 0);
        if (n > len - done) {
            n = len - done;
        }
        if (page) {
            tf_copy((uint8_t *)buf + done, page + in_page, n);
        } else {
            tf_zero((uint8_t *)buf + done, n);   /* a hole */
        }
        done += n;
    }
    return (long)done;
}

static long tf_write(vibeos_tmpfs_t *t, vibeos_tmpfs_inode_t *in, uint64_t off, const void *buf,
                     uint32_t len, tf_ctx_t *cx) {
    uint64_t done = 0;

    if (len == 0u) {
        return 0;
    }
    if (off > MAX_PAGES * VIBEOS_TMPFS_PAGE || (uint64_t)len > MAX_PAGES * VIBEOS_TMPFS_PAGE - off) {
        return -VIBEOS_EFBIG;
    }
    while (done < len) {
        uint64_t at = off + done;
        uint64_t in_page = at % VIBEOS_TMPFS_PAGE;
        uint64_t n = VIBEOS_TMPFS_PAGE - in_page;
        uint8_t *page = tf_block(t, in, at, cx);
        if (!page) {
            break;   /* out of pages: what was written stands */
        }
        if (n > len - done) {
            n = len - done;
        }
        tf_copy(page + in_page, (const uint8_t *)buf + done, n);
        done += n;
    }
    if (done == 0u) {
        return -VIBEOS_ENOSPC;
    }
    if (off + done > in->size) {
        in->size = off + done;
    }
    in->mtime_ns = in->ctime_ns = vibeos_fs_now_ns();
    return (long)done;
}

/* A new size: pages past it go, and the tail of the last kept page is zeroed so
 * a later extension reads zeros, not what was cut off. */
static void tf_resize(vibeos_tmpfs_t *t, vibeos_tmpfs_inode_t *in, uint64_t size) {
    if (size < in->size) {
        uint64_t keep = (size + VIBEOS_TMPFS_PAGE - 1u) / VIBEOS_TMPFS_PAGE;
        uint8_t *last;
        tf_drop_pages(t, in, keep);
        if (size % VIBEOS_TMPFS_PAGE && (last = tf_block(t, in, size, 0)) != 0) {
            tf_zero(last + size % VIBEOS_TMPFS_PAGE, VIBEOS_TMPFS_PAGE - size % VIBEOS_TMPFS_PAGE);
        }
    }
    in->size = size;
}

/* ---- inodes -------------------------------------------------------------------- */

static int tf_alloc_inode(vibeos_tmpfs_t *t, uint32_t mode, uint32_t *out) {
    uint32_t i;
    uint64_t now = vibeos_fs_now_ns();

    for (i = 1; i < VIBEOS_TMPFS_INODES; i++) {
        vibeos_tmpfs_inode_t *in = &t->inode[i];
        if (in->mode == 0u) {
            uint32_t gen = in->gen;
            tf_zero(in, sizeof(*in));
            in->gen = gen;
            in->mode = mode;
            in->nlink = 1u;
            in->atime_ns = in->mtime_ns = in->ctime_ns = now;
            t->inodes_used++;
            *out = i;
            return 0;
        }
    }
    return -VIBEOS_ENOSPC;
}

static void tf_free_inode(vibeos_tmpfs_t *t, uint32_t i) {
    vibeos_tmpfs_inode_t *in = &t->inode[i];
    tf_drop_pages(t, in, 0);
    in->mode = 0;
    in->gen++;          /* a description still holding the old id is refused */
    t->inodes_used--;
}

/* The id a node carries: the generation above the index, and never 0 - the
 * layers above read 0 as "no identity". */
static uint64_t tf_id(const vibeos_tmpfs_t *t, uint32_t i) {
    return ((uint64_t)t->inode[i].gen << 32) | (uint64_t)(i + 1u);
}

static int tf_from_id(const vibeos_tmpfs_t *t, uint64_t id, uint32_t *out) {
    uint32_t i = (uint32_t)(id & 0xFFFFFFFFu);
    if (i == 0u || i > VIBEOS_TMPFS_INODES) {
        return -VIBEOS_ENOENT;
    }
    i--;
    if (t->inode[i].mode == 0u || t->inode[i].gen != (uint32_t)(id >> 32)) {
        return -VIBEOS_ENOENT;   /* freed since it was looked up */
    }
    *out = i;
    return 0;
}

static void tf_fill(const vibeos_tmpfs_t *t, uint32_t i, vibeos_fs_node_t *out) {
    const vibeos_tmpfs_inode_t *in = &t->inode[i];
    out->id = tf_id(t, i);
    out->is_dir = tf_is(in, VIBEOS_S_IFDIR);
    out->size = out->is_dir ? 0u : in->size;
    out->mode = in->mode;
    out->nlink = in->nlink;
    out->uid = in->uid;
    out->gid = in->gid;
    out->atime_ns = in->atime_ns;
    out->mtime_ns = in->mtime_ns;
    out->ctime_ns = in->ctime_ns;
}

/* ---- directories --------------------------------------------------------------- */

/* A directory's size counts records, not bytes. */
static tf_dirent_t *tf_dirent(vibeos_tmpfs_t *t, vibeos_tmpfs_inode_t *dir, uint64_t slot, tf_ctx_t *alloc) {
    uint8_t *page = tf_block(t, dir, (slot / DIRENTS_PER_PAGE) * VIBEOS_TMPFS_PAGE, alloc);
    if (!page) {
        return 0;
    }
    return (tf_dirent_t *)(void *)(page + (slot % DIRENTS_PER_PAGE) * sizeof(tf_dirent_t));
}

static int tf_name_eq(const tf_dirent_t *d, const char *name, uint32_t len) {
    uint32_t i;
    if (d->len != len) {
        return 0;
    }
    for (i = 0; i < len; i++) {
        if (d->name[i] != name[i]) {
            return 0;
        }
    }
    return 1;
}

/* The record naming `name` in `dir`, or -1. */
static int64_t tf_find(vibeos_tmpfs_t *t, uint32_t dir, const char *name, uint32_t len,
                       uint32_t *ino) {
    vibeos_tmpfs_inode_t *d = &t->inode[dir];
    uint64_t s;

    for (s = 0; s < d->size; s++) {
        tf_dirent_t *e = tf_dirent(t, d, s, 0);
        if (e && tf_name_eq(e, name, len)) {
            if (ino) {
                *ino = e->ino;
            }
            return (int64_t)s;
        }
    }
    return -1;
}

/* Room for one more record, taken now: a rename checks before it changes
 * anything, so it never has to put a name back. */
static int tf_reserve(vibeos_tmpfs_t *t, uint32_t dir, tf_ctx_t *cx) {
    vibeos_tmpfs_inode_t *d = &t->inode[dir];
    return tf_dirent(t, d, d->size, cx) ? 0 : -VIBEOS_ENOSPC;
}

/* Only after tf_reserve, or with a context that can supply a page. */
static int tf_add(vibeos_tmpfs_t *t, uint32_t dir, const char *name, uint32_t len, uint32_t ino,
                  tf_ctx_t *cx) {
    vibeos_tmpfs_inode_t *d = &t->inode[dir];
    tf_dirent_t *e = tf_dirent(t, d, d->size, cx);

    if (!e) {
        return -VIBEOS_ENOSPC;
    }
    e->ino = ino;
    e->len = (uint16_t)len;
    tf_copy(e->name, name, len);
    e->name[len] = 0;
    d->size++;
    d->mtime_ns = d->ctime_ns = vibeos_fs_now_ns();
    return 0;
}

/* The last record moves into the hole: order in a directory means nothing. */
static void tf_remove(vibeos_tmpfs_t *t, uint32_t dir, uint64_t slot) {
    vibeos_tmpfs_inode_t *d = &t->inode[dir];
    tf_dirent_t *hole = tf_dirent(t, d, slot, 0);
    tf_dirent_t *last = tf_dirent(t, d, d->size - 1u, 0);

    if (hole && last && hole != last) {
        tf_copy(hole, last, sizeof(*hole));
    }
    d->size--;
    d->mtime_ns = d->ctime_ns = vibeos_fs_now_ns();
}

/* ---- paths inside the filesystem ------------------------------------------------ */

/* The next component of *p, advancing it. 0 when there is none. */
static uint32_t tf_comp(const char **p, const char **start) {
    const char *s = *p;
    while (*s == '/') {
        s++;
    }
    *start = s;
    while (*s && *s != '/') {
        s++;
    }
    *p = s;
    return (uint32_t)(s - *start);
}

static int tf_step(vibeos_tmpfs_t *t, uint32_t *cur, const char *c, uint32_t len) {
    uint32_t next;

    if (!tf_is(&t->inode[*cur], VIBEOS_S_IFDIR)) {
        return -VIBEOS_ENOTDIR;
    }
    if (len > VIBEOS_NAME_MAX) {
        return -VIBEOS_ENAMETOOLONG;
    }
    if (len == 1u && c[0] == '.') {
        return 0;
    }
    if (len == 2u && c[0] == '.' && c[1] == '.') {
        *cur = t->inode[*cur].parent;
        return 0;
    }
    if (tf_find(t, *cur, c, len, &next) < 0) {
        return -VIBEOS_ENOENT;
    }
    /* A record naming a free inode is a directory that outlived a free or a
     * rename that forgot a record: the filesystem is wrong about itself.
     * Counted, and refused as EIO rather than followed into a slot that may
     * hold somebody else's file by now. */
    if (next >= VIBEOS_TMPFS_INODES || t->inode[next].mode == 0u) {
        vibeos_mbz_hit(VIBEOS_MBZ_TMPFS_BAD_RECORD, next);
        return -VIBEOS_EIO;
    }
    *cur = next;
    return 0;
}

static int tf_resolve(vibeos_tmpfs_t *t, const char *path, uint32_t *out) {
    uint32_t cur = 0, len;
    const char *c;
    int r;

    while ((len = tf_comp(&path, &c)) != 0u) {
        r = tf_step(t, &cur, c, len);
        if (r != 0) {
            return r;
        }
    }
    *out = cur;
    return 0;
}

/* The directory a new name goes in, and the name. The root has no parent: it
 * already exists. */
static int tf_split(vibeos_tmpfs_t *t, const char *path, uint32_t *dir, const char **name,
                    uint32_t *name_len) {
    uint32_t cur = 0, len, next_len;
    const char *c, *next;
    int r;

    len = tf_comp(&path, &c);
    if (len == 0u) {
        return -VIBEOS_EEXIST;
    }
    for (;;) {
        const char *probe = path;
        next_len = tf_comp(&probe, &next);
        if (next_len == 0u) {
            break;
        }
        r = tf_step(t, &cur, c, len);
        if (r != 0) {
            return r;
        }
        path = probe;
        c = next;
        len = next_len;
    }
    if (!tf_is(&t->inode[cur], VIBEOS_S_IFDIR)) {
        return -VIBEOS_ENOTDIR;
    }
    if (len > VIBEOS_NAME_MAX) {
        return -VIBEOS_ENAMETOOLONG;
    }
    if ((len == 1u && c[0] == '.') || (len == 2u && c[0] == '.' && c[1] == '.')) {
        return -VIBEOS_EEXIST;
    }
    *dir = cur;
    *name = c;
    *name_len = len;
    return 0;
}

/* A new inode named in `dir`. */
static int tf_make(vibeos_tmpfs_t *t, const char *path, uint32_t mode, uint32_t *out,
                   tf_ctx_t *cx) {
    uint32_t dir, ino;
    const char *name;
    uint32_t len;
    int r = tf_split(t, path, &dir, &name, &len);

    if (r != 0) {
        return r;
    }
    if (tf_find(t, dir, name, len, 0) >= 0) {
        return -VIBEOS_EEXIST;
    }
    r = tf_reserve(t, dir, cx);
    if (r != 0) {
        return r;
    }
    r = tf_alloc_inode(t, mode, &ino);
    if (r != 0) {
        return r;
    }
    t->inode[ino].parent = dir;
    (void)tf_add(t, dir, name, len, ino, cx);   /* reserved: cannot fail */
    if (tf_is(&t->inode[ino], VIBEOS_S_IFDIR)) {
        t->inode[ino].nlink = 2u;
        t->inode[dir].nlink++;
    }
    *out = ino;
    return 0;
}

/* One name fewer for `ino`; the last one frees it. */
static void tf_unref(vibeos_tmpfs_t *t, uint32_t ino) {
    vibeos_tmpfs_inode_t *in = &t->inode[ino];
    if (tf_is(in, VIBEOS_S_IFDIR) || in->nlink <= 1u) {
        tf_free_inode(t, ino);
        return;
    }
    in->nlink--;
    in->ctime_ns = vibeos_fs_now_ns();
}

/* ---- the operations ------------------------------------------------------------- */

#define T ((vibeos_tmpfs_t *)fs)

static int tf_op_lookup(void *fs, const char *path, vibeos_fs_node_t *out) {
    uint32_t i;
    int r;

    T->lock();
    r = tf_resolve(T, path, &i);
    if (r == 0) {
        tf_fill(T, i, out);
    }
    T->unlock();
    return r;
}

static long tf_op_read_at(void *fs, const vibeos_fs_node_t *node, uint64_t off, void *buf,
                          uint32_t len) {
    uint32_t i;
    long r;

    T->lock();
    r = tf_from_id(T, node->id, &i);
    if (r == 0) {
        vibeos_tmpfs_inode_t *in = &T->inode[i];
        if (tf_is(in, VIBEOS_S_IFDIR)) {
            r = -VIBEOS_EISDIR;
        } else {
            r = tf_read(T, in, off, buf, len);
            in->atime_ns = vibeos_fs_now_ns();
        }
    }
    T->unlock();
    return r;
}

/* A page at a time, so each step needs at most TF_SPARES pages and the lock is
 * never held across an allocation. */
static long tf_write_steps(vibeos_tmpfs_t *t, uint64_t id, uint64_t off, const void *buf,
                           uint32_t len) {
    uint32_t done = 0;

    if (len == 0u) {
        return 0;
    }
    while (done < len) {
        uint32_t n = VIBEOS_TMPFS_PAGE - (uint32_t)((off + done) % VIBEOS_TMPFS_PAGE);
        tf_ctx_t cx;
        uint32_t i;
        long w;

        if (n > len - done) {
            n = len - done;
        }
        tf_ctx_fill(t, &cx, TF_SPARES);
        t->lock();
        w = tf_from_id(t, id, &i);
        if (w == 0) {
            w = tf_is(&t->inode[i], VIBEOS_S_IFREG)
                    ? tf_write(t, &t->inode[i], off + done, (const uint8_t *)buf + done, n, &cx)
                    : -VIBEOS_EISDIR;
        }
        t->unlock();
        tf_ctx_drain(t, &cx);
        if (w < 0) {
            return done > 0u ? (long)done : w;
        }
        done += (uint32_t)w;
        if ((uint32_t)w < n) {
            break;
        }
    }
    return (long)done;
}

static long tf_op_write_at(void *fs, const vibeos_fs_node_t *node, uint64_t off, const void *buf,
                           uint32_t len) {
    return tf_write_steps(T, node->id, off, buf, len);
}

static int tf_op_truncate(void *fs, const vibeos_fs_node_t *node, uint64_t size) {
    uint32_t i;
    int r;

    if (size > MAX_PAGES * VIBEOS_TMPFS_PAGE) {
        return -VIBEOS_EFBIG;
    }
    T->lock();
    r = tf_from_id(T, node->id, &i);
    if (r == 0) {
        if (tf_is(&T->inode[i], VIBEOS_S_IFREG)) {
            tf_resize(T, &T->inode[i], size);
            T->inode[i].mtime_ns = T->inode[i].ctime_ns = vibeos_fs_now_ns();
        } else {
            r = -VIBEOS_EISDIR;
        }
    }
    T->unlock();
    return r;
}

/* The whole-file path the VFS started with: replace a file's contents, making
 * it if it is not there. */
static long tf_op_write_file(void *fs, const char *path, const void *buf, uint32_t len) {
    uint32_t i;
    uint64_t id = 0;
    tf_ctx_t cx;
    long r;

    tf_ctx_fill(T, &cx, 2u);
    T->lock();
    r = tf_resolve(T, path, &i);
    if (r == -VIBEOS_ENOENT) {
        r = tf_make(T, path, VIBEOS_S_IFREG | 0644u, &i, &cx);
    }
    if (r == 0 && !tf_is(&T->inode[i], VIBEOS_S_IFREG)) {
        r = -VIBEOS_EISDIR;
    }
    if (r == 0) {
        tf_resize(T, &T->inode[i], 0);
        id = tf_id(T, i);
    }
    T->unlock();
    tf_ctx_drain(T, &cx);
    return r != 0 ? r : tf_write_steps(T, id, 0, buf, len);
}

static int tf_op_list(void *fs, const char *path, uint32_t index, char *name, uint32_t cap,
                      uint64_t *out_size, int *out_is_dir) {
    uint32_t dir, k, ino;
    const char *src;
    uint32_t len;
    int r;

    T->lock();
    r = tf_resolve(T, path, &dir);
    if (r == 0 && !tf_is(&T->inode[dir], VIBEOS_S_IFDIR)) {
        r = -VIBEOS_ENOTDIR;
    }
    if (r == 0) {
        /* "." and ".." first, as Linux lists them. */
        if (index < 2u) {
            src = index == 0u ? "." : "..";
            len = index + 1u;
            ino = index == 0u ? dir : T->inode[dir].parent;
        } else if (index - 2u < T->inode[dir].size) {
            tf_dirent_t *e = tf_dirent(T, &T->inode[dir], index - 2u, 0);
            if (e) {
                src = e->name;
                len = e->len;
                ino = e->ino;
            } else {
                /* A record the directory counts and has no page for. */
                vibeos_mbz_hit(VIBEOS_MBZ_TMPFS_BAD_RECORD, dir);
                r = -1;
            }
        } else {
            r = -1;   /* the end */
        }
    }
    if (r == 0) {
        for (k = 0; k < len && k + 1u < cap; k++) {
            name[k] = src[k];
        }
        name[k] = 0;
        if (out_size) {
            *out_size = tf_is(&T->inode[ino], VIBEOS_S_IFDIR) ? 0u : T->inode[ino].size;
        }
        if (out_is_dir) {
            *out_is_dir = tf_is(&T->inode[ino], VIBEOS_S_IFDIR);
        }
    }
    T->unlock();
    return r == 0 ? 0 : -1;
}

static int tf_op_unlink(void *fs, const char *path) {
    uint32_t dir, ino, len;
    const char *name;
    int64_t slot;
    int r;

    T->lock();
    r = tf_split(T, path, &dir, &name, &len);
    if (r == 0) {
        slot = tf_find(T, dir, name, len, &ino);
        if (slot < 0) {
            r = -VIBEOS_ENOENT;
        } else if (tf_is(&T->inode[ino], VIBEOS_S_IFDIR)) {
            r = -VIBEOS_EISDIR;
        } else {
            tf_remove(T, dir, (uint64_t)slot);
            tf_unref(T, ino);
        }
    }
    T->unlock();
    return r;
}

static int tf_op_mkdir(void *fs, const char *path) {
    uint32_t ino;
    int r;

    tf_ctx_t cx;

    tf_ctx_fill(T, &cx, 2u);
    T->lock();
    r = tf_make(T, path, VIBEOS_S_IFDIR | 0755u, &ino, &cx);
    T->unlock();
    tf_ctx_drain(T, &cx);
    return r;
}

static int tf_op_create(void *fs, const char *path, uint32_t mode, vibeos_fs_node_t *out) {
    uint32_t ino;
    int r;

    tf_ctx_t cx;

    tf_ctx_fill(T, &cx, 2u);
    T->lock();
    r = tf_make(T, path, VIBEOS_S_IFREG | (mode & 07777u), &ino, &cx);
    if (r == 0) {
        tf_fill(T, ino, out);
    }
    T->unlock();
    tf_ctx_drain(T, &cx);
    return r;
}

static int tf_op_rmdir(void *fs, const char *path) {
    uint32_t dir, ino, len;
    const char *name;
    int64_t slot;
    int r;

    T->lock();
    r = tf_split(T, path, &dir, &name, &len);
    if (r == -VIBEOS_EEXIST) {
        r = -VIBEOS_EBUSY;   /* the root of the filesystem, or "." / ".." */
    }
    if (r == 0) {
        slot = tf_find(T, dir, name, len, &ino);
        if (slot < 0) {
            r = -VIBEOS_ENOENT;
        } else if (!tf_is(&T->inode[ino], VIBEOS_S_IFDIR)) {
            r = -VIBEOS_ENOTDIR;
        } else if (T->inode[ino].size != 0u) {
            r = -VIBEOS_ENOTEMPTY;
        } else {
            tf_remove(T, dir, (uint64_t)slot);
            T->inode[dir].nlink--;
            tf_free_inode(T, ino);
        }
    }
    T->unlock();
    return r;
}

static int tf_op_rename(void *fs, const char *from, const char *to, uint32_t flags) {
    uint32_t fdir, tdir, src, dst = 0, flen, tlen, x;
    const char *fname, *tname;
    int64_t fslot, tslot;
    int sdir, r;
    tf_ctx_t cx;

    tf_ctx_fill(T, &cx, 2u);
    T->lock();
    r = tf_split(T, from, &fdir, &fname, &flen);
    if (r == -VIBEOS_EEXIST) {
        r = -VIBEOS_EBUSY;
    }
    if (r != 0) {
        goto out;
    }
    fslot = tf_find(T, fdir, fname, flen, &src);
    if (fslot < 0) {
        r = -VIBEOS_ENOENT;
        goto out;
    }
    r = tf_split(T, to, &tdir, &tname, &tlen);
    if (r == -VIBEOS_EEXIST) {
        r = -VIBEOS_EBUSY;
    }
    if (r != 0) {
        goto out;
    }
    tslot = tf_find(T, tdir, tname, tlen, &dst);
    /* NOREPLACE first: Linux refuses a taken name before it asks whether the
     * name is another link to the same file. */
    if (tslot >= 0 && (flags & VIBEOS_RENAME_NOREPLACE)) {
        r = -VIBEOS_EEXIST;
        goto out;
    }
    if (tslot >= 0 && dst == src) {
        goto out;   /* two names of one file: POSIX says do nothing */
    }
    sdir = tf_is(&T->inode[src], VIBEOS_S_IFDIR);
    if (sdir) {
        /* A directory cannot move inside itself: the tree would lose it. */
        for (x = tdir;; x = T->inode[x].parent) {
            if (x == src) {
                r = -VIBEOS_EINVAL;
                goto out;
            }
            if (x == 0u) {
                break;
            }
        }
        if (tslot >= 0 && !tf_is(&T->inode[dst], VIBEOS_S_IFDIR)) {
            r = -VIBEOS_ENOTDIR;
            goto out;
        }
        if (tslot >= 0 && T->inode[dst].size != 0u) {
            r = -VIBEOS_ENOTEMPTY;
            goto out;
        }
    } else if (tslot >= 0 && tf_is(&T->inode[dst], VIBEOS_S_IFDIR)) {
        r = -VIBEOS_EISDIR;
        goto out;
    }
    if (tslot < 0) {
        r = tf_reserve(T, tdir, &cx);   /* before anything moves */
        if (r != 0) {
            goto out;
        }
    }
    /* Checked; now nothing below can fail. The replaced name goes first, and
     * the source's record is found again after, because removing a record
     * moves the directory's last one into its place. */
    if (tslot >= 0) {
        tf_remove(T, tdir, (uint64_t)tslot);
        if (tf_is(&T->inode[dst], VIBEOS_S_IFDIR)) {
            T->inode[tdir].nlink--;
        }
        tf_unref(T, dst);
    }
    fslot = tf_find(T, fdir, fname, flen, 0);
    tf_remove(T, fdir, (uint64_t)fslot);
    (void)tf_add(T, tdir, tname, tlen, src, &cx);
    if (sdir && fdir != tdir) {
        T->inode[fdir].nlink--;
        T->inode[tdir].nlink++;
        T->inode[src].parent = tdir;
    }
    T->inode[src].ctime_ns = vibeos_fs_now_ns();
out:
    T->unlock();
    tf_ctx_drain(T, &cx);
    return r;
}

static int tf_op_link(void *fs, const char *existing, const char *path) {
    uint32_t src, dir, len;
    const char *name;
    int r;
    tf_ctx_t cx;

    tf_ctx_fill(T, &cx, 2u);
    T->lock();
    r = tf_resolve(T, existing, &src);
    if (r == 0 && tf_is(&T->inode[src], VIBEOS_S_IFDIR)) {
        r = -VIBEOS_EPERM;   /* Linux refuses a hard link to a directory */
    }
    if (r == 0 && T->inode[src].nlink >= 65000u) {
        r = -VIBEOS_EMLINK;
    }
    if (r == 0) {
        r = tf_split(T, path, &dir, &name, &len);
    }
    if (r == 0 && tf_find(T, dir, name, len, 0) >= 0) {
        r = -VIBEOS_EEXIST;
    }
    if (r == 0) {
        r = tf_add(T, dir, name, len, src, &cx);
    }
    if (r == 0) {
        T->inode[src].nlink++;
        T->inode[src].ctime_ns = vibeos_fs_now_ns();
    }
    T->unlock();
    tf_ctx_drain(T, &cx);
    return r;
}

static int tf_op_symlink(void *fs, const char *target, const char *path) {
    uint32_t ino, len = 0;
    long w;
    int r;
    tf_ctx_t cx;

    while (target[len]) {
        len++;
    }
    if (len == 0u) {
        return -VIBEOS_ENOENT;   /* Linux: an empty target is refused */
    }
    if (len >= VIBEOS_PATH_MAX) {
        return -VIBEOS_ENAMETOOLONG;
    }
    tf_ctx_fill(T, &cx, 3u);
    T->lock();
    r = tf_make(T, path, VIBEOS_S_IFLNK | 0777u, &ino, &cx);
    if (r == 0) {
        w = tf_write(T, &T->inode[ino], 0, target, len, &cx);
        if (w != (long)len) {
            /* No page for the target: the link must not exist half-made. */
            uint32_t dir, nl;
            const char *name;
            int64_t slot;
            (void)tf_split(T, path, &dir, &name, &nl);
            slot = tf_find(T, dir, name, nl, 0);
            if (slot >= 0) {
                tf_remove(T, dir, (uint64_t)slot);
            }
            tf_free_inode(T, ino);
            r = -VIBEOS_ENOSPC;
        }
    }
    T->unlock();
    tf_ctx_drain(T, &cx);
    return r;
}

static long tf_op_readlink(void *fs, const char *path, char *buf, uint32_t cap) {
    uint32_t ino;
    long r;

    T->lock();
    r = tf_resolve(T, path, &ino);
    if (r == 0) {
        r = tf_is(&T->inode[ino], VIBEOS_S_IFLNK) ? tf_read(T, &T->inode[ino], 0, buf, cap)
                                                   : -VIBEOS_EINVAL;
    }
    T->unlock();
    return r;
}

static int tf_op_setattr(void *fs, const char *path, const vibeos_fs_attr_t *a) {
    uint32_t ino;
    int r;

    T->lock();
    r = tf_resolve(T, path, &ino);
    if (r == 0) {
        vibeos_tmpfs_inode_t *in = &T->inode[ino];
        if (a->valid & VIBEOS_ATTR_MODE) {
            in->mode = (in->mode & VIBEOS_S_IFMT) | (a->mode & 07777u);
        }
        if (a->valid & VIBEOS_ATTR_UID) {
            in->uid = a->uid;
        }
        if (a->valid & VIBEOS_ATTR_GID) {
            in->gid = a->gid;
        }
        if (a->valid & VIBEOS_ATTR_ATIME) {
            in->atime_ns = a->atime_ns;
        }
        if (a->valid & VIBEOS_ATTR_MTIME) {
            in->mtime_ns = a->mtime_ns;
        }
        in->ctime_ns = vibeos_fs_now_ns();
    }
    T->unlock();
    return r;
}

static int tf_op_statfs(void *fs, vibeos_fs_statfs_t *out) {
    T->lock();
    out->magic = VIBEOS_TMPFS_MAGIC;
    out->block_size = VIBEOS_TMPFS_PAGE;
    out->blocks = T->pages_max;
    out->blocks_free = T->pages_max - T->pages_used;
    out->files = VIBEOS_TMPFS_INODES;
    out->files_free = VIBEOS_TMPFS_INODES - T->inodes_used;
    out->name_max = VIBEOS_NAME_MAX;
    out->read_only = 0;
    T->unlock();
    return 0;
}

#undef T

static const vibeos_fs_ops_t g_tmpfs_ops = {
    .lookup = tf_op_lookup,
    .read_at = tf_op_read_at,
    .write_file = tf_op_write_file,
    .list = tf_op_list,
    .unlink = tf_op_unlink,
    .mkdir = tf_op_mkdir,
    .write_at = tf_op_write_at,
    .truncate = tf_op_truncate,
    .create = tf_op_create,
    .rmdir = tf_op_rmdir,
    .rename = tf_op_rename,
    .link = tf_op_link,
    .symlink = tf_op_symlink,
    .readlink = tf_op_readlink,
    .setattr = tf_op_setattr,
    .statfs = tf_op_statfs,
};

const vibeos_fs_ops_t *vibeos_tmpfs_ops(void) {
    return &g_tmpfs_ops;
}

int vibeos_tmpfs_init(vibeos_tmpfs_t *t, uint64_t pages_max,
                      void *(*page_alloc)(void), void (*page_free)(void *page),
                      void (*lock)(void), void (*unlock)(void)) {
    uint64_t now;

    if (!t || !page_alloc || !page_free || !lock || !unlock) {
        return -1;
    }
    tf_zero(t, sizeof(*t));
    t->page_alloc = page_alloc;
    t->page_free = page_free;
    t->lock = lock;
    t->unlock = unlock;
    t->pages_max = pages_max;
    now = vibeos_fs_now_ns();
    t->inode[0].mode = VIBEOS_S_IFDIR | 01777u;   /* /tmp: anyone, sticky */
    t->inode[0].nlink = 2u;
    t->inode[0].parent = 0;
    t->inode[0].atime_ns = t->inode[0].mtime_ns = t->inode[0].ctime_ns = now;
    t->inodes_used = 1u;
    return 0;
}

void vibeos_tmpfs_destroy(vibeos_tmpfs_t *t) {
    uint32_t i;

    t->lock();
    for (i = 0; i < VIBEOS_TMPFS_INODES; i++) {
        if (t->inode[i].mode != 0u) {
            tf_drop_pages(t, &t->inode[i], 0);
            t->inode[i].mode = 0;
            t->inode[i].gen++;
        }
    }
    t->inodes_used = 0;
    t->unlock();
}
