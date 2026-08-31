/* L2: regions. See include/vibeos/vma.h for why this exists.
 *
 * A sorted list with no overlaps, and every operation maintains both of those.
 * The invariant is worth stating because most of the code below is there to
 * preserve it rather than to do anything: sorted and disjoint is what makes
 * `find` a walk instead of a search, and what makes "is this range mapped?" a
 * question with one answer.
 */

#include "vibeos/vma.h"
#include "vibeos/mm_stats.h"

static vibeos_vma_t *g_pool;
static uint32_t g_pool_entries;
static vibeos_vma_t *g_free;        /* threaded through `next` */
static uint32_t g_live;
static void (*g_lock)(void);
static void (*g_unlock)(void);

void vibeos_vma_set_lock(void (*lock)(void), void (*unlock)(void)) {
    g_lock = lock;
    g_unlock = unlock;
}

/* Not recursive, and nothing inside this file calls another public entry point
 * of it, so it does not need to be. */
static void vibeos_vma_clear_locked(vibeos_vma_list_t *list);

static void vma_lock(void) {
    if (g_lock) {
        g_lock();
    }
}

static void vma_unlock(void) {
    if (g_unlock) {
        g_unlock();
    }
}

void vibeos_vma_pool_init(vibeos_vma_t *pool, uint32_t entries) {
    uint32_t i;

    g_pool = pool;
    g_pool_entries = entries;
    g_free = 0;
    g_live = 0;
    if (!pool) {
        return;
    }
    /* Back to front, so the free list comes out in ascending order and a dump
     * of it reads the way the array does. */
    for (i = entries; i > 0u; i--) {
        pool[i - 1u].next = g_free;
        g_free = &pool[i - 1u];
    }
}

uint32_t vibeos_vma_live(void) {
    return g_live;
}

static vibeos_vma_t *vma_alloc(void) {
    vibeos_vma_t *v = g_free;

    if (!v) {
        return 0;
    }
    g_free = v->next;
    v->next = 0;
    g_live++;
    vibeos_mm_stats()->vmas_live = g_live;
    /* The peak, not just the current count. A pool that runs dry mid-boot and
     * is empty again by the time anybody looks reports zero live regions and
     * eight refusals, which says something went wrong but not how close the
     * ceiling is. */
    if ((uint64_t)g_live > vibeos_mm_stats()->vmas_peak) {
        vibeos_mm_stats()->vmas_peak = g_live;
    }
    return v;
}

static void vma_free(vibeos_vma_t *v) {
    v->next = g_free;
    g_free = v;
    if (g_live > 0u) {
        g_live--;
    }
    vibeos_mm_stats()->vmas_live = g_live;
}

/* Two regions describe the same kind of memory, so one descriptor could stand
 * for both if they were adjacent. Everything is compared, including the backing
 * offset: two halves of one file at unrelated offsets are not one region even
 * when they sit next to each other in the address space. */
static int vma_same_kind(const vibeos_vma_t *a, uint64_t a_end,
                         vibeos_prot_t prot, vibeos_backing_kind_t backing,
                         uint32_t backing_id, uint64_t backing_offset) {
    if (a->prot != prot || a->backing != backing || a->backing_id != backing_id) {
        return 0;
    }
    if (backing == VIBEOS_BACKING_ANON) {
        return 1;   /* anonymous memory has no offset to be continuous in */
    }
    return a->backing_offset + (a_end - a->base) == backing_offset;
}

static void vma_link_after(vibeos_vma_list_t *list, vibeos_vma_t *prev,
                           vibeos_vma_t *v) {
    if (prev) {
        v->next = prev->next;
        prev->next = v;
    } else {
        v->next = list->head;
        list->head = v;
    }
    list->count++;
}

static void vma_unlink_after(vibeos_vma_list_t *list, vibeos_vma_t *prev,
                             vibeos_vma_t *v) {
    if (prev) {
        prev->next = v->next;
    } else {
        list->head = v->next;
    }
    if (list->count > 0u) {
        list->count--;
    }
    vma_free(v);
}

/* Fold `v` into its successor when they have become adjacent and alike. Called
 * after every change that can create that situation, so a program growing its
 * heap a page at a time ends with one region rather than a thousand. */
static void vma_try_merge(vibeos_vma_list_t *list, vibeos_vma_t *prev,
                          vibeos_vma_t *v) {
    vibeos_vma_t *n;

    if (!v) {
        return;
    }
    n = v->next;
    if (n && v->base + v->len == n->base &&
        vma_same_kind(v, v->base + v->len, n->prot, n->backing,
                      n->backing_id, n->backing_offset)) {
        v->len += n->len;
        vma_unlink_after(list, v, n);
    }
    if (prev && prev->base + prev->len == v->base &&
        vma_same_kind(prev, prev->base + prev->len, v->prot, v->backing,
                      v->backing_id, v->backing_offset)) {
        prev->len += v->len;
        vma_unlink_after(list, prev, v);
    }
}

