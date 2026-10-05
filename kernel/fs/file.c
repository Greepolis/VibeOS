/* Open file descriptions. See include/vibeos/file.h. */

#include "vibeos/file.h"
#include "vibeos/mbz.h"
#include "vibeos/filelock.h"

static vibeos_file_t g_files[VIBEOS_FILE_MAX];
static void (*g_lock)(void);
static void (*g_unlock)(void);

static void lock(void) {
    if (g_lock) {
        g_lock();
    }
}

static void unlock(void) {
    if (g_unlock) {
        g_unlock();
    }
}

void vibeos_file_set_lock(void (*l)(void), void (*u)(void)) {
    g_lock = l;
    g_unlock = u;
}

static void file_clear(vibeos_file_t *f) {
    unsigned char *raw = (unsigned char *)f;
    uint32_t gen = f->gen, i;

    for (i = 0; i < (uint32_t)sizeof(*f); i++) {
        raw[i] = 0;
    }
    f->gen = gen;
    f->pipe = -1;
    f->sock = -1;
}

void vibeos_file_reset(void) {
    uint32_t i;

    lock();
    for (i = 0; i < VIBEOS_FILE_MAX; i++) {
        file_clear(&g_files[i]);
        g_files[i].gen = 0;
    }
    unlock();
}

vibeos_file_t *vibeos_file_alloc(const vibeos_file_ops_t *ops, uint32_t flags) {
    uint32_t i;
    vibeos_file_t *f = 0;

    if (!ops) {
        return 0;
    }
    lock();
    for (i = 0; i < VIBEOS_FILE_MAX; i++) {
        /* Free is both: no reference, and no release still running. The last
         * put takes refs to zero before it runs the type's release, so refs
         * alone would hand out a slot whose pipe end is still being given back. */
        if (g_files[i].refs == 0u && g_files[i].ops == 0) {
            f = &g_files[i];
            file_clear(f);
            f->gen++;
            f->ops = ops;
            f->flags = flags & ~VIBEOS_O_CLOEXEC;
            __atomic_store_n(&f->refs, 1u, __ATOMIC_RELEASE);
            break;
        }
    }
    unlock();
    return f;
}

void vibeos_file_get(vibeos_file_t *f) {
    if (f) {
        (void)__atomic_add_fetch(&f->refs, 1u, __ATOMIC_ACQ_REL);
    }
}

void vibeos_file_paths_moved(const char *from, const char *to) {
    uint32_t i;

    lock();
    for (i = 0; i < VIBEOS_FILE_MAX; i++) {
        vibeos_file_t *f = &g_files[i];
        char out[VIBEOS_FILE_PATH];
        uint32_t n = 0, k = 0;

        if (f->ops == 0 || f->path[0] == 0) {
            continue;
        }
        while (from[n] && from[n] == f->path[n]) {
            n++;
        }
        if (from[n] != 0 || (f->path[n] != 0 && f->path[n] != '/')) {
            continue;
        }
        while (to[k] && k + 1u < sizeof(out)) {
            out[k] = to[k];
            k++;
        }
        while (f->path[n] && k + 1u < sizeof(out)) {
            out[k++] = f->path[n++];
        }
        if (f->path[n] != 0) {
            continue;   /* would not fit: left as it was */
        }
        out[k] = 0;
        for (n = 0; n <= k; n++) {
            f->path[n] = out[n];
        }
    }
    unlock();
}

int vibeos_file_try_get(vibeos_file_t *f) {
    uint32_t n;

    if (!f) {
        return 0;
    }
    n = __atomic_load_n(&f->refs, __ATOMIC_ACQUIRE);
    while (n != 0u) {
        if (__atomic_compare_exchange_n(&f->refs, &n, n + 1u, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            return 1;
        }
    }
    return 0;
}

static void (*g_on_release)(vibeos_file_t *f);

void vibeos_file_on_release(void (*fn)(vibeos_file_t *f)) {
    g_on_release = fn;
}

void vibeos_file_put(vibeos_file_t *f) {
    uint32_t n;

    if (!f) {
        return;
    }
    /* Never below zero: a second release of the same reference would otherwise
     * run the type's release twice - a pipe end given back twice, a socket closed
     * under whoever was handed its slot - and then wrap, so nothing released
     * the description again. The same rule as a thread leaving a table. */
    for (;;) {
        n = __atomic_load_n(&f->refs, __ATOMIC_ACQUIRE);
        if (n == 0u) {
            vibeos_mbz_hit(VIBEOS_MBZ_FILE_PUT_UNDERFLOW, (uint64_t)(uintptr_t)f);
            return;
        }
        if (__atomic_compare_exchange_n(&f->refs, &n, n - 1u, 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
            break;
        }
    }
    if (n != 1u) {
        return;
    }
    /* Nobody else can reach it now: the count was the last thing that said
     * somebody might. The release runs without this layer's lock, because it
     * wakes pipe readers and writes files back. The slot stays taken while it
     * runs - alloc wants no references *and* no ops - and is handed back here,
     * under the lock alloc takes. */
    /* What the description itself held of locks - a flock, an OFD lock - goes
     * with it: that is the whole of their lifetime rule. */
    if (vibeos_flk_count() != 0u) {
        vibeos_flk_drop_owner(VIBEOS_FLK_OWNER_FILE(f));
    }
    if (g_on_release) {
        g_on_release(f);
    }
    if (f->ops && f->ops->release) {
        f->ops->release(f);
    }
    lock();
    f->ops = 0;
    unlock();
}

void vibeos_file_stat_clear(vibeos_file_stat_t *st) {
    uint8_t *p = (uint8_t *)st;
    uint32_t i;
    for (i = 0; i < sizeof(*st); i++) {
        p[i] = 0;
    }
}

void vibeos_file_stat_from_node(vibeos_file_stat_t *st, const vibeos_fs_node_t *node) {
    vibeos_file_stat_clear(st);
    st->mode = node->mode;
    st->size = node->is_dir ? 0u : node->size;   /* 64-bit: do not narrow (M-018) */
    st->ino = node->id ? node->id : 2u;
    st->nlink = node->nlink;
    st->uid = node->uid;
    st->gid = node->gid;
    st->rdev = node->rdev;
    st->atime_ns = node->atime_ns;
    st->mtime_ns = node->mtime_ns;
    st->ctime_ns = node->ctime_ns;
}

uint32_t vibeos_file_in_use(void) {
    uint32_t i, n = 0;

    lock();
    for (i = 0; i < VIBEOS_FILE_MAX; i++) {
        if (g_files[i].refs != 0u) {
            n++;
        }
    }
    unlock();
    return n;
}
