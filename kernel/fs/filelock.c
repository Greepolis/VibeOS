/* Advisory file locks. See include/vibeos/filelock.h. */

#include "vibeos/filelock.h"
#include "vibeos/abi_linux.h"
#include "vibeos/mbz.h"

typedef struct {
    uint8_t used;
    uint8_t space;
    uint8_t type;
    uint32_t pid;
    const void *fs;
    uint64_t node;
    uint64_t owner;
    uint64_t start, end;
} flk_t;

static flk_t g_flk[VIBEOS_FLK_MAX];
static uint32_t g_flk_used;
static struct {
    uint64_t waiter, blocker;
} g_wait[VIBEOS_FLK_WAITERS];
static void (*g_lock)(void);
static void (*g_unlock)(void);

/* A call with no lock registered is counted (must be zero) rather than
 * trusted: the table is reached from every core's close, and "configured by
 * nobody" is the defect this project produces most often. Tests register one. */
static void lock(void) {
    if (g_lock) {
        g_lock();
    } else {
        vibeos_mbz_hit(VIBEOS_MBZ_FILELOCK_UNLOCKED, 0);
    }
}

static void unlock(void) {
    if (g_unlock) {
        g_unlock();
    }
}

void vibeos_flk_set_lock(void (*l)(void), void (*u)(void)) {
    g_lock = l;
    g_unlock = u;
}

void vibeos_flk_reset(void) {
    uint32_t i;

    for (i = 0; i < VIBEOS_FLK_MAX; i++) {
        g_flk[i].used = 0;
    }
    for (i = 0; i < VIBEOS_FLK_WAITERS; i++) {
        g_wait[i].waiter = 0;
    }
    g_flk_used = 0;
}

static int on_file(const flk_t *e, uint32_t space, const void *fs, uint64_t node) {
    return e->used && e->space == space && e->fs == fs && e->node == node;
}

/* The first lock of another owner that `type` on [start, end] cannot coexist
 * with, or null. */
static const flk_t *conflict(uint32_t space, const void *fs, uint64_t node, uint64_t owner,
                             uint32_t type, uint64_t start, uint64_t end) {
    const flk_t *first = 0;
    uint32_t i;

    /* The one that starts lowest, not the one the table happens to hold first.
     * Which lock is "in the way" is something a program reads: F_GETLK reports
     * it, and Linux keeps a file's locks in order of where they start, so the
     * answer there is the first by position. Returning the table's first made
     * the answer depend on the order the locks had been taken in - LTP's
     * fcntl11 takes a write lock at byte 10 and then a read lock at byte 1,
     * asks, and was told about byte 10. */
    for (i = 0; i < VIBEOS_FLK_MAX; i++) {
        const flk_t *e = &g_flk[i];
        if (!on_file(e, space, fs, node) || e->owner == owner) {
            continue;
        }
        if (e->end < start || e->start > end) {
            continue;
        }
        if ((type == VIBEOS_FLK_EXCL || e->type == VIBEOS_FLK_EXCL) &&
            (!first || e->start < first->start)) {
            first = e;
        }
    }
    return first;
}

static flk_t *take_free(void) {
    uint32_t i;

    for (i = 0; i < VIBEOS_FLK_MAX; i++) {
        if (!g_flk[i].used) {
            g_flk[i].used = 1;
            g_flk_used++;
            return &g_flk[i];
        }
    }
    return 0;
}

static void give_back(flk_t *e) {
    e->used = 0;
    g_flk_used--;
}

static void wait_done_locked(uint64_t waiter) {
    uint32_t i;

    for (i = 0; i < VIBEOS_FLK_WAITERS; i++) {
        if (g_wait[i].waiter == waiter) {
            g_wait[i].waiter = 0;
        }
    }
}

