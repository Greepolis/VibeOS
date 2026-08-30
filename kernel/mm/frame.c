/* L0: physical frames.
 *
 * One owner for physical memory. Everything that allocates, shares or releases
 * a frame goes through here, and nothing else touches the free list or a
 * reference count. That is the whole point: the defect this rewrite exists to
 * fix survived four fixes because the counting was a discipline spread across
 * nine call sites rather than arithmetic in one place.
 *
 * Two decisions worth explaining, because both are departures from what the
 * kernel does today.
 *
 * **The free list is threaded through the descriptors, not through the pages.**
 * The old one wrote a next-pointer into the first word of every freed page,
 * which meant the poison could not cover the whole page, the "who freed this"
 * tag had to live at offset 8, and the verification had to skip both. Here the
 * link is a frame index in the descriptor, so a freed page is poison from end
 * to end and any byte of it is evidence.
 *
 * **A count of one means one owner.** The previous scheme counted owners beyond
 * the first, so zero meant one, every path carried a mental offset, and an
 * untracked frame answered "yes, free it" - which turned a missed increment
 * anywhere into silent corruption. Here zero means nobody, and a frame this
 * table cannot describe is never freed.
 */

#include "vibeos/frame.h"
#include "vibeos/mm_stats.h"

/* Read as a pointer this is non-canonical, so dereferencing a stale one faults
 * on the spot instead of quietly reaching a live page. */
#define FRAME_POISON 0xDEAD0000DEAD0000ull

#define FRAME_NONE 0xFFFFFFFFu   /* end of the free list */

static vibeos_frame_t *g_table;
static uint32_t g_entries;
static uint64_t g_base;
static uint32_t g_free_head = FRAME_NONE;
static uint64_t g_free_count;
static vibeos_frame_map_fn g_map;
static int g_allocated_yet;      /* reserve() must come first */
static void (*g_watch)(uint64_t phys);

void vibeos_frame_set_release_watch(void (*watch)(uint64_t phys)) {
    g_watch = watch;
}

static void (*g_lock)(void);
static void (*g_unlock)(void);

void vibeos_frame_set_lock(void (*lock)(void), void (*unlock)(void)) {
    g_lock = lock;
    g_unlock = unlock;
}

/* Deliberately not recursive, and nothing inside this file calls another
 * public entry point of it, so it does not need to be. If that ever stops
 * being true the deadlock is immediate and obvious, which is the failure mode
 * to prefer. */
static void frame_lock(void) {
    if (g_lock) {
        g_lock();
    }
}

static void frame_unlock(void) {
    if (g_unlock) {
        g_unlock();
    }
}

static uint32_t frame_index(uint64_t phys) {
    uint64_t off;

    if (!g_table || phys < g_base) {
        return FRAME_NONE;
    }
    off = (phys - g_base) >> 12;
    return (off < (uint64_t)g_entries) ? (uint32_t)off : FRAME_NONE;
}

static uint64_t frame_addr(uint32_t index) {
    return g_base + ((uint64_t)index << 12);
}

static void frame_fill(uint32_t index, uint64_t pattern) {
    uint64_t *w;
    uint32_t i;

    if (!g_map) {
        return;
    }
    w = (uint64_t *)g_map(frame_addr(index));
    if (!w) {
        return;   /* not addressable from here; counted, not written */
    }
    for (i = 0; i < 4096u / 8u; i++) {
        w[i] = pattern;
    }
}

/* Was anything written to this frame while it sat on the free list?
 *
 * Sampled rather than exhaustive: the loop that zeroes the page is already the
 * cost of an allocation, and a writer that corrupts a free frame almost never
 * touches only one word of it. Unlike the old check, this can look at every
 * word including the first, because the free list no longer lives in the page.
 */
#define POISON_PROBES 16u

