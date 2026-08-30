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

/* Watch the last release of a frame.
 *
 * Called just before a frame goes back on the free list, with the address of
 * the frame whose owner count is about to reach zero. It exists because the
 * free-while-mapped detector was installed in the architecture's free path, and
 * since the rewrite almost nothing frees a frame through there any more: the
 * address-space layer releases directly. The detector was watching a door
 * nothing walks through, and "it is silent" was being read as evidence.
 *
 * Sampled by the caller, not here - the check walks page tables and is far too
 * expensive to run on every release. */
void vibeos_frame_set_release_watch(void (*watch)(uint64_t phys));

/* Serialise this layer against itself.
 *
 * The lock belongs here, not at the call sites. It used to be taken by the
 * architecture around each of the three or four places that touched a frame,
 * which worked exactly as long as nothing new touched one - and then the
 * copy-on-write fault moved into the address-space layer, allocated a frame
 * without it, and two cores resolving a fault at the same moment corrupted the
 * free list. The boot wedged with no output, which is the expensive kind.
 *
 * A layer that can be driven from several cores has to defend itself, because
 * "remember to hold the memory lock" is not a property a compiler checks.
 *
 * Neither function may allocate, free, or re-enter this layer. Both may be
 * null, which is what a host test passes. */
void vibeos_frame_set_lock(void (*lock)(void), void (*unlock)(void));

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

/* `count` physically contiguous frames, zeroed, each with one owner.
 *
 * This exists because the bootstrap bump allocator has to stop existing. While
 * two allocators hand out frames from the same region, one of them is wrong
 * about what is free - and that is the shape of the defect this rewrite is for.
 * So everything that needed contiguous memory from the bump allocator needs it
 * here instead: the exec staging area, and a two-page kernel stack.
 *
 * First fit over the descriptor table. It is a linear scan, which is fine for
 * something asked for once at boot and twice per task creation, and would not
 * be fine for a page fault - which is why the single-frame path is a free list
 * and this is not.
 *
 * Returns 0 when no run that long is free, having changed nothing (I5).
 */
uint64_t vibeos_frame_alloc_contig(uint32_t count, vibeos_frame_state_t state);

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

/* One walk of the descriptor table, answering what a memory tool actually asks:
 * how many frames are in each state, and how long the longest run of free ones
 * is.
 *
 * Both come from the same walk on purpose. meminfo used to assemble its picture
 * from three independent sources - a bump remainder, a free-list length and a
 * reserved constant - which did not add up to the total and could not, since
 * nothing made them one partition. A tool that reports a memory breakdown whose
 * parts do not sum to the whole teaches people to distrust all of it.
 *
 * `by_state` must have VIBEOS_FRAME_STATE_COUNT entries. Either pointer may be
 * null. The longest free run is the honest measure of fragmentation: free
 * memory that cannot satisfy a contiguous request is not usable memory, and
 * without this number that only becomes visible as an allocation failure
 * nobody can explain. */
void vibeos_frame_survey(uint64_t *by_state, uint64_t *largest_free_run);

/* Totals, for meminfo and the boot gate. */
uint64_t vibeos_frame_total(void);
uint64_t vibeos_frame_free_count(void);

#endif /* VIBEOS_FRAME_H */
