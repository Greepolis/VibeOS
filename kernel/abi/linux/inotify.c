/* Linux ABI: inotify (docs/abi/ L4 step 6) - what happened to files, read as
 * records.
 *
 * The file layer says what happened, once, where every filesystem's operation
 * passes (vibeos_fs_notify in kernel/fs/vfs.c, and the regular-file type for
 * open, read, write and close): a mount, the path inside it, the node it was
 * about, an event in the file layer's numbering (VIBEOS_FSN_*), and a cookie
 * that ties the two halves of a rename. Everything after that is here: which
 * watches name that node or its directory, and the records Linux's read
 * returns. A watch names a node - a mount and the filesystem's identity for it -
 * so it follows a rename on a filesystem that keeps identities (tmpfs) and not
 * on one that does not (FAT, whose identity is where its entry sits); that is
 * FAT's gap, written down in kernel/fs/fat.c.
 *
 * Nothing here runs for a boot that never calls inotify_init: the file layer
 * asks vibeos_fs_notify_active() before it looks anything up. */

#include "linux_internal.h"

#define IN_INSTANCES 8u
#define IN_QUEUE 32u         /* events an instance holds before IN_Q_OVERFLOW */
#define IN_WATCHES 128u      /* in every instance together */
#define IN_NAME 256u

typedef struct {
    int32_t wd;
    uint32_t mask;
    uint32_t cookie;
    char name[IN_NAME];
} linux_in_event_t;

typedef struct {
    vibeos_file_t *file;     /* 0 for a free instance */
    linux_in_event_t q[IN_QUEUE];
    uint32_t head, count;
    int overflowed;          /* the overflow record is queued: nothing more until it is read */
    int32_t next_wd;
} linux_in_instance_t;

typedef struct {
    int inst;                /* -1 for a free watch */
    int32_t wd;
    vibeos_fsmount_t *mnt;
    uint64_t id;
    uint32_t mask;           /* Linux's bits: events and IN_ONESHOT, IN_EXCL_UNLINK */
} linux_in_watch_t;

static linux_in_instance_t g_in[IN_INSTANCES];
static linux_in_watch_t g_inw[IN_WATCHES];
static vibeos_lock_t g_in_lock;
/* What an instance holds while its description is being made: not a free slot,
 * and not any description - a mark, not a pointer to follow. */
static vibeos_file_t g_in_claimed;
#define IN_CLAIMED (&g_in_claimed)
/* The file layer's numbering is Linux's for the events both have; the flags are
 * Linux's own. */
#define IN_EVENTS (LINUX_IN_ACCESS | LINUX_IN_MODIFY | LINUX_IN_ATTRIB | LINUX_IN_CLOSE_WRITE | \
                   LINUX_IN_CLOSE_NOWRITE | LINUX_IN_OPEN | LINUX_IN_MOVED_FROM | LINUX_IN_MOVED_TO | \
                   LINUX_IN_CREATE | LINUX_IN_DELETE | LINUX_IN_DELETE_SELF | LINUX_IN_MOVE_SELF)
#define IN_ADD_FLAGS (LINUX_IN_DONT_FOLLOW | LINUX_IN_EXCL_UNLINK | LINUX_IN_MASK_ADD | LINUX_IN_ONESHOT | \
                      LINUX_IN_ONLYDIR | LINUX_IN_MASK_CREATE)

static uint32_t in_linux_of(uint32_t fsn) {
    uint32_t m = 0;

    m |= (fsn & VIBEOS_FSN_ACCESS) ? LINUX_IN_ACCESS : 0u;
    m |= (fsn & VIBEOS_FSN_MODIFY) ? LINUX_IN_MODIFY : 0u;
    m |= (fsn & VIBEOS_FSN_ATTRIB) ? LINUX_IN_ATTRIB : 0u;
    m |= (fsn & VIBEOS_FSN_CLOSE_WRITE) ? LINUX_IN_CLOSE_WRITE : 0u;
    m |= (fsn & VIBEOS_FSN_CLOSE_NOWRITE) ? LINUX_IN_CLOSE_NOWRITE : 0u;
    m |= (fsn & VIBEOS_FSN_OPEN) ? LINUX_IN_OPEN : 0u;
    m |= (fsn & VIBEOS_FSN_MOVED_FROM) ? LINUX_IN_MOVED_FROM : 0u;
    m |= (fsn & VIBEOS_FSN_MOVED_TO) ? LINUX_IN_MOVED_TO : 0u;
    m |= (fsn & VIBEOS_FSN_CREATE) ? LINUX_IN_CREATE : 0u;
    m |= (fsn & VIBEOS_FSN_DELETE) ? LINUX_IN_DELETE : 0u;
    m |= (fsn & VIBEOS_FSN_DELETE_SELF) ? LINUX_IN_DELETE_SELF : 0u;
    m |= (fsn & VIBEOS_FSN_MOVE_SELF) ? LINUX_IN_MOVE_SELF : 0u;
    return m;
}