static void frame_check_poison(uint32_t index) {
    const uint64_t *w;
    uint32_t i;
    uint32_t step = (4096u / 8u) / POISON_PROBES;

    if (!g_map) {
        return;
    }
    if ((g_table[index].flags & VIBEOS_FRAME_WAS_FREED) == 0u) {
        return;   /* never released, so there is no poison to have survived */
    }
    w = (const uint64_t *)g_map(frame_addr(index));
    if (!w) {
        return;
    }
    for (i = 0; i < POISON_PROBES; i++) {
        if (w[i * step] != FRAME_POISON) {
            vibeos_mm_stats()->poison_hits++;
            return;
        }
    }
}

static void frame_push_free(uint32_t index) {
    g_table[index].state = (uint8_t)VIBEOS_FRAME_FREE;
    g_table[index].owners = 0;
    /* Everything except the was-freed mark, which is the one fact about a frame
     * that must outlive its contents. */
    g_table[index].flags &= (uint8_t)VIBEOS_FRAME_WAS_FREED;
    g_table[index].backing = 0;
    g_table[index].lru_next = g_free_head;
    g_free_head = index;
    g_free_count++;
}

int vibeos_frame_init(uint64_t base, uint64_t len,
                      vibeos_frame_t *table, uint32_t entries,
                      vibeos_frame_map_fn map) {
    uint64_t frames = len >> 12;
    uint32_t i;

    if (!table || entries == 0u || frames == 0ull) {
        return -1;
    }
    if (frames > (uint64_t)entries) {
        frames = entries;   /* describe what we can, and say so in the totals */
    }

    g_table = table;
    g_entries = (uint32_t)frames;
    g_base = base & ~0xFFFull;
    g_map = map;
    g_free_head = FRAME_NONE;
    g_free_count = 0;
    g_allocated_yet = 0;

    /* Built back to front so the list comes out in ascending order, which makes
     * a boot's allocations land contiguously and a dump readable.
     *
     * Deliberately no poisoning here. When the kernel brings this layer up, the
     * bump allocator underneath has already handed out early page tables and
     * this very descriptor table; filling the region would destroy live memory.
     * Poison belongs to release, and the check that reads it only applies to
     * frames that have actually been released - see VIBEOS_FRAME_WAS_FREED. */
    for (i = g_entries; i > 0u; i--) {
        uint32_t index = i - 1u;
        g_table[index].lru_prev = FRAME_NONE;
        frame_push_free(index);
    }

    vibeos_mm_stats()->frames_total = g_entries;
    vibeos_mm_stats()->frames_free = g_free_count;
    vibeos_mm_stats()->frames_allocated = 0;
    return 0;
}

int vibeos_frame_reserve(uint64_t base, uint64_t len) {
    uint64_t addr;
    uint32_t prev, cur;

    if (!g_table || g_allocated_yet) {
        return -1;   /* reserving after the first allocation is a bug, not a hint */
    }
    /* Check the whole range is describable and free before changing anything,
     * so a refusal leaves the layer exactly as it was (I5). */
    for (addr = base & ~0xFFFull; addr < base + len; addr += 4096ull) {
        uint32_t index = frame_index(addr);
        if (index == FRAME_NONE ||
            g_table[index].state != (uint8_t)VIBEOS_FRAME_FREE) {
            return -1;
        }
    }
    for (addr = base & ~0xFFFull; addr < base + len; addr += 4096ull) {
        uint32_t index = frame_index(addr);

        /* Unlink from wherever it sits in the free list. The list is short-lived
         * at this point - reservation happens once, at boot - so a walk is
         * cheaper than a doubly linked list everywhere else would then pay for. */
        prev = FRAME_NONE;
        for (cur = g_free_head; cur != FRAME_NONE; cur = g_table[cur].lru_next) {
            if (cur == index) {
                if (prev == FRAME_NONE) {
                    g_free_head = g_table[cur].lru_next;
                } else {
                    g_table[prev].lru_next = g_table[cur].lru_next;
                }
                g_free_count--;
                break;
            }
            prev = cur;
        }
        g_table[index].state = (uint8_t)VIBEOS_FRAME_RESERVED;
        g_table[index].lru_next = FRAME_NONE;
    }
    vibeos_mm_stats()->frames_free = g_free_count;
    return 0;
}

