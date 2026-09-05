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

static void (*g_poison_watch)(uint64_t phys, uint32_t word, uint64_t found,
                              uint64_t tag);

void vibeos_frame_set_poison_watch(void (*watch)(uint64_t phys, uint32_t word,
                                                 uint64_t found, uint64_t tag)) {
    g_poison_watch = watch;
}

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
            if (g_poison_watch) {
                /* Word 1 is where the release stored its tag, and the probes
                 * never touch it - so unless the writer covered the whole
                 * page, the frame still names who let go of it. */
                g_poison_watch(frame_addr(index), i * step, w[i * step], w[1]);
            }
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

/* How many descriptors arrived from the caller already claiming to have been
 * released. Zero on a clean table; non-zero says the memory underneath was
 * reused, which is legal - but the poison check must not read it as evidence. */
static uint32_t g_dirty_at_init;

uint32_t vibeos_frame_dirty_at_init(void) {
    return g_dirty_at_init;
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
    g_dirty_at_init = 0;

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
        /* The caller hands us memory, not a clean table, and this layer owns
         * what a descriptor means - so start every field from a known value
         * here rather than trusting the bump allocator's leftovers.
         *
         * flags is why this exists. frame_push_free deliberately preserves the
         * was-freed mark (`flags &= WAS_FREED`), which on an uninitialised
         * table preserves *garbage*: every frame whose stale byte happened to
         * have bit 0x10 set came up claiming it had been released, and the
         * poison check then read its virgin contents as corruption. That is
         * 3019 reported use-after-frees in a boot with none, and it moved with
         * the staging-buffer sizes only because the sizes decide which physical
         * memory this table lands on - which is what made it look for months
         * like an allocator-layout-sensitive memory bug.
         *
         * The count of frames that arrived already marked is reported rather
         * than silently cleared: it is the difference between "the table was
         * clean" and "the table was dirty and we coped". */
        if ((g_table[index].flags & VIBEOS_FRAME_WAS_FREED) != 0u) {
            g_dirty_at_init++;
        }
        g_table[index].flags = 0;
        g_table[index].owners = 0;
        g_table[index].backing = 0;
        g_table[index].lru_next = FRAME_NONE;
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
/* A frame handed out while somebody still owns it.
 *
 * Every other detector in this layer watches the release side, because that is
 * where a frame is normally lost. This one watches the other end, and the
 * difference is why the defect it looks for survived so long: a double
 * *allocation* frees nothing, so the free-side watch stays silent for a whole
 * boot, and the new owner's first act is to be handed a zeroed page with
 * owners reset to 1 - which erases the evidence that two parties held it.
 * What is left from outside is a live private page whose contents changed with
 * nobody visibly to blame, which is exactly how this was reported for months.
 *
 * Counted rather than refused: this layer is host-tested and has no console,
 * and the boot gate asserts the count is zero. */
static void frame_take(uint32_t index, vibeos_frame_state_t state) {
    if (g_table[index].state != (uint8_t)VIBEOS_FRAME_FREE ||
        g_table[index].owners != 0u) {
        vibeos_mm_stats()->double_allocs++;
    }
    g_allocated_yet = 1;
    frame_check_poison(index);
    frame_fill(index, 0ull);          /* handed out zeroed (I4) */
    g_table[index].owners = 1;
    g_table[index].state = (uint8_t)state;
    g_table[index].flags = 0;
    g_table[index].backing = 0;
    g_table[index].lru_next = FRAME_NONE;
}

/* Fault injection. See the header for why it is compiled in. */
static uint32_t g_fail_after;
static uint32_t g_injected;

void vibeos_frame_fail_after(uint32_t n) {
    frame_lock();
    g_fail_after = n;
    g_injected = 0;
    frame_unlock();
}

uint32_t vibeos_frame_injected_failures(void) {
    uint32_t n;

    frame_lock();
    n = g_injected;
    frame_unlock();
    return n;
}

uint64_t vibeos_frame_alloc_locked(vibeos_frame_state_t state) {
    uint32_t index;

    if (g_fail_after != 0u) {
        /* Counted down under the same lock that guards the free list, so an
         * injected failure lands at a definite point in a sequence rather than
         * approximately. */
        if (--g_fail_after == 0u) {
            g_injected++;
            return 0;   /* nothing changed (I5) - the point of the exercise */
        }
    }
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

/* Set while a release is in progress, so the poison fill can leave the tag
 * behind without a second pass over the page. */
static const void *g_put_tag;

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
    if (g_put_tag && g_map) {
        /* Who released it, in the page itself, written here rather than by the
         * caller. A caller writing it after the release is a use-after-free:
         * the frame can be allocated by another core in between, and word 1 is
         * slot 1 of a PML4. Under this lock, allocation cannot intervene.
         *
         * Word 1 is chosen because the poison check never probes it, so the
         * tag does not read as corruption when the frame is handed out again. */
        uint64_t *w = (uint64_t *)g_map(frame_addr(index));
        if (w) {
            w[1] = (uint64_t)(uintptr_t)g_put_tag;
        }
    }
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

uint64_t vibeos_frame_phys_at(uint32_t index) {
    if (index >= g_entries) {
        return 0ull;
    }
    return frame_addr(index);
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

void vibeos_frame_set_flag(uint64_t phys, uint8_t flag) {
    uint32_t index;

    frame_lock();
    index = frame_index(phys);
    if (index != FRAME_NONE) {
        g_table[index].flags |= flag;
    }
    frame_unlock();
}

void vibeos_frame_clear_flag(uint64_t phys, uint8_t flag) {
    uint32_t index;

    frame_lock();
    index = frame_index(phys);
    if (index != FRAME_NONE) {
        g_table[index].flags &= (uint8_t)~flag;
    }
    frame_unlock();
}

int vibeos_frame_test_flag(uint64_t phys, uint8_t flag) {
    uint32_t index;
    int set = 0;

    frame_lock();
    index = frame_index(phys);
    if (index != FRAME_NONE) {
        set = (g_table[index].flags & flag) != 0u;
    }
    frame_unlock();
    return set;
}

void vibeos_frame_get(uint64_t phys) {
    frame_lock();
    vibeos_frame_get_locked(phys);
    frame_unlock();
}

int vibeos_frame_put_why(uint64_t phys, const void *tag) {
    int r;

    frame_lock();
    g_put_tag = tag;
    r = vibeos_frame_put_locked(phys);
    g_put_tag = 0;
    frame_unlock();

    if (r && g_watch) {
        g_watch(phys);
    }
    return r;
}

int vibeos_frame_put(uint64_t phys) {
    int r;

    frame_lock();
    r = vibeos_frame_put_locked(phys);
    frame_unlock();

    /* After the release, and only when the frame actually went back on the free
     * list. The first version asked before the decrement, on the prediction
     * that owners == 1 meant this put would free it - and a prediction taken
     * while other cores are mapping and unmapping is not a fact. It reported
     * frames that were never freed at all, which is the one thing a detector
     * must not do: eleven boots were failed by the detector after the defect it
     * was built for had been fixed.
     *
     * Asking afterwards is unambiguous. The frame is on the free list; if a
     * live process still maps it, that is wrong however it happened. Outside
     * the lock because the check walks page tables, which is not work to do
     * with interrupts masked. */
    if (r && g_watch) {
        g_watch(phys);
    }
    return r;
}

uint32_t vibeos_frame_id(uint64_t phys) {
    return frame_index(phys);
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
