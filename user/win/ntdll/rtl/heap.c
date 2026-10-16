/* The allocator under the Rtl functions that allocate (docs/abi/windows.md W2).
 *
 * There is no heap service yet - NtAllocateVirtualMemory is W3 - so the Rtl runtime
 * carves a static arena in .bss: first-fit over a free list kept in address
 * order, so that a free can merge with both neighbours. Blocks are 16-byte
 * aligned, headers 16 bytes. It is deliberately the smallest thing that does not
 * fragment itself to death, and it sits behind vibeos_rtl_alloc / vibeos_rtl_free
 * alone because RtlAllocateHeap is meant to replace it without anyone else
 * noticing.
 *
 * One thread per process today, so there is no lock. W5 brings threads and
 * this is the first place that has to take one. */

#include "rtl_internal.h"

#define HDR      16u
#define MIN_BLK  32u
#define M_ALLOC  0x41425255524C4F43ull   /* "COLRURBA": any value that is unlikely by accident */
#define M_FREE   0x45454552464C4F43ull

typedef struct blk {
    size_t   size;     /* the whole block, header included */
    uint64_t magic;
} blk_t;

/* A free block keeps its list link in the first word of its payload. */
typedef struct freeblk {
    blk_t           h;
    struct freeblk *next;
} freeblk_t;

static uint8_t g_arena[VIBEOS_RTL_ARENA_SIZE] __attribute__((aligned(16)));
static freeblk_t *g_free;
static int g_ready;

static void heap_init(void) {
    g_free = (freeblk_t *)g_arena;
    g_free->h.size = VIBEOS_RTL_ARENA_SIZE;
    g_free->h.magic = M_FREE;
    g_free->next = 0;
    g_ready = 1;
}

void *vibeos_rtl_alloc(size_t size) {
    freeblk_t *prev = 0, *cur;
    size_t need;

    if (!g_ready) {
        heap_init();
    }
    if (size == 0) {
        size = 1;                         /* a zero-byte request still gets its own block */
    }
    if (size > VIBEOS_RTL_ARENA_SIZE) {
        return 0;                         /* also keeps the rounding below from wrapping */
    }
    need = ((size + 15u) & ~(size_t)15u) + HDR;
    if (need < MIN_BLK) {
        need = MIN_BLK;
    }
    for (cur = g_free; cur; prev = cur, cur = cur->next) {
        if (cur->h.size < need) {
            continue;
        }
        if (cur->h.size - need >= MIN_BLK) {
            /* Split: the tail stays on the list in this block's place. */
            freeblk_t *rest = (freeblk_t *)((uint8_t *)cur + need);

            rest->h.size = cur->h.size - need;
            rest->h.magic = M_FREE;
            rest->next = cur->next;
            if (prev) {
                prev->next = rest;
            } else {
                g_free = rest;
            }
            cur->h.size = need;
        } else if (prev) {
            prev->next = cur->next;
        } else {
            g_free = cur->next;
        }
        cur->h.magic = M_ALLOC;
        return (uint8_t *)cur + HDR;
    }
    return 0;
}

void vibeos_rtl_free(void *p) {
    uint8_t *b = (uint8_t *)p;
    freeblk_t *blk, *prev = 0, *cur;

    if (!g_ready || b < g_arena + HDR || b >= g_arena + VIBEOS_RTL_ARENA_SIZE ||
        (((uintptr_t)b) & 15u) != 0) {
        return;                           /* not ours, or nothing allocated yet */
    }
    blk = (freeblk_t *)(b - HDR);
    if (blk->h.magic != M_ALLOC) {
        return;                           /* a double free, or an interior pointer */
    }
    blk->h.magic = M_FREE;
    for (cur = g_free; cur && cur < blk; prev = cur, cur = cur->next) {
    }
    blk->next = cur;
    if (prev) {
        prev->next = blk;
    } else {
        g_free = blk;
    }
    /* Merge forwards, then backwards: both neighbours may be free at once. */
    if (cur && (uint8_t *)blk + blk->h.size == (uint8_t *)cur) {
        blk->h.size += cur->h.size;
        blk->next = cur->next;
        cur->h.magic = 0;
    }
    if (prev && (uint8_t *)prev + prev->h.size == (uint8_t *)blk) {
        prev->h.size += blk->h.size;
        prev->next = blk->next;
        blk->h.magic = 0;
    }
}

void vibeos_rtl_heap_stats(size_t *free_bytes, size_t *largest, size_t *blocks) {
    freeblk_t *c;
    size_t f = 0, l = 0, n = 0;

    if (!g_ready) {
        heap_init();
    }
    for (c = g_free; c; c = c->next) {
        f += c->h.size;
        if (c->h.size > l) {
            l = c->h.size;
        }
        n++;
    }
    if (free_bytes) {
        *free_bytes = f;
    }
    if (largest) {
        *largest = l;
    }
    if (blocks) {
        *blocks = n;
    }
}