/* Hand one frame out: checked, zeroed, one owner. Shared by both allocation
 * paths so they cannot drift apart - the contiguous one was written second, and
 * a second copy of "what allocation means" is how this subsystem got here. */
static void frame_take(uint32_t index, vibeos_frame_state_t state) {
    g_allocated_yet = 1;
    frame_check_poison(index);
    frame_fill(index, 0ull);          /* handed out zeroed (I4) */
    g_table[index].owners = 1;
    g_table[index].state = (uint8_t)state;
    g_table[index].flags = 0;
    g_table[index].backing = 0;
    g_table[index].lru_next = FRAME_NONE;
}

uint64_t vibeos_frame_alloc_locked(vibeos_frame_state_t state) {
    uint32_t index;

    if (!g_table || g_free_head == FRAME_NONE) {
        return 0;   /* nothing changed (I5) */
    }
    index = g_free_head;
    g_free_head = g_table[index].lru_next;
    g_free_count--;

    frame_take(index, state);

    vibeos_mm_stats()->frames_free = g_free_count;
    vibeos_mm_stats()->frames_allocated++;
    return frame_addr(index);
}

uint64_t vibeos_frame_alloc_contig_locked(uint32_t count, vibeos_frame_state_t state) {
    uint32_t i, run = 0u, first = 0u;
    uint32_t prev, cur;

    if (!g_table || count == 0u || count > g_entries) {
        return 0;
    }

    /* First fit. The state field is the authority on what is free, not
     * membership of the list: a reserved frame is not free and must break the
     * run even though it is nowhere in the list. */
    for (i = 0; i < g_entries; i++) {
        if (g_table[i].state == (uint8_t)VIBEOS_FRAME_FREE) {
            if (run == 0u) {
                first = i;
            }
            if (++run == count) {
                break;
            }
        } else {
            run = 0u;
        }
    }
    if (run < count) {
        return 0;   /* nothing changed (I5) */
    }

    /* Unlink the whole run in one walk of the free list rather than one walk
     * per frame. The frames are contiguous by address, not by position in the
     * list, so there is no shortcut past this. */
    prev = FRAME_NONE;
    cur = g_free_head;
    while (cur != FRAME_NONE) {
        uint32_t next = g_table[cur].lru_next;
        if (cur >= first && cur < first + count) {
            if (prev == FRAME_NONE) {
                g_free_head = next;
            } else {
                g_table[prev].lru_next = next;
            }
            g_free_count--;
        } else {
            prev = cur;
        }
        cur = next;
    }

    for (i = 0; i < count; i++) {
        frame_take(first + i, state);
    }
    vibeos_mm_stats()->frames_free = g_free_count;
    vibeos_mm_stats()->frames_allocated += count;
    return frame_addr(first);
}

void vibeos_frame_get_locked(uint64_t phys) {
    uint32_t index = frame_index(phys);

    if (index == FRAME_NONE) {
        return;
    }
    if (g_table[index].owners < 0xFFFFu) {
        g_table[index].owners++;
    }
    /* Saturated: never reclaimed. A frame that leaks can be seen in a counter;
     * one freed early is found three programs later. */
}

int vibeos_frame_put_locked(uint64_t phys) {
    uint32_t index = frame_index(phys);

    if (index == FRAME_NONE) {
        vibeos_mm_stats()->frames_leaked++;
        return 0;                     /* not describable, not ours to free (I2) */
    }
    if (g_table[index].owners == 0u) {
        vibeos_mm_stats()->frames_double_put++;
        return 0;
    }
    if (g_table[index].owners == 0xFFFFu) {
        return 0;                     /* saturated above */
    }
    if (--g_table[index].owners != 0u) {
        return 0;
    }
    frame_fill(index, FRAME_POISON);  /* released poisoned (I4) */
    g_table[index].flags |= (uint8_t)VIBEOS_FRAME_WAS_FREED;
    frame_push_free(index);
    vibeos_mm_stats()->frames_free = g_free_count;
    if (vibeos_mm_stats()->frames_allocated > 0ull) {
        vibeos_mm_stats()->frames_allocated--;
    }
    return 1;                         /* freed, and only at zero (I1) */
}