/* Queue one record, under the lock. Two identical records in a row are one, as
 * Linux merges them; a full queue gets one IN_Q_OVERFLOW and then nothing. */
static void in_queue(linux_in_instance_t *in, int32_t wd, uint32_t mask, uint32_t cookie, const char *name) {
    linux_in_event_t *e;
    uint32_t k;

    if (in->overflowed) {
        return;
    }
    if (in->count != 0u) {
        linux_in_event_t *last = &in->q[(in->head + in->count - 1u) % IN_QUEUE];

        for (k = 0; name[k] && name[k] == last->name[k]; k++) {
        }
        if (last->wd == wd && last->mask == mask && last->cookie == cookie && name[k] == last->name[k]) {
            return;
        }
    }
    if (in->count == IN_QUEUE - 1u) {
        wd = -1;
        mask = LINUX_IN_Q_OVERFLOW;
        cookie = 0;
        name = "";
        in->overflowed = 1;
    }
    e = &in->q[(in->head + in->count) % IN_QUEUE];
    e->wd = wd;
    e->mask = mask;
    e->cookie = cookie;
    for (k = 0; name[k] && k + 1u < IN_NAME; k++) {
        e->name[k] = name[k];
    }
    e->name[k] = 0;
    in->count++;
}

/* A watch goes: IN_IGNORED to its instance, and the slot is free. */
static void in_drop_watch(linux_in_watch_t *w) {
    in_queue(&g_in[w->inst], w->wd, LINUX_IN_IGNORED, 0, "");
    w->inst = -1;
}

/* Every watch on (mnt, id) asking for any of `mask`: a record, named if the
 * watch is on the directory the event happened in. */
static void in_deliver(vibeos_fsmount_t *mnt, uint64_t id, uint32_t mask, uint32_t isdir, uint32_t cookie,
                       const char *name) {
    uint32_t i;

    for (i = 0; i < IN_WATCHES; i++) {
        linux_in_watch_t *w = &g_inw[i];

        if (w->inst < 0 || w->mnt != mnt || w->id != id || !(w->mask & mask)) {
            continue;
        }
        in_queue(&g_in[w->inst], w->wd, (w->mask & mask) | isdir, cookie, name);
        if (w->mask & LINUX_IN_ONESHOT) {
            in_drop_watch(w);
        }
    }
}

/* The hook (vibeos_fs_set_notify): what happened at `path` inside `mnt`, to the
 * node `id`. The directory is looked up here, outside the lock - a lookup is
 * the filesystem's and may take its own. */
static void in_notify(vibeos_fsmount_t *mnt, const char *path, uint64_t id, uint32_t fsn, uint32_t cookie,
                      int is_dir) {
    char dir[VIBEOS_FILE_PATH];
    const char *name = path;
    vibeos_fs_node_t parent;
    int have_parent;
    uint32_t mask = in_linux_of(fsn), k, slash = 0, isdir = is_dir ? LINUX_IN_ISDIR : 0u, i;

    for (k = 0; path[k] && k + 1u < sizeof(dir); k++) {
        dir[k] = path[k];
        if (path[k] == '/') {
            slash = k;
            name = path + k + 1u;
        }
    }
    dir[slash] = 0;
    if (dir[0] == 0) {
        dir[0] = '/';   /* a driver spells its root "/" */
        dir[1] = 0;
    }
    /* A mount's root has no directory inside the mount to tell, and a link
     * count is the node's own business: Linux's IN_ATTRIB for it goes to the
     * node alone (LTP's inotify04 watches a directory and a file in it). */
    if (fsn & VIBEOS_FSN_NLINK) {
        mask |= LINUX_IN_ATTRIB;
    }
    have_parent = name[0] != 0 && !(fsn & VIBEOS_FSN_NLINK) && vibeos_fs_lookup(mnt, dir, &parent) == 0;
    ks_lock(&g_in_lock, __func__);
    /* Its directory first: everything but what only the node hears, named.
     * Linux tells the parent before the node, and one instance watching both
     * reads them in that order (LTP's inotify10; it was the other way). */
    if (have_parent) {
        in_deliver(mnt, parent.id, mask & ~(uint32_t)(LINUX_IN_DELETE_SELF | LINUX_IN_MOVE_SELF), isdir, cookie,
                   name);
    }
    /* Then the node itself: everything but the directory's events, unnamed. A
     * directory says it is one, except in DELETE_SELF and MOVE_SELF, which
     * Linux reports bare (LTP's inotify02 and 04 ask both ways). */
    in_deliver(mnt, id, mask & ~(uint32_t)(LINUX_IN_CREATE | LINUX_IN_DELETE | LINUX_IN_MOVED_FROM |
                                           LINUX_IN_MOVED_TO),
               (mask & (LINUX_IN_DELETE_SELF | LINUX_IN_MOVE_SELF)) ? 0u : isdir, 0, "");
    /* A node deleted is a watch gone. */
    if (mask & LINUX_IN_DELETE_SELF) {
        for (i = 0; i < IN_WATCHES; i++) {
            if (g_inw[i].inst >= 0 && g_inw[i].mnt == mnt && g_inw[i].id == id) {
                in_drop_watch(&g_inw[i]);
            }
        }
    }
    ks_unlock(&g_in_lock);
    ks_wake_waiters();
}