int vibeos_flk_set(uint32_t space, const void *fs, uint64_t node, uint64_t owner,
                   uint32_t pid, uint32_t type, uint64_t start, uint64_t end,
                   uint64_t *blocker) {
    const flk_t *c;
    flk_t *n;
    uint32_t i;
    int merged;

    if (end < start || owner == 0u || type > VIBEOS_FLK_EXCL) {
        return -VIBEOS_EINVAL;
    }
    lock();
    if (type != VIBEOS_FLK_UNLOCK &&
        (c = conflict(space, fs, node, owner, type, start, end)) != 0) {
        if (blocker) {
            *blocker = c->owner;
        }
        unlock();
        return -VIBEOS_EAGAIN;
    }
    /* Room for the worst case before anything changes: a range cut out of the
     * middle of one lock leaves two pieces where there was one, and the new
     * lock is one more. A refusal half-way would leave the owner holding
     * something it never asked for. */
    if (VIBEOS_FLK_MAX - g_flk_used < 2u) {
        unlock();
        return -VIBEOS_ENOLCK;
    }
    /* Cut [start, end] out of whatever the owner holds on this file. Its locks
     * are disjoint, so at most one of them reaches past both ends. */
    for (i = 0; i < VIBEOS_FLK_MAX; i++) {
        flk_t *e = &g_flk[i];
        if (!on_file(e, space, fs, node) || e->owner != owner || e->end < start || e->start > end) {
            continue;
        }
        if (e->start < start && e->end > end) {
            n = take_free();
            *n = *e;
            n->start = end + 1u;
            e->end = start - 1u;
        } else if (e->start < start) {
            e->end = start - 1u;
        } else if (e->end > end) {
            e->start = end + 1u;
        } else {
            give_back(e);
        }
    }
    if (type != VIBEOS_FLK_UNLOCK) {
        n = take_free();
        n->space = (uint8_t)space;
        n->type = (uint8_t)type;
        n->pid = pid;
        n->fs = fs;
        n->node = node;
        n->owner = owner;
        n->start = start;
        n->end = end;
        /* Join it to the owner's neighbours of the same type: a file locked a
         * record at a time must not fill the table with touching pieces. */
        do {
            merged = 0;
            for (i = 0; i < VIBEOS_FLK_MAX; i++) {
                flk_t *e = &g_flk[i];
                if (e == n || !on_file(e, space, fs, node) || e->owner != owner ||
                    e->type != n->type) {
                    continue;
                }
                if (e->end != VIBEOS_FLK_END && e->end + 1u == n->start) {
                    n->start = e->start;
                } else if (n->end != VIBEOS_FLK_END && n->end + 1u == e->start) {
                    n->end = e->end;
                } else {
                    continue;
                }
                give_back(e);
                merged = 1;
            }
        } while (merged);
    }
    unlock();
    return 0;
}

int vibeos_flk_test(uint32_t space, const void *fs, uint64_t node, uint64_t owner,
                    uint32_t type, uint64_t start, uint64_t end, vibeos_flk_info_t *out) {
    const flk_t *c;
    int hit = 0;

    lock();
    c = conflict(space, fs, node, owner, type, start, end);
    if (c) {
        hit = 1;
        if (out) {
            out->type = c->type;
            out->start = c->start;
            out->end = c->end;
            out->owner = c->owner;
            out->pid = c->pid;
        }
    }
    unlock();
    return hit;
}

void vibeos_flk_drop_file(uint64_t owner, const void *fs, uint64_t node) {
    uint32_t i;

    lock();
    for (i = 0; i < VIBEOS_FLK_MAX; i++) {
        flk_t *e = &g_flk[i];
        if (e->used && e->owner == owner && e->fs == fs && e->node == node) {
            give_back(e);
        }
    }
    wait_done_locked(owner);
    unlock();
}

void vibeos_flk_drop_owner(uint64_t owner) {
    uint32_t i;

    lock();
    for (i = 0; i < VIBEOS_FLK_MAX; i++) {
        if (g_flk[i].used && g_flk[i].owner == owner) {
            give_back(&g_flk[i]);
        }
    }
    wait_done_locked(owner);
    unlock();
}

int vibeos_flk_wait(uint64_t waiter, uint64_t blocker) {
    uint64_t cur = blocker;
    uint32_t hops, i;
    int slot = -1;

    lock();
    /* Follow who the blocker is waiting for, and who that one is waiting for:
     * arriving back at the waiter is a wait that cannot end. The hop count
     * bounds a walk over a table something else got wrong. */
    for (hops = 0; hops <= VIBEOS_FLK_WAITERS; hops++) {
        if (cur == waiter) {
            unlock();
            return -VIBEOS_EDEADLK;
        }
        for (i = 0; i < VIBEOS_FLK_WAITERS; i++) {
            if (g_wait[i].waiter == cur) {
                break;
            }
        }
        if (i == VIBEOS_FLK_WAITERS) {
            break;
        }
        cur = g_wait[i].blocker;
    }
    for (i = 0; i < VIBEOS_FLK_WAITERS; i++) {
        if (g_wait[i].waiter == waiter) {
            slot = (int)i;
            break;
        }
        if (g_wait[i].waiter == 0u && slot < 0) {
            slot = (int)i;
        }
    }
    /* With no room the wait goes unrecorded: it still happens, and a deadlock
     * through it is one this table cannot see. */
    if (slot >= 0) {
        g_wait[slot].waiter = waiter;
        g_wait[slot].blocker = blocker;
    }
    unlock();
    return 0;
}

void vibeos_flk_wait_done(uint64_t waiter) {
    lock();
    wait_done_locked(waiter);
    unlock();
}

uint32_t vibeos_flk_count(void) {
    return g_flk_used;
}
