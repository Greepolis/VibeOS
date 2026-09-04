/* L1a: the reverse map. See include/vibeos/rmap.h for why it exists.
 *
 * A chained hash from frame index to a list of holders, out of a fixed pool.
 * The layer never allocates: it is called from inside the address-space layer,
 * which is called with the frame lock held in places, and a layer that can call
 * back into the allocator from there is a deadlock waiting for the first time
 * memory is tight.
 *
 * Running out of nodes is therefore a *reported* condition and not a failure.
 * Losing the ability to move or evict a frame degrades reclaim; failing the
 * mapping that wanted to be recorded would break a program that has done
 * nothing wrong. The counter says how often it happened and the high-water mark
 * says what the pool should have been.
 */

#include "vibeos/rmap.h"
#include "vibeos/mm_stats.h"

#define RMAP_NONE 0xFFFFFFFFu

typedef struct rmap_node {
    uint64_t root_phys;
    uint64_t va;
    uint32_t frame;        /* frame index, so a node knows its own bucket    */
    uint32_t next;         /* next holder of the same frame                  */
} rmap_node_t;

static rmap_node_t *g_nodes;
static uint32_t     g_node_count;
static uint32_t     g_free_head = RMAP_NONE;

/* One list head per frame. Indexed by frame index, so lookup is direct rather
 * than hashed: the table is one 32-bit word per frame, which is a quarter of
 * what the frame descriptor already costs, and it removes a whole class of
 * question about collisions from a structure whose entire job is to be
 * trustworthy about identity. */
static uint32_t *g_head;
static uint32_t  g_frames;

static uint64_t g_base_phys;
static int      g_ready;

static vibeos_rmap_stats_t g_stats;

vibeos_rmap_stats_t *vibeos_rmap_stats(void) {
    return &g_stats;
}

static void (*g_lock)(void);
static void (*g_unlock)(void);

void vibeos_rmap_set_lock(void (*lock)(void), void (*unlock)(void)) {
    g_lock = lock;
    g_unlock = unlock;
}

static void rmap_lock(void) {
    if (g_lock) {
        g_lock();
    }
}

static void rmap_unlock(void) {
    if (g_unlock) {
        g_unlock();
    }
}

/* The frame layer owns the mapping from address to index; this layer is given
 * the base and count so it does not have to call back into it while holding a
 * different lock. */
void vibeos_rmap_set_base(uint64_t base_phys) {
    g_base_phys = base_phys;
}

static uint32_t frame_index(uint64_t phys) {
    uint64_t off;

    if (!g_ready || phys < g_base_phys) {
        return RMAP_NONE;
    }
    off = (phys - g_base_phys) >> 12;
    if (off >= (uint64_t)g_frames) {
        return RMAP_NONE;
    }
    return (uint32_t)off;
}

int vibeos_rmap_init(void *pool, uint64_t bytes, uint32_t frames) {
    uint64_t heads_bytes;
    uint64_t node_bytes;
    uint32_t i;
    unsigned char *p = (unsigned char *)pool;

    if (!pool || frames == 0u) {
        return -1;
    }
    heads_bytes = (uint64_t)frames * sizeof(uint32_t);
    if (bytes <= heads_bytes + sizeof(rmap_node_t)) {
        return -1;   /* no room for even one node; nothing changed (I5) */
    }
    node_bytes = bytes - heads_bytes;

    g_head = (uint32_t *)p;
    g_frames = frames;
    g_nodes = (rmap_node_t *)(p + heads_bytes);
    g_node_count = (uint32_t)(node_bytes / sizeof(rmap_node_t));

    for (i = 0; i < g_frames; i++) {
        g_head[i] = RMAP_NONE;
    }
    for (i = 0; i < g_node_count; i++) {
        g_nodes[i].next = (i + 1u < g_node_count) ? (i + 1u) : RMAP_NONE;
        g_nodes[i].frame = RMAP_NONE;
        g_nodes[i].root_phys = 0;
        g_nodes[i].va = 0;
    }
    g_free_head = (g_node_count != 0u) ? 0u : RMAP_NONE;

    g_stats.nodes_used = 0;
    g_stats.nodes_peak = 0;
    g_stats.exhausted = 0;
    g_stats.missing_remove = 0;
    g_ready = 1;
    return 0;
}

/* ---- the list ------------------------------------------------------------ */