/* ---- the file type --------------------------------------------------------------------- */

static linux_in_instance_t *in_of(vibeos_file_t *f) {
    return &g_in[f->ev_index];
}

/* A record's length: the header and the name padded with zeroes to a multiple of
 * the header, as Linux pads it; a record with no name has no name field. */
static uint32_t in_name_len(const linux_in_event_t *e) {
    uint32_t n = 0;

    while (e->name[n]) {
        n++;
    }
    return n == 0u ? 0u : (n + 1u + (uint32_t)sizeof(linux_inotify_event_t) - 1u) /
                              (uint32_t)sizeof(linux_inotify_event_t) * (uint32_t)sizeof(linux_inotify_event_t);
}

/* A read hands out whole records, as many as fit; a buffer too small for the
 * first is EINVAL. */
static long in_read(vibeos_file_t *f, uint64_t buf, uint64_t len) {
    linux_in_instance_t *in = in_of(f);

    for (;;) {
        uint64_t done = 0;
        long r = 0;

        ks_lock(&g_in_lock, __func__);
        while (in->count != 0u) {
            linux_in_event_t *e = &in->q[in->head];
            uint32_t nl = in_name_len(e);
            linux_inotify_event_t h;
            char pad[IN_NAME + 16];
            uint32_t k;

            if (done + sizeof(h) + nl > len) {
                if (done == 0u) {
                    r = -VIBEOS_EINVAL;
                }
                break;
            }
            h.wd = e->wd;
            h.mask = e->mask;
            h.cookie = e->cookie;
            h.len = nl;
            for (k = 0; k < nl; k++) {
                pad[k] = 0;
            }
            for (k = 0; e->name[k]; k++) {
                pad[k] = e->name[k];
            }
            if (vibeos_uaccess_copy((void *)(uintptr_t)(buf + done), &h, sizeof(h)) != 0 ||
                (nl != 0u && vibeos_uaccess_copy((void *)(uintptr_t)(buf + done + sizeof(h)), pad, nl) != 0)) {
                if (done == 0u) {
                    r = -VIBEOS_EFAULT;
                }
                break;
            }
            if (e->mask == LINUX_IN_Q_OVERFLOW) {
                in->overflowed = 0;
            }
            in->head = (in->head + 1u) % IN_QUEUE;
            in->count--;
            done += sizeof(h) + nl;
        }
        ks_unlock(&g_in_lock);
        if (r != 0) {
            return r;
        }
        if (done != 0u) {
            return (long)done;
        }
        if (f->flags & VIBEOS_O_NONBLOCK) {
            return -VIBEOS_EAGAIN;
        }
        if (ks_current() >= 0 && ks_signal_interrupts(ks_current())) {
            return -VIBEOS_RESTART_CALL;
        }
        ks_block_point();
    }
}

static uint32_t in_ready(vibeos_file_t *f) {
    return __atomic_load_n(&in_of(f)->count, __ATOMIC_ACQUIRE) != 0u ? VIBEOS_READY_IN : 0u;
}

static int in_stat(vibeos_file_t *f, vibeos_file_stat_t *out) {
    (void)f;
    out->mode = VIBEOS_S_IFREG | 0600u;   /* Linux's anonymous inode */
    out->size = 0;
    return 0;
}

/* The instance goes, and its watches with it, without IN_IGNORED: nobody is
 * left to read them. */
