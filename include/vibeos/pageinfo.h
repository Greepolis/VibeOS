#ifndef VIBEOS_PAGEINFO_H
#define VIBEOS_PAGEINFO_H

#include <stdint.h>

/* What backs one page of a process's own address space.
 *
 * This exists because a whole class of defect in this kernel produces exactly
 * one symptom from ring 3 - "the bytes are not what I wrote" - and three
 * completely different causes:
 *
 *   the copy never happened   the child still shares the parent's frame, so
 *                             its writes went into the parent's page
 *   the copy was lost         the child got a private frame, wrote to it, and
 *                             then something replaced it with a fresh copy of
 *                             the original
 *   somebody else wrote it    the frame is private and singly owned, and its
 *                             contents changed anyway
 *
 * A program cannot tell those apart, so every report of the third kind has been
 * investigated as though it might be the first. With this, one failing run
 * distinguishes them: the identity says whether the page moved, and the owner
 * count says whether it was ever private.
 *
 * **It reports an identity, not an address.** `frame` is the allocator's index
 * for the frame, which is stable within a boot and comparable between samples -
 * everything a diagnosis needs - and is deliberately not the physical address,
 * because handing ring 3 the physical layout of memory is a disclosure that
 * would outlive the debugging it was added for.
 */

#define VIBEOS_PAGE_PRESENT 0x01u
#define VIBEOS_PAGE_WRITE   0x02u
#define VIBEOS_PAGE_USER    0x04u
#define VIBEOS_PAGE_COW     0x08u
#define VIBEOS_PAGE_OWNED   0x10u   /* this address space holds a reference */

typedef struct vibeos_pageinfo {
    uint64_t frame;    /* allocator index, 0 when nothing is mapped */
    uint32_t flags;
    uint32_t owners;   /* address spaces plus other holders of this frame */
    /* The first eight bytes of the frame, read by the kernel through its own
     * mapping rather than through the caller's.
     *
     * The three causes above are all about *which frame*. This answers a
     * different question that none of them can: whether the caller and the
     * kernel, looking at the same frame by two different translations, see the
     * same bytes. A process reporting "the bytes are not what I wrote" from a
     * page that the page tables and the ownership count both say is its own
     * has only two remaining explanations - the store never reached the frame,
     * or the read did not come from it - and they are told apart by asking
     * somebody else to look.
     *
     * No disclosure: these are bytes of a page the caller already maps and can
     * read for itself. */
    uint64_t first_word;
} vibeos_pageinfo_t;

#endif /* VIBEOS_PAGEINFO_H */
