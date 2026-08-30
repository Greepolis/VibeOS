#ifndef VIBEOS_FRAME_H
#define VIBEOS_FRAME_H

#include <stdint.h>

#include "vibeos/mm_model.h"

/* L0: physical frames. The only code that owns the free list or a reference
 * count. See docs/mm/ for why this is a layer rather than a few helpers.
 *
 * The invariants this file exists to uphold, from mm_model.h:
 *
 *   I1  a frame is freed only when its owner count reaches zero
 *   I2  a frame outside the table is never freed
 *   I4  handed out zeroed, released poisoned, poison verified on reuse
 *   I5  a failed allocation changes nothing
 *
 * The caller supplies the descriptor table and the mapping from a physical
 * address to something the CPU can write, which is what lets this run in a host
 * test with a malloc'd region standing in for RAM. The kernel passes an
 * identity mapping; the tests pass an offset. Neither is baked in.
 */

/* Turn a physical address into a pointer this code may write. The kernel's is
 * the identity map; a host test's is its own buffer. Returning null means "not
 * addressable", and the layer will then skip poisoning rather than fault - a
 * frame it cannot touch is still a frame it can count. */
typedef void *(*vibeos_frame_map_fn)(uint64_t phys);

/* Bring the layer up over [base, base+len), with `table` holding one descriptor
 * per frame. `entries` must be at least len/4096; anything beyond that is
 * ignored. Returns 0 on success.
 *
 * Every frame starts FREE and poisoned. Reserving comes after. */
int vibeos_frame_init(uint64_t base, uint64_t len,
                      vibeos_frame_t *table, uint32_t entries,
                      vibeos_frame_map_fn map);

/* Take a range out of circulation permanently: firmware memory, and the low
 * user window whose physical addresses a Linux process can shadow. Must be
 * called before the first allocation, and refuses a range that is not entirely
 * free. */
int vibeos_frame_reserve(uint64_t base, uint64_t len);

/* One frame, zeroed, with one owner and the given state. Returns 0 when there
 * is nothing left - and changes nothing when it does (I5). */
uint64_t vibeos_frame_alloc(vibeos_frame_state_t state);

/* One more address space maps this frame. Silently ignores a frame the table
 * does not describe: not knowing about a frame is not a reason to miscount a
 * different one. */
void vibeos_frame_get(uint64_t phys);

/* One fewer owner. Returns non-zero when that was the last one and the frame
 * has been returned to the free list, poisoned.
 *
 * Refuses, and counts, two cases that were previously silent corruption: a
 * frame outside the table (I2), and a frame that already had no owners. Both
 * leak rather than corrupt, which is the direction that can be measured. */
int vibeos_frame_put(uint64_t phys);

/* Queries, for the copy-on-write path and for inspection. An unknown frame
 * reports zero owners and state FREE. */
uint32_t vibeos_frame_owners(uint64_t phys);
vibeos_frame_state_t vibeos_frame_state(uint64_t phys);

/* Totals, for meminfo and the boot gate. */
uint64_t vibeos_frame_total(void);
uint64_t vibeos_frame_free_count(void);

#endif /* VIBEOS_FRAME_H */