uint32_t vibeos_frame_owners_locked(uint64_t phys) {
    uint32_t index = frame_index(phys);
    return (index == FRAME_NONE) ? 0u : g_table[index].owners;
}

vibeos_frame_state_t vibeos_frame_state(uint64_t phys) {
    uint32_t index = frame_index(phys);
    return (index == FRAME_NONE) ? VIBEOS_FRAME_FREE
                                 : (vibeos_frame_state_t)g_table[index].state;
}

void vibeos_frame_survey_locked(uint64_t *by_state, uint64_t *largest_free_run) {
    uint32_t i;
    uint64_t run = 0, best = 0;

    if (by_state) {
        for (i = 0; i < (uint32_t)VIBEOS_FRAME_STATE_COUNT; i++) {
            by_state[i] = 0;
        }
    }
    if (largest_free_run) {
        *largest_free_run = 0;
    }
    if (!g_table) {
        return;
    }
    for (i = 0; i < g_entries; i++) {
        uint8_t st = g_table[i].state;
        if (by_state && st < (uint8_t)VIBEOS_FRAME_STATE_COUNT) {
            by_state[st]++;
        }
        if (st == (uint8_t)VIBEOS_FRAME_FREE) {
            if (++run > best) {
                best = run;
            }
        } else {
            run = 0;
        }
    }
    if (largest_free_run) {
        *largest_free_run = best;
    }
}

uint64_t vibeos_frame_total(void) {
    return g_entries;
}

uint64_t vibeos_frame_free_count_locked(void) {
    return g_free_count;
}

uint64_t vibeos_frame_alloc(vibeos_frame_state_t state) {
    uint64_t r;
    frame_lock();
    r = vibeos_frame_alloc_locked(state);
    frame_unlock();
    return r;
}

uint64_t vibeos_frame_alloc_contig(uint32_t count, vibeos_frame_state_t state) {
    uint64_t r;
    frame_lock();
    r = vibeos_frame_alloc_contig_locked(count, state);
    frame_unlock();
    return r;
}

void vibeos_frame_get(uint64_t phys) {
    frame_lock();
    vibeos_frame_get_locked(phys);
    frame_unlock();
}

int vibeos_frame_put(uint64_t phys) {
    int r;

    /* Before the lock, and sampled by the caller's own counter: the watch walks
     * page tables to ask whether a live process still maps this frame, which is
     * far too slow to do under a lock that masks interrupts - and it takes that
     * lock itself, by asking this layer for owner counts. */
    if (g_watch && vibeos_frame_owners(phys) == 1u) {
        g_watch(phys);
    }
    frame_lock();
    r = vibeos_frame_put_locked(phys);
    frame_unlock();
    return r;
}

uint32_t vibeos_frame_owners(uint64_t phys) {
    uint32_t r;
    frame_lock();
    r = vibeos_frame_owners_locked(phys);
    frame_unlock();
    return r;
}

uint64_t vibeos_frame_free_count(void) {
    uint64_t r;
    frame_lock();
    r = vibeos_frame_free_count_locked();
    frame_unlock();
    return r;
}

void vibeos_frame_survey(uint64_t *by_state, uint64_t *largest_free_run) {
    frame_lock();
    vibeos_frame_survey_locked(by_state, largest_free_run);
    frame_unlock();
}

/* A note on vibeos_frame_survey: it walks every descriptor, which with a
 * hundred thousand frames is a few hundred microseconds under the lock, and on
 * this machine the lock masks interrupts. That is acceptable because the only
 * caller is a command somebody typed. It would not be acceptable on a fault
 * path, and if one ever needs these numbers it should get them from the
 * counters rather than from a walk. */
