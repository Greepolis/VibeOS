#ifndef VIBEOS_VMA_H
#define VIBEOS_VMA_H

#include <stdint.h>

#include "vibeos/mm_model.h"

/* L2: regions. A process's memory as it asked for it, rather than as it
 * happens to be mapped.
 *
 * `munmap` used to answer "what should I release here?" by walking the page
 * tables and acting on whatever it found. That is the wrong source: the page
 * tables record what the hardware currently does, not what the program
 * requested, and the difference is where shared frames got released. A region
 * list is the record of the request, and it is also the prerequisite for
 * everything in L3 - a fault can only be resolved from a backing store if
 * something remembers what backs the address.
 *
 * Deliberately a sorted singly-linked list rather than a tree. A process here
 * has a handful of regions, the operations are insert, find and split, and a
 * tree would be more code to be wrong in for no measurable gain at this size.
 * If a workload ever shows the walk costing anything, the shape can change
 * behind these five functions.
 */

/* What a region's pages are (docs/abi/windows.md W3). PRESENT is every
 * region until W3: whoever inserted it mapped its pages. DEMAND is anonymous
 * memory made on first touch - a zeroed page mapped by the page fault, as the
 * stack's growth area always was. RESERVED is address space set aside with no
 * pages and no access: a touch is a fault the program takes, and the range is
 * nobody else's to map - which an empty range munmap had freed could not say. */
typedef enum {
    VIBEOS_VMA_PRESENT = 0,
    VIBEOS_VMA_DEMAND = 1,
    VIBEOS_VMA_RESERVED = 2
} vibeos_vma_state_t;

typedef struct vibeos_vma {
    uint64_t base;                  /* page aligned, inclusive              */
    uint64_t len;                   /* page multiple, non-zero              */
    vibeos_prot_t prot;
    vibeos_backing_kind_t backing;
    uint32_t backing_id;            /* file/inode handle, or 0 for anonymous */
    uint64_t backing_offset;
    /* W3: the state above, and the allocation the region belongs to - its base
     * and the protection it was asked for, which NtQueryVirtualMemory reports
     * and which keeps two allocations from merging into one. 0 for a region no
     * personality allocated that way. `user_prot` is the protection as the
     * personality spelled it - PAGE_EXECUTE, PAGE_GUARD, which `prot` cannot
     * say - reported back as it was given; 0 when nobody spelled one.
     * `alloc_prot` is the personality's word about the allocation and opaque
     * here: NT keeps its placeholder marks in bits no PAGE_* value uses. */
    uint32_t state;
    uint32_t user_prot;
    uint32_t alloc_prot;
    uint64_t alloc_base;
    struct vibeos_vma *next;
} vibeos_vma_t;

/* The list head a process carries. A bare pointer would do; the struct exists
 * so a count can be kept without every caller remembering to update one. */
typedef struct vibeos_vma_list {
    vibeos_vma_t *head;             /* sorted by base, never overlapping */
    uint32_t count;
} vibeos_vma_list_t;

/* Where regions come from. The list does not allocate: the caller supplies a
 * pool, because this layer is host-tested with a static array and used in the
 * kernel with frames, and neither should know about the other.
 *
 * `vibeos_vma_pool_init` hands the layer an array to carve from; every list
 * created afterwards draws on it. */
void vibeos_vma_pool_init(vibeos_vma_t *pool, uint32_t entries);

/* Serialise this layer against itself, exactly as the frame layer does.
 *
 * The pool is a free list threaded through the descriptors, and mmap, munmap,
 * exec and fork all reach it from any core. Without this, two allocations
 * racing both take the head and both advance it: one descriptor is handed to
 * two callers and the rest of the list is lost. The symptom was eight refused
 * inserts in a boot whose peak usage was twenty-nine descriptors out of two
 * thousand - a pool that was nowhere near full and had shredded its own list.
 *
 * The same mistake the frame layer made, repeated here. A layer several cores
 * can drive has to defend itself; "remember to hold the lock" is not a property
 * a compiler checks. Both may be null, which is what a host test passes. */
void vibeos_vma_set_lock(void (*lock)(void), void (*unlock)(void));

/* How many regions are in use across every list. The boot gate asserts this
 * returns to zero once userland has finished: a region leaked per process is a
 * machine that stops accepting new ones after a few hundred commands, and the
 * failure appears as an unrelated refusal much later. */
uint32_t vibeos_vma_live(void);

/* Add a region. Refuses an overlap rather than silently merging over one:
 * `mmap` picking an address that is already spoken for is a bug in the caller,
 * and quietly absorbing it hides which caller. Merges with an adjacent region
 * when everything about them matches, so a program that grows its heap one page
 * at a time does not accumulate a thousand descriptors.
 *
 * Returns 0, or negative on overlap or an exhausted pool. */