static void in_release(vibeos_file_t *f) {
    int at = f->ev_index;
    uint32_t i, used = 0;

    ks_lock(&g_in_lock, __func__);
    for (i = 0; i < IN_WATCHES; i++) {
        if (g_inw[i].inst == at) {
            g_inw[i].inst = -1;
        }
    }
    g_in[at].file = 0;
    for (i = 0; i < IN_INSTANCES; i++) {
        used += g_in[i].file != 0;
    }
    /* Under the lock, as inotify_init1 turns it on: decided here and done
     * after unlocking, an init1 on another core could create an instance and
     * turn notification on in between, and this would then turn it off under
     * a live instance that never heard of another file again (external
     * review, 2026-10-07). */
    if (used == 0u) {
        vibeos_fs_set_notify(0);   /* nothing listens: the file layer stops asking */
    }
    ks_unlock(&g_in_lock);
}

/* How many bytes a read would hand out now: FIONREAD's answer (fs.c). */
uint32_t linux_inotify_pending(vibeos_file_t *f) {
    linux_in_instance_t *in = in_of(f);
    uint32_t n = 0, i;

    ks_lock(&g_in_lock, __func__);
    for (i = 0; i < in->count; i++) {
        n += (uint32_t)sizeof(linux_inotify_event_t) + in_name_len(&in->q[(in->head + i) % IN_QUEUE]);
    }
    ks_unlock(&g_in_lock);
    return n;
}

const vibeos_file_ops_t linux_fops_inotify = {
    .name = "inotify",
    .read = in_read,
    .stat = in_stat,
    .ready = in_ready,
    .release = in_release,
};

void linux_inotify_init(void) {
    uint32_t i;

    for (i = 0; i < IN_INSTANCES; i++) {
        g_in[i].file = 0;
    }
    for (i = 0; i < IN_WATCHES; i++) {
        g_inw[i].inst = -1;
    }
    vibeos_fs_set_notify(0);
}

/* What /proc/sys/fs/inotify says (procsrc.c): the events before the overflow
 * record - one slot of the queue is kept for it - the instances, the watches. */
void linux_inotify_limits(uint32_t *queued, uint32_t *instances, uint32_t *watches) {
    *queued = IN_QUEUE - 1u;
    *instances = IN_INSTANCES;
    *watches = IN_WATCHES;
}

/* ---- the calls ------------------------------------------------------------------------- */

static long linux_sys_inotify_init1(uint64_t flags) {
    vibeos_file_t *f;
    uint32_t i;

    if (flags & ~(uint64_t)(LINUX_IN_NONBLOCK | LINUX_IN_CLOEXEC)) {
        return -VIBEOS_EINVAL;
    }
    /* The instance first, held by a mark until its description exists: a
     * description that failed to get one would have nothing to release. */
    ks_lock(&g_in_lock, __func__);
    for (i = 0; i < IN_INSTANCES && g_in[i].file != 0; i++) {
    }
    if (i < IN_INSTANCES) {
        g_in[i].file = IN_CLAIMED;
        g_in[i].head = 0;
        g_in[i].count = 0;
        g_in[i].overflowed = 0;
        g_in[i].next_wd = 1;
    }
    ks_unlock(&g_in_lock);
    if (i == IN_INSTANCES) {
        return -VIBEOS_EMFILE;        /* Linux's answer past max_user_instances */
    }
    if (!(f = vibeos_file_alloc(&linux_fops_inotify, (flags & LINUX_IN_NONBLOCK) ? VIBEOS_O_NONBLOCK : 0u))) {
        __atomic_store_n(&g_in[i].file, (vibeos_file_t *)0, __ATOMIC_RELEASE);
        return -VIBEOS_ENFILE;
    }
    f->ev_index = (int)i;
    ks_lock(&g_in_lock, __func__);
    __atomic_store_n(&g_in[i].file, f, __ATOMIC_RELEASE);
    vibeos_fs_set_notify(in_notify);   /* with the instance, under the lock: see in_release */
    ks_unlock(&g_in_lock);
    return linux_fd_install(f, (flags & LINUX_IN_CLOEXEC) ? VIBEOS_FD_CLOEXEC : 0u, 0);
}

static long linux_sys_inotify_init(void) {
    return linux_sys_inotify_init1(0);
}

static long in_instance(uint64_t fd, vibeos_file_t **out) {
    vibeos_file_t *f = linux_file_get(fd);

    if (!f) {
        return -VIBEOS_EBADF;
    }
    if (f->ops != &linux_fops_inotify) {
        vibeos_file_put(f);
        return -VIBEOS_EINVAL;
    }
    *out = f;
    return 0;
}