static int add_locked(uint32_t idx, uint64_t root_phys, uint64_t va) {
    uint32_t cur = g_head[idx];
    uint32_t n;

    /* Already recorded? Mapping over an existing entry is legitimate - the
     * copy-on-write fault does it, and so does a second map of the same page -
     * and must not lengthen the list, or the count stops matching `owners` and
     * the invariant this layer is checked by becomes noise. */
    while (cur != RMAP_NONE) {
        if (g_nodes[cur].root_phys == root_phys && g_nodes[cur].va == va) {
            return 0;
        }
        cur = g_nodes[cur].next;
    }

    if (g_free_head == RMAP_NONE) {
        g_stats.exhausted++;
        return -1;
    }
    n = g_free_head;
    g_free_head = g_nodes[n].next;

    g_nodes[n].root_phys = root_phys;
    g_nodes[n].va = va;
    g_nodes[n].frame = idx;
    g_nodes[n].next = g_head[idx];
    g_head[idx] = n;

    g_stats.nodes_used++;
    if (g_stats.nodes_used > g_stats.nodes_peak) {
        g_stats.nodes_peak = g_stats.nodes_used;
    }
    return 0;
}

static void release_node(uint32_t n) {
    g_nodes[n].frame = RMAP_NONE;
    g_nodes[n].root_phys = 0;
    g_nodes[n].va = 0;
    g_nodes[n].next = g_free_head;
    g_free_head = n;
    if (g_stats.nodes_used > 0u) {
        g_stats.nodes_used--;
    }
}

int vibeos_rmap_add(uint64_t frame_phys, uint64_t root_phys, uint64_t va) {
    uint32_t idx;
    int rc;

    rmap_lock();
    idx = frame_index(frame_phys);
    if (idx == RMAP_NONE) {
        rmap_unlock();
        return -1;
    }
    rc = add_locked(idx, root_phys, va & ~0xFFFull);
    rmap_unlock();
    return rc;
}

int vibeos_rmap_remove(uint64_t frame_phys, uint64_t root_phys, uint64_t va) {
    uint32_t idx, cur, prev = RMAP_NONE;

    rmap_lock();
    idx = frame_index(frame_phys);
    if (idx == RMAP_NONE) {
        rmap_unlock();
        return -1;
    }
    va &= ~0xFFFull;
    cur = g_head[idx];
    while (cur != RMAP_NONE) {
        if (g_nodes[cur].root_phys == root_phys && g_nodes[cur].va == va) {
            if (prev == RMAP_NONE) {
                g_head[idx] = g_nodes[cur].next;
            } else {
                g_nodes[prev].next = g_nodes[cur].next;
            }
            release_node(cur);
            rmap_unlock();
            return 0;
        }
        prev = cur;
        cur = g_nodes[cur].next;
    }
    /* Counted, because it means the two sides disagree about what was mapped -
     * which is the same shape as the defect this subsystem keeps producing,
     * seen from the other end. */
    g_stats.missing_remove++;
    rmap_unlock();
    return -1;
}

void vibeos_rmap_forget_frame(uint64_t frame_phys) {
    uint32_t idx, cur;

    rmap_lock();
    idx = frame_index(frame_phys);
    if (idx == RMAP_NONE) {
        rmap_unlock();
        return;
    }
    cur = g_head[idx];
    g_head[idx] = RMAP_NONE;
    while (cur != RMAP_NONE) {
        uint32_t next = g_nodes[cur].next;
        release_node(cur);
        cur = next;
    }
    rmap_unlock();
}

void vibeos_rmap_forget_root(uint64_t root_phys) {
    uint32_t i;

    rmap_lock();
    if (!g_ready) {
        rmap_unlock();
        return;
    }
    for (i = 0; i < g_frames; i++) {
        uint32_t cur = g_head[i];
        uint32_t prev = RMAP_NONE;

        while (cur != RMAP_NONE) {
            uint32_t next = g_nodes[cur].next;

            if (g_nodes[cur].root_phys == root_phys) {
                if (prev == RMAP_NONE) {
                    g_head[i] = next;
                } else {
                    g_nodes[prev].next = next;
                }
                release_node(cur);
            } else {
                prev = cur;
            }
            cur = next;
        }
    }
    rmap_unlock();
}

uint32_t vibeos_rmap_count(uint64_t frame_phys) {
    uint32_t idx, cur, n = 0;

    rmap_lock();
    idx = frame_index(frame_phys);
    if (idx == RMAP_NONE) {
        rmap_unlock();
        return 0;
    }
    for (cur = g_head[idx]; cur != RMAP_NONE; cur = g_nodes[cur].next) {
        n++;
    }
    rmap_unlock();
    return n;
}

uint32_t vibeos_rmap_holders(uint64_t frame_phys, vibeos_rmap_holder_t *out,
                             uint32_t max) {
    uint32_t idx, cur, n = 0;

    if (!out || max == 0u) {
        return 0;
    }
    rmap_lock();
    idx = frame_index(frame_phys);
    if (idx == RMAP_NONE) {
        rmap_unlock();
        return 0;
    }
    for (cur = g_head[idx]; cur != RMAP_NONE && n < max; cur = g_nodes[cur].next) {
        out[n].root_phys = g_nodes[cur].root_phys;
        out[n].va = g_nodes[cur].va;
        n++;
    }
    rmap_unlock();
    return n;
}