int vibeos_vma_insert_locked(vibeos_vma_list_t *list, uint64_t base, uint64_t len,
                      vibeos_prot_t prot, vibeos_backing_kind_t backing,
                      uint32_t backing_id, uint64_t backing_offset) {
    vibeos_vma_t *prev = 0, *cur, *v;
    uint64_t end;

    if (!list || len == 0u || (base & 0xFFFull) || (len & 0xFFFull)) {
        return -1;
    }
    end = base + len;
    if (end < base) {
        return -1;   /* wrapped */
    }

    /* Find the insertion point and refuse an overlap on the way. Checked before
     * anything is allocated, so a refusal leaves the pool untouched. */
    for (cur = list->head; cur; cur = cur->next) {
        if (cur->base >= end) {
            break;
        }
        if (cur->base + cur->len > base) {
            return -1;   /* overlaps an existing region */
        }
        prev = cur;
    }

    v = vma_alloc();
    if (!v) {
        /* Counted, not silent. Every caller ignores the return - a region that
         * cannot be described is not a reason to fail an mmap that worked -
         * so without this the pool running dry looks like mprotect refusing
         * addresses for no reason, several services later. */
        vibeos_mm_stats()->vmas_refused++;
        return -1;
    }
    v->base = base;
    v->len = len;
    v->prot = prot;
    v->backing = backing;
    v->backing_id = backing_id;
    v->backing_offset = backing_offset;
    vma_link_after(list, prev, v);
    vma_try_merge(list, prev, v);
    vibeos_mm_stats()->vmas_created++;
    return 0;
}

vibeos_vma_t *vibeos_vma_find_locked(vibeos_vma_list_t *list, uint64_t va) {
    vibeos_vma_t *cur;

    if (!list) {
        return 0;
    }
    for (cur = list->head; cur; cur = cur->next) {
        if (va < cur->base) {
            return 0;   /* sorted, so nothing further can contain it */
        }
        if (va < cur->base + cur->len) {
            return cur;
        }
    }
    return 0;
}

/* Cut `v` at `at`, leaving `v` as the part below and a new region above.
 * Returns the new region, or null if the pool is empty - in which case nothing
 * has changed. */
static vibeos_vma_t *vma_split(vibeos_vma_list_t *list, vibeos_vma_t *v,
                               uint64_t at) {
    vibeos_vma_t *hi = vma_alloc();

    if (!hi) {
        return 0;
    }
    hi->base = at;
    hi->len = v->base + v->len - at;
    hi->prot = v->prot;
    hi->backing = v->backing;
    hi->backing_id = v->backing_id;
    /* The offset advances with the address: the upper half of a file mapping
     * starts further into the file. Getting this wrong is silent until
     * something actually reads through the region, which is why it is written
     * here once rather than at each caller. */
    hi->backing_offset = v->backing_offset + (at - v->base);
    hi->next = v->next;
    v->next = hi;
    v->len = at - v->base;
    list->count++;
    vibeos_mm_stats()->vmas_split++;
    return hi;
}

uint64_t vibeos_vma_remove_locked(vibeos_vma_list_t *list, uint64_t base, uint64_t len) {
    vibeos_vma_t *prev = 0, *cur;
    uint64_t end, removed = 0;

    if (!list || len == 0u) {
        return 0;
    }
    end = base + len;
    if (end < base) {
        return 0;
    }

    cur = list->head;
    while (cur) {
        uint64_t cur_end = cur->base + cur->len;

        if (cur->base >= end) {
            break;              /* sorted: nothing further overlaps */
        }
        if (cur_end <= base) {
            prev = cur;
            cur = cur->next;
            continue;
        }

        /* Four shapes, and the middle one is the case a page-table walk had no
         * way to represent: removing the middle of a region leaves two. */
        if (cur->base < base && cur_end > end) {
            vibeos_vma_t *hi = vma_split(list, cur, end);
            if (!hi) {
                return removed;   /* out of descriptors; stop, do not corrupt */
            }
            cur->len = base - cur->base;
            removed += len;
            break;
        }
        if (cur->base < base) {                 /* trim the tail */
            removed += cur_end - base;
            cur->len = base - cur->base;
            prev = cur;
            cur = cur->next;
            continue;
        }
        if (cur_end > end) {                    /* trim the head */
            removed += end - cur->base;
            cur->backing_offset += end - cur->base;
            cur->len = cur_end - end;
            cur->base = end;
            break;
        }
        {                                       /* swallowed whole */
            vibeos_vma_t *next = cur->next;
            removed += cur->len;
            vma_unlink_after(list, prev, cur);
            cur = next;
        }
    }
    vibeos_mm_stats()->vmas_removed++;
    return removed;
}

