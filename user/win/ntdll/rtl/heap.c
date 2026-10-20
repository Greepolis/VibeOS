/* The allocator under the Rtl functions that allocate (docs/abi/windows.md W2,
 * W3 step 3).
 *
 * First-fit over a free list kept in address order, so that a free can merge
 * with both neighbours. Blocks are 16-byte aligned, headers 16 bytes. It is
 * deliberately the smallest thing that does not fragment itself to death, and
 * it sits behind vibeos_rtl_alloc / vibeos_rtl_free so that the Heap functions
 * below and everything else in Rtl share it.
 *
 * The memory comes in segments from vibeos_rtl_pages - NtAllocateVirtualMemory
 * in ntdll.dll, a pool on the host - when the free list has nothing that fits.
 * Until W3 it was a 512 KiB arena in ntdll's .bss, which every NT process paid
 * for in full before running a line and which no process could outgrow. A
 * segment is committed memory, made by the page fault when first touched, so a
 * process pays for what it reaches. Segments are never given back: a heap that
 * shrank would have to know a segment holds nothing, and Windows' own process
 * heap keeps what it reserved too.
 *
 * Two segments that happen to be adjacent are one run of memory, and a free
 * block may span the seam - the list cares about addresses, not about who
 * asked for them. What is "ours" is any address inside a segment.
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

/* The segments, in address order. */
static struct {
    uint8_t *base;
    size_t size;
} g_seg[VIBEOS_RTL_SEGMENTS_MAX];
static uint32_t g_nseg;
static size_t g_total;
static freeblk_t *g_free;

/* The process heap's handle: what PEB->ProcessHeap holds and every Heap
 * function is given. Its address, nothing more - there is one heap. */
static uint8_t g_heap_handle[16] __attribute__((aligned(16)));

static int ours(const uint8_t *p) {
    uint32_t i;

    for (i = 0; i < g_nseg; i++) {
        if (p >= g_seg[i].base && (size_t)(p - g_seg[i].base) < g_seg[i].size) {
            return 1;
        }
    }
    return 0;
}

/* Put a free block on the list in its place, merging with whichever
 * neighbours it touches. */
static void insert_free(freeblk_t *blk) {
    freeblk_t *prev = 0, *cur;

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

/* A new segment for a block of `need` bytes: at least VIBEOS_RTL_SEGMENT, and
 * whole 64 KiB pieces, which is what Windows reserves in. 0 when the table or
 * the machine has no more to give. */
static int grow(size_t need) {
    size_t bytes = need < VIBEOS_RTL_SEGMENT ? VIBEOS_RTL_SEGMENT
                                             : (need + 0xFFFFu) & ~(size_t)0xFFFFu;
    uint8_t *mem;
    uint32_t i;

    if (g_nseg == VIBEOS_RTL_SEGMENTS_MAX || (mem = vibeos_rtl_pages(bytes)) == 0) {
        return 0;
    }
    if (((uintptr_t)mem & 15u) != 0) {
        return 0;   /* the hook's promise; a block header would be misaligned */
    }
    for (i = g_nseg; i > 0 && g_seg[i - 1].base > mem; i--) {
        g_seg[i] = g_seg[i - 1];
    }
    g_seg[i].base = mem;
    g_seg[i].size = bytes;
    g_nseg++;
    g_total += bytes;
    ((freeblk_t *)mem)->h.size = bytes;
    insert_free((freeblk_t *)mem);
    return 1;
}

void *vibeos_rtl_alloc(size_t size) {
    freeblk_t *prev, *cur;
    size_t need, asked = size;
    int grown = 0;

    if (size == 0) {
        size = 1;                         /* a zero-byte request still gets its own block */
    }
    if (size > VIBEOS_RTL_ALLOC_MAX) {
        return 0;                         /* also keeps the rounding below from wrapping */
    }
    need = ((size + 15u) & ~(size_t)15u) + HDR;
    if (need < MIN_BLK) {
        need = MIN_BLK;
    }
    for (;;) {
        for (prev = 0, cur = g_free; cur; prev = cur, cur = cur->next) {
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
        /* Nothing fits: one new segment that does, and once - a second miss
         * would mean the segment was not what was asked for. */
        if (grown || !grow(need)) {
            return 0;
        }
        grown = 1;
    }
}

/* The header of the block `p` is the payload of, or null: inside a segment,
 * aligned, and allocated. */
static blk_t *block_of(const void *p) {
    const uint8_t *b = (const uint8_t *)p;
    blk_t *blk;

    if (!b || (((uintptr_t)b) & 15u) != 0 || !ours(b - HDR)) {
        return 0;                         /* not ours */
    }
    blk = (blk_t *)(uintptr_t)(b - HDR);
    if ((blk->magic & M_MASK) != M_ALLOC) {
        return 0;                         /* a double free, or an interior pointer */
    }
    return blk;
}

void vibeos_rtl_free(void *p) {
    blk_t *blk = block_of(p);

    if (blk) {
        insert_free((freeblk_t *)blk);
    }
}

size_t vibeos_rtl_size(const void *p) {
    const blk_t *blk = block_of(p);

    if (!blk) {
        return (size_t)-1;
    }
    return blk->size - HDR - (size_t)(blk->magic & 0xFFu);
}

/* Every block of every run of adjacent segments, walked by its headers: the
 * sizes add up to the run exactly, each block is allocated or free, the free
 * ones are on the list in address order and none sits beside another (it would
 * have merged). 0, or the number of the first rule broken. */
int vibeos_rtl_heap_check(size_t *total) {
    const freeblk_t *f = g_free;
    uint32_t i = 0;

    if (total) {
        *total = g_total;
    }
    while (i < g_nseg) {
        uint8_t *at = g_seg[i].base, *end = g_seg[i].base + g_seg[i].size;
        int last_free = 0;

        while (i + 1 < g_nseg && g_seg[i + 1].base == end) {
            end += g_seg[++i].size;   /* the next segment continues this run */
        }
        while (at < end) {
            const blk_t *b = (const blk_t *)at;

            if (b->size < MIN_BLK || (b->size & 15u) != 0 || b->size > (size_t)(end - at)) {
                return 1;
            }
            if (b->magic == M_FREE) {
                if ((const blk_t *)f != b) {
                    return 2;   /* a free block the list skips, or out of order */
                }
                if (last_free) {
                    return 3;   /* two free neighbours that did not merge */
                }
                f = f->next;
                last_free = 1;
            } else if ((b->magic & M_MASK) == M_ALLOC) {
                last_free = 0;
            } else {
                return 4;
            }
            at += b->size;
        }
        i++;
    }
    return f ? 5 : 0;   /* the list names a block no segment holds */
}

/* ---- Windows' heap functions ------------------------------------------------------
 * One heap, the process's: PEB->ProcessHeap names it (start.c) and every handle
 * is taken to mean it. HeapCreate is still to come; it needs nothing but a
 * second free list now that memory can be asked for. */

#define HEAP_ZERO_MEMORY           0x00000008u
#define HEAP_REALLOC_IN_PLACE_ONLY 0x00000010u

void *vibeos_rtl_process_heap(void) {
    return g_heap_handle;
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
