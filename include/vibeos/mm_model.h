#ifndef VIBEOS_MM_MODEL_H
#define VIBEOS_MM_MODEL_H

#include <stdint.h>

#include "vibeos/mm_stats.h"

/* The memory manager's contract.
 *
 * Named mm_model.h and not mm.h because mm.h is already the physical page
 * allocator's header - a collision worth recording, since the first attempt at
 * this file silently replaced it.
 *
 * This header exists before the implementation does, because the thing being
 * replaced failed for want of a stated contract rather than for want of care.
 * One defect - a frame released while another address space still mapped it -
 * was diagnosed four times and fixed three times. Every fix was real. None
 * worked, because "does this address space own this frame?" had no answer
 * stored anywhere, so each call site derived one from hardware bits that mean
 * something else.
 *
 * The invariants, each mapped to a test in docs/mm/maintainability.md:
 *
 *   I1. A frame is freed only when its owner count reaches zero.
 *   I2. A frame outside the frame table is never freed.
 *   I3. Every page-table entry describing user memory is written by exactly one
 *       function, and carries PTE_OWNED if and only if that address space holds
 *       a reference to the frame.
 *   I4. Every frame handed out is zeroed; every frame released is poisoned; the
 *       poison is verified when the frame is handed out again.
 *   I5. An allocation failure leaves no mapping, no reference and no counter
 *       changed.
 *   I6. No path a syscall can reach waits unboundedly for another core.
 *   I7. Every counter in mm_stats.h is either asserted by the boot gate or
 *       documented as not yet meaningful.
 *
 * The full plan, including why this is a rewrite rather than a fifth fix, is in
 * docs/mm/. P0 defines this contract and changes no behaviour: the declarations
 * below are the shape the later phases fill in, and only the statistics and the
 * inspection interface are live today.
 */

/* ---- L0: physical frames (P1) ------------------------------------------- */

typedef enum vibeos_frame_state {
    VIBEOS_FRAME_FREE = 0,     /* on the free list, poisoned                  */
    VIBEOS_FRAME_ALLOCATED,    /* handed out for general use                  */
    VIBEOS_FRAME_RESERVED,     /* never allocatable: firmware, the low window */
    VIBEOS_FRAME_PAGE_TABLE,   /* holds a page table, never user data         */
    VIBEOS_FRAME_CACHE,        /* holds file contents (P4)                    */
    VIBEOS_FRAME_STATE_COUNT
} vibeos_frame_state_t;

/* Per-frame descriptor. Sixteen bytes, 0.4% of the memory this kernel sees -
 * the complete form (decision D2) so the page cache, swap and reclaim are added
 * by filling in fields rather than by migrating this structure and its tests a
 * second time. */
typedef struct vibeos_frame {
    uint16_t owners;           /* address spaces mapping it; 0 means free     */
    uint8_t  state;            /* vibeos_frame_state_t                        */
    uint8_t  flags;            /* PINNED | DIRTY | REFERENCED | SWAP_BACKED   */
    uint32_t backing;          /* backing-store handle, or 0 (P4/P5)          */
    uint32_t lru_next;         /* reclaim lists, unused until P6              */
    uint32_t lru_prev;
} vibeos_frame_t;

#define VIBEOS_FRAME_PINNED       0x01u  /* never reclaimed                   */
#define VIBEOS_FRAME_DIRTY        0x02u  /* differs from its backing store    */
#define VIBEOS_FRAME_REFERENCED   0x04u  /* touched since the last scan       */
#define VIBEOS_FRAME_SWAP_BACKED  0x08u  /* has a slot in the swap map        */

/* ---- L1: address spaces (P2) -------------------------------------------- */

typedef enum vibeos_prot {
    VIBEOS_PROT_NONE  = 0,
    VIBEOS_PROT_READ  = 1u << 0,
    VIBEOS_PROT_WRITE = 1u << 1,
    VIBEOS_PROT_EXEC  = 1u << 2,
    VIBEOS_PROT_USER  = 1u << 3   /* reachable from ring 3                    */
} vibeos_prot_t;

/* ---- L2: regions (P3) ---------------------------------------------------- */

typedef enum vibeos_backing_kind {
    VIBEOS_BACKING_ANON = 0,   /* zero-filled on first touch                  */
    VIBEOS_BACKING_FILE,       /* page cache (P4)                             */
    VIBEOS_BACKING_SHARED      /* shared memory (later)                       */
} vibeos_backing_kind_t;

/* ---- Inspection ---------------------------------------------------------- */

/* What the machine's memory is being used for, right now.
 *
 * The counters in mm_stats.h say what has happened; this says what *is*, which
 * is a different question and the one a person actually asks when a machine is
 * behaving oddly. It is the shape of what RAMMap and a task manager show:
 * a breakdown by state, and per-process totals.
 *
 * Sized rather than allocated, and filled by a caller-provided buffer, because
 * it must be usable from a panic path and from a console command with no heap
 * behind either.
 *
 * P0 defines it and fills in what today's kernel can already answer. The
 * per-process and per-backing figures arrive with L1 and L3; until then they
 * report zero and say so rather than guessing.
 */
typedef struct vibeos_mm_usage {
    uint64_t frames_by_state[VIBEOS_FRAME_STATE_COUNT];  /* the RAMMap view   */
    uint64_t bytes_total;
    uint64_t bytes_free;
    uint64_t bytes_reserved;      /* firmware and the low user window         */
    uint64_t bytes_kernel;        /* page tables, stacks, staging buffers     */
    uint64_t bytes_user;          /* mapped into at least one address space   */
    uint64_t bytes_shared;        /* mapped into more than one - the cost fork
                                   * is saving, which nothing has ever shown  */
    uint64_t bytes_cache;         /* file contents (P4)                       */
    uint64_t largest_free_run;    /* fragmentation, in bytes                  */
    uint32_t processes;           /* how many entries the per-process view has*/
} vibeos_mm_usage_t;

/* Per-process memory, as a task manager would show it. */
typedef struct vibeos_mm_process_usage {
    uint32_t pid;
    uint64_t bytes_mapped;        /* everything this address space maps       */
    uint64_t bytes_private;       /* mapped here and nowhere else             */
    uint64_t bytes_shared;        /* mapped here and elsewhere - copy-on-write
                                   * pages, and later shared memory           */
    uint32_t mappings;            /* how many pages, for an average size      */
} vibeos_mm_process_usage_t;

/* Fill `out` with the current picture. Walks the frame table, so it is not
 * free: intended for a console command, a panic, or a periodic sample - not for
 * a syscall in a loop. Returns 0 on success. */
int vibeos_mm_usage(vibeos_mm_usage_t *out);

/* Fill up to `max` entries with per-process figures, newest process last.
 * Returns the number written, or negative on error. Requires walking every
 * live address space, so the same caveat applies. */
int vibeos_mm_process_usage(vibeos_mm_process_usage_t *out, uint32_t max);

/* Human-readable names, for the console and for logs. Never null. */
const char *vibeos_frame_state_name(vibeos_frame_state_t state);

#endif /* VIBEOS_MM_MODEL_H */
