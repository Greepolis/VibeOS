#ifndef VIBEOS_SWAPMAP_H
#define VIBEOS_SWAPMAP_H

#include <stdint.h>

/* L3b: the swap map — slots on a block device, and what is in them.
 *
 * The bookkeeping half of swap, kept apart from the eviction policy on
 * purpose. Deciding *which* page to evict is a judgement that can be wrong
 * without being dangerous; deciding *where* it went cannot. A slot handed out
 * twice, or freed while a page still refers to it, does not degrade anything -
 * it hands a process another process's memory, silently, at some later fault.
 *
 * So this layer knows nothing about pages, address spaces or pressure. It
 * allocates a slot, gives it back, and moves 4 KiB in either direction. What
 * it guarantees is narrow and checkable:
 *
 *   a slot that is handed out is not handed out again until it is freed;
 *   a read of a slot returns what the last write to that slot put there;
 *   a failed write leaves the slot allocated and reports, rather than
 *     silently losing a page that something has already stopped mapping.
 *
 * ## Why the device is a pair of callbacks
 *
 * The block layer is not portable and this is. The same reason the frame layer
 * takes a map function: a layer that reaches for a specific driver cannot be
 * host-tested, and everything in this subsystem that could not be host-tested
 * is where the defects were.
 */

/* Move one 4 KiB page to or from a slot. Return 0 on success. The context is
 * whatever the caller passed to init - a device handle, usually. */
typedef int (*vibeos_swap_io_fn)(void *ctx, uint32_t slot, void *page, int write);

/* `bitmap` is one bit per slot, so slots/8 bytes rounded up, owned by the
 * caller. The layer never allocates: it is called from reclaim, which is
 * called when memory is short, and a layer that allocates on that path is a
 * layer that fails exactly when it is needed. */
int vibeos_swapmap_init(uint8_t *bitmap, uint32_t slots,
                        vibeos_swap_io_fn io, void *ctx);

/* Its own lock, by registration. The fifth layer here to need one, and the
 * reason has not changed: a layer with mutable statics and more than one
 * possible caller locks itself. */
void vibeos_swapmap_set_lock(void (*lock)(void), void (*unlock)(void));

/* Take a free slot. Returns 0 and writes the slot number, or -1 when swap is
 * full - which is a normal condition, not an error, and the caller decides
 * what to do about it. */
int vibeos_swap_alloc(uint32_t *out_slot);

/* Give a slot back. Refuses, and counts, a slot that is already free: a double
 * free here means two things believe they own the same page of swap, and the
 * second one to write wins without either finding out. */
int vibeos_swap_free(uint32_t slot);

/* 4 KiB in or out. A slot that is not allocated is refused rather than
 * transferred - reading one would return whatever the last tenant left, which
 * is precisely the disclosure this layer exists to prevent. */
int vibeos_swap_write(uint32_t slot, void *page);
int vibeos_swap_read(uint32_t slot, void *page);

int vibeos_swap_is_allocated(uint32_t slot);
uint32_t vibeos_swap_slots(void);

typedef struct vibeos_swap_stats {
    uint64_t allocated;      /* slots currently held                          */
    uint64_t peak;           /* high water mark, so the area can be sized     */
    uint64_t full;           /* an allocation that found nothing free         */
    uint64_t double_free;    /* freeing a slot that was free. MUST BE ZERO    */
    uint64_t bad_slot;       /* an operation on an out-of-range slot. ZERO    */
    uint64_t unallocated_io; /* read or write of a free slot. MUST BE ZERO    */
    uint64_t io_errors;      /* the device refused. SHOULD BE ZERO            */
    uint64_t writes;
    uint64_t reads;
} vibeos_swap_stats_t;

vibeos_swap_stats_t *vibeos_swap_stats(void);

#endif /* VIBEOS_SWAPMAP_H */