int vibeos_vma_protect_locked(vibeos_vma_list_t *list, uint64_t base, uint64_t len,
                       vibeos_prot_t prot) {
    vibeos_vma_t *prev = 0, *cur;
    uint64_t end, covered = 0;

    if (!list || len == 0u) {
        return -1;
    }
    end = base + len;
    if (end < base) {
        return -1;
    }

    /* Every page in the range must be mapped, and that is established before
     * anything changes: a partial application leaves the process in a state it
     * never asked for, and the caller cannot tell how far it got. */
    for (cur = list->head; cur; cur = cur->next) {
        uint64_t lo = cur->base > base ? cur->base : base;
        uint64_t hi = (cur->base + cur->len) < end ? (cur->base + cur->len) : end;
        if (hi > lo) {
            covered += hi - lo;
        }
    }
    if (covered != len) {
        return -1;
    }

    cur = list->head;
    while (cur) {
        uint64_t cur_end = cur->base + cur->len;

        if (cur->base >= end) {
            break;
        }
        if (cur_end <= base) {
            prev = cur;
            cur = cur->next;
            continue;
        }
        if (cur->prot == prot) {
            prev = cur;
            cur = cur->next;
            continue;
        }
        if (cur->base < base) {
            if (!vma_split(list, cur, base)) {
                return -1;
            }
            prev = cur;
            cur = cur->next;
            continue;           /* the upper half is handled next time round */
        }
        if (cur_end > end) {
            if (!vma_split(list, cur, end)) {
                return -1;
            }
            cur_end = end;
        }
        cur->prot = prot;
        {
            vibeos_vma_t *next = cur->next;
            vma_try_merge(list, prev, cur);
            cur = next;
        }
    }
    return 0;
}

int vibeos_vma_clone_locked(vibeos_vma_list_t *dst, const vibeos_vma_list_t *src) {
    const vibeos_vma_t *cur;
    vibeos_vma_t *tail = 0;

    if (!dst || !src || dst->head) {
        return -1;
    }
    dst->count = 0;
    for (cur = src->head; cur; cur = cur->next) {
        vibeos_vma_t *v = vma_alloc();
        if (!v) {
            /* Empty it again rather than handing back half a picture: a
             * process whose regions describe some of its memory is worse than
             * one whose fork failed. */
            vibeos_vma_clear_locked(dst);
            return -1;
        }
        *v = *cur;
        v->next = 0;
        if (tail) {
            tail->next = v;
        } else {
            dst->head = v;
        }
        tail = v;
        dst->count++;
    }
    return 0;
}

void vibeos_vma_clear_locked(vibeos_vma_list_t *list) {
    vibeos_vma_t *cur;

    if (!list) {
        return;
    }
    cur = list->head;
    while (cur) {
        vibeos_vma_t *next = cur->next;
        vma_free(cur);
        cur = next;
    }
    list->head = 0;
    list->count = 0;
}

int vibeos_vma_insert(vibeos_vma_list_t *list, uint64_t base, uint64_t len,
                      vibeos_prot_t prot, vibeos_backing_kind_t backing,
                      uint32_t backing_id, uint64_t backing_offset) {
    int r;
    vma_lock();
    r = vibeos_vma_insert_locked(list, base, len, prot, backing, backing_id, backing_offset);
    vma_unlock();
    return r;
}

uint64_t vibeos_vma_remove(vibeos_vma_list_t *list, uint64_t base, uint64_t len) {
    uint64_t r;
    vma_lock();
    r = vibeos_vma_remove_locked(list, base, len);
    vma_unlock();
    return r;
}

int vibeos_vma_protect(vibeos_vma_list_t *list, uint64_t base, uint64_t len,
                       vibeos_prot_t prot) {
    int r;
    vma_lock();
    r = vibeos_vma_protect_locked(list, base, len, prot);
    vma_unlock();
    return r;
}

int vibeos_vma_clone(vibeos_vma_list_t *dst, const vibeos_vma_list_t *src) {
    int r;
    vma_lock();
    r = vibeos_vma_clone_locked(dst, src);
    vma_unlock();
    return r;
}

void vibeos_vma_clear(vibeos_vma_list_t *list) {
    vma_lock();
    vibeos_vma_clear_locked(list);
    vma_unlock();
}

vibeos_vma_t *vibeos_vma_find(vibeos_vma_list_t *list, uint64_t va) {
    vibeos_vma_t * r;
    vma_lock();
    r = vibeos_vma_find_locked(list, va);
    vma_unlock();
    return r;
}
