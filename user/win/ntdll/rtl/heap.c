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
#define M_ALLOC  0x41425255524C4F00ull   /* any value that is unlikely by accident */
#define M_FREE   0x45454552464C4F43ull
/* An allocated block's magic carries in its low byte how many payload bytes
 * past the request it has - under MIN_BLK + 16, so a byte holds it - which is
 * how RtlSizeHeap answers with the size that was asked for, as Windows does. */
#define M_MASK   (~(uint64_t)0xFF)

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
    size_t need, asked = size;

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
        cur->h.magic = M_ALLOC | (uint64_t)(cur->h.size - HDR - asked);
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
    if ((blk->h.magic & M_MASK) != M_ALLOC) {
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

size_t vibeos_rtl_size(const void *p) {
    const uint8_t *b = (const uint8_t *)p;
    const blk_t *blk;

    if (!g_ready || b < g_arena + HDR || b >= g_arena + VIBEOS_RTL_ARENA_SIZE ||
        (((uintptr_t)b) & 15u) != 0) {
        return (size_t)-1;
    }
    blk = (const blk_t *)(b - HDR);
    if ((blk->magic & M_MASK) != M_ALLOC) {
        return (size_t)-1;
    }
    return blk->size - HDR - (size_t)(blk->magic & 0xFFu);
}

/* ---- Windows' heap functions, on the arena ------------------------------------------
 * One heap, the process's: PEB->ProcessHeap names it (start.c) and every handle
 * is taken to mean it. HeapCreate is W3's, with memory of its own to give. */

#define HEAP_ZERO_MEMORY           0x00000008u
#define HEAP_REALLOC_IN_PLACE_ONLY 0x00000010u

void *vibeos_rtl_process_heap(void) {
    return g_arena;
}

RTL_API void *RtlAllocateHeap(void *heap, uint32_t flags, size_t size) {
    void *p;

    (void)heap;
    p = vibeos_rtl_alloc(size);
    if (p && (flags & HEAP_ZERO_MEMORY)) {
        memset(p, 0, size);
    }
    return p;
}

RTL_API vibeos_boolean_t RtlFreeHeap(void *heap, uint32_t flags, void *p) {
    (void)heap;
    (void)flags;
    if (!p) {
        return 1;                         /* freeing nothing succeeds */
    }
    if (vibeos_rtl_size(p) == (size_t)-1) {
        return 0;
    }
    vibeos_rtl_free(p);
    return 1;
}

RTL_API size_t RtlSizeHeap(void *heap, uint32_t flags, const void *p) {
    (void)heap;
    (void)flags;
    return vibeos_rtl_size(p);
}

/* A new block and a copy, or - with HEAP_REALLOC_IN_PLACE_ONLY - the same block
 * if the size fits what it already has, and nothing otherwise. Bytes past the
 * old size are zero when the caller asked for HEAP_ZERO_MEMORY. */
RTL_API void *RtlReAllocateHeap(void *heap, uint32_t flags, void *p, size_t size) {
    size_t old;
    void *n;

    if (!p) {
        return 0;
    }
    old = vibeos_rtl_size(p);
    if (old == (size_t)-1) {
        return 0;
    }
    if (flags & HEAP_REALLOC_IN_PLACE_ONLY) {
        blk_t *blk = (blk_t *)((uint8_t *)p - HDR);

        /* Shrinking by more than the magic's byte can say would need the
         * block split; refused instead, which the flag allows. */
        if (size > blk->size - HDR || blk->size - HDR - size > 0xFFu) {
            return 0;
        }
        if ((flags & HEAP_ZERO_MEMORY) && size > old) {
            memset((uint8_t *)p + old, 0, size - old);
        }
        blk->magic = M_ALLOC | (uint64_t)(blk->size - HDR - size);
        return p;
    }
    n = RtlAllocateHeap(heap, flags & ~HEAP_ZERO_MEMORY, size);
    if (!n) {
        return 0;
    }
    memcpy(n, p, old < size ? old : size);
    if ((flags & HEAP_ZERO_MEMORY) && size > old) {
        memset((uint8_t *)n + old, 0, size - old);
    }
    vibeos_rtl_free(p);
    return n;
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