int vibeos_vma_insert(vibeos_vma_list_t *list, uint64_t base, uint64_t len,
                      vibeos_prot_t prot, vibeos_backing_kind_t backing,
                      uint32_t backing_id, uint64_t backing_offset);

/* Add an anonymous region of an allocation: its state (DEMAND or RESERVED, or
 * PRESENT when the caller maps it), its protection in both spellings, the
 * allocation's base and the protection it was asked for. As vibeos_vma_insert
 * otherwise. */
int vibeos_vma_insert_alloc(vibeos_vma_list_t *list, uint64_t base, uint64_t len,
                            vibeos_prot_t prot, uint32_t user_prot, vibeos_vma_state_t state,
                            uint64_t alloc_base, uint32_t alloc_prot);

/* Add a region like `t` - every field of it but the range and the link - at
 * [base, base+len). For a caller that has more to say than insert or
 * insert_alloc carry: a view of a section is a file's pages, of an allocation,
 * with a spelled protection (W3 step 5). As vibeos_vma_insert otherwise. */
int vibeos_vma_insert_like(vibeos_vma_list_t *list, uint64_t base, uint64_t len,
                           const vibeos_vma_t *t);

/* Change [base, base+len): its protection (`prot` and `user_prot` together) if
 * `what` has VIBEOS_VMA_CHANGE_PROT, its state - commit (DEMAND), decommit
 * (RESERVED) - if it has VIBEOS_VMA_CHANGE_STATE; splitting as protect does.
 * The same all-or-nothing rule: every page of the range must be in a region,
 * or nothing changes and the answer is negative. */
#define VIBEOS_VMA_CHANGE_PROT  1u
#define VIBEOS_VMA_CHANGE_STATE 2u
#define VIBEOS_VMA_CHANGE_ALLOC 4u   /* vibeos_vma_change_alloc's alone */
int vibeos_vma_change(vibeos_vma_list_t *list, uint64_t base, uint64_t len, uint32_t what,
                      vibeos_prot_t prot, uint32_t user_prot, vibeos_vma_state_t state);

/* As vibeos_vma_change, and with VIBEOS_VMA_CHANGE_ALLOC the allocation the
 * range belongs to as well: its base and protection. Windows 10 splits one
 * reservation into several and joins them again (placeholders, docs/abi/
 * windows.md W3 step 10), and an allocation is only the regions that name one
 * base - so splitting one is giving part of it a base of its own. */
int vibeos_vma_change_alloc(vibeos_vma_list_t *list, uint64_t base, uint64_t len, uint32_t what,
                            vibeos_prot_t prot, uint32_t user_prot, vibeos_vma_state_t state,
                            uint64_t alloc_base, uint32_t alloc_prot);

/* The region containing `va`, or null. */
vibeos_vma_t *vibeos_vma_find(vibeos_vma_list_t *list, uint64_t va);

/* Remove [base, base+len), splitting regions that only partly overlap it.
 *
 * The partial case is the one that matters and the one a page-table walk got
 * wrong: unmapping the middle of a region leaves two, and unmapping the front
 * or back leaves one with different bounds. Returns the number of bytes
 * actually removed, which is not `len` when the range has holes - or
 * VIBEOS_VMA_REFUSED, with nothing changed, when the middle of a region was
 * asked for and no descriptor was left to split it with. That used to answer
 * 0, the same as "nothing there", and the caller unmapped the pages of a
 * region that went on describing them (external review, 2026-10-07).
 */
#define VIBEOS_VMA_REFUSED (~0ull)
uint64_t vibeos_vma_remove(vibeos_vma_list_t *list, uint64_t base, uint64_t len);

/* Change the protection of [base, base+len), splitting as needed. Returns 0, or
 * negative if any page in the range is not mapped - the whole range is checked
 * before anything changes, so a refusal leaves the list untouched. */
int vibeos_vma_protect(vibeos_vma_list_t *list, uint64_t base, uint64_t len,
                       vibeos_prot_t prot);

/* Copy every region of `src` into `dst`, which must be empty. This is fork.
 * Returns 0, or negative on an exhausted pool - and on failure `dst` is emptied
 * again, so a half-cloned list never escapes. */
int vibeos_vma_clone(vibeos_vma_list_t *dst, const vibeos_vma_list_t *src);

/* Release every region. Used at exit, and by clone's own failure path. */
void vibeos_vma_clear(vibeos_vma_list_t *list);

#endif /* VIBEOS_VMA_H */