/* inotify_add_watch(fd, path, mask): a watch on the node the path names, or the
 * one already on it changed - replaced, or added to under IN_MASK_ADD, or
 * EEXIST under IN_MASK_CREATE. The same wd either way. */
static long linux_sys_inotify_add_watch(uint64_t fd, uint64_t upath, uint64_t mask64) {
    uint32_t mask = (uint32_t)mask64, i;
    vibeos_file_t *f;
    vibeos_path_t w;
    long r;
    int free_at = -1;

    if ((mask & IN_EVENTS) == 0u || (mask & ~(uint32_t)(IN_EVENTS | IN_ADD_FLAGS)) ||
        ((mask & LINUX_IN_MASK_ADD) && (mask & LINUX_IN_MASK_CREATE))) {
        if ((r = in_instance(fd, &f)) != 0) {
            return r;     /* EBADF first, as Linux checks the descriptor first */
        }
        vibeos_file_put(f);
        return -VIBEOS_EINVAL;
    }
    if ((r = in_instance(fd, &f)) != 0) {
        return r;
    }
    r = linux_walk_at((uint64_t)(int64_t)LINUX_AT_FDCWD, upath,
                      (mask & LINUX_IN_DONT_FOLLOW) ? VIBEOS_PATH_NOFOLLOW : 0u, &w);
    if (r == 0 && (r = linux_may(&w.node, VIBEOS_MAY_READ)) == 0 && (mask & LINUX_IN_ONLYDIR) &&
        (w.node.mode & VIBEOS_S_IFMT) != VIBEOS_S_IFDIR) {
        r = -VIBEOS_ENOTDIR;
    }
    if (r != 0) {
        vibeos_file_put(f);
        return r;
    }
    ks_lock(&g_in_lock, __func__);
    r = -VIBEOS_ENOSPC;
    for (i = 0; i < IN_WATCHES; i++) {
        linux_in_watch_t *x = &g_inw[i];

        if (x->inst == f->ev_index && x->mnt == w.mnt && x->id == w.node.id) {
            if (mask & LINUX_IN_MASK_CREATE) {
                r = -VIBEOS_EEXIST;
            } else {
                x->mask = (mask & LINUX_IN_MASK_ADD) ? (x->mask | mask) : mask;
                x->mask &= ~(uint32_t)(LINUX_IN_MASK_ADD | LINUX_IN_DONT_FOLLOW | LINUX_IN_ONLYDIR);
                r = x->wd;
            }
            break;
        }
        if (x->inst < 0 && free_at < 0) {
            free_at = (int)i;
        }
    }
    if (i == IN_WATCHES && free_at >= 0) {
        linux_in_watch_t *x = &g_inw[free_at];

        x->inst = f->ev_index;
        x->wd = g_in[f->ev_index].next_wd++;
        x->mnt = w.mnt;
        x->id = w.node.id;
        x->mask = mask & ~(uint32_t)(LINUX_IN_MASK_ADD | LINUX_IN_MASK_CREATE | LINUX_IN_DONT_FOLLOW |
                                     LINUX_IN_ONLYDIR);
        r = x->wd;
    }
    ks_unlock(&g_in_lock);
    vibeos_file_put(f);
    return r;
}

static long linux_sys_inotify_rm_watch(uint64_t fd, uint64_t wd) {
    vibeos_file_t *f;
    uint32_t i;
    long r;

    if ((r = in_instance(fd, &f)) != 0) {
        return r;
    }
    r = -VIBEOS_EINVAL;
    ks_lock(&g_in_lock, __func__);
    for (i = 0; i < IN_WATCHES; i++) {
        if (g_inw[i].inst == f->ev_index && g_inw[i].wd == VIBEOS_ARG_INT(wd)) {
            in_drop_watch(&g_inw[i]);
            r = 0;
            break;
        }
    }
    ks_unlock(&g_in_lock);
    vibeos_file_put(f);
    if (r == 0) {
        ks_wake_waiters();
    }
    return r;
}

#define LINUX_INOTIFY_SYSCALLS(X) \
    X(253, inotify_init,      INOTIFY, NOPTR, linux_sys_inotify_init()) \
    X(294, inotify_init1,     INOTIFY, NOPTR, linux_sys_inotify_init1(ARG(0))) \
    X(254, inotify_add_watch, INOTIFY, NOPTR, linux_sys_inotify_add_watch(ARG(0), ARG(1), ARG(2))) \
    X(255, inotify_rm_watch,  INOTIFY, NOPTR, linux_sys_inotify_rm_watch(ARG(0), ARG(1)))

LINUX_DEFINE_SYSCALLS(inotify, LINUX_INOTIFY_SYSCALLS)
