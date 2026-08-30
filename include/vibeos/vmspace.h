#ifndef VIBEOS_VMSPACE_H
#define VIBEOS_VMSPACE_H

#include <stdint.h>

#include "vibeos/mm_model.h"

/* L1: address spaces. The only code that writes a page-table entry.
 *
 * This layer exists to make one question answerable: *does this address space
 * own this frame?* Today's kernel infers the answer from hardware bits - is the
 * entry present, does it carry PTE_USER, is it inside the window a process can
 * reach - and every one of those is a permission, not an ownership record. The
 * inference has been wrong four times, each time freeing a frame another
 * process was still running from, and each time the fix was a better inference.
 *
 * So ownership stops being inferred. `map` takes a reference on the frame and
 * records that it did, in the entry itself; `unmap` and `destroy` release
 * exactly the entries carrying that mark. Nothing else in the kernel calls
 * vibeos_frame_get or vibeos_frame_put on a user frame.
 *
 * See docs/mm/architecture.md L1 for the reasoning, and docs/mm/phases.md P2
 * for the order this was built in.
 */

/* Bit 11 of a page-table entry. x86-64 leaves bits 9-11 to software, and this is
 * the whole mechanism: an entry this layer installed says so, and an entry that
 * arrived some other way - an identity mapping split to finer granularity, a
 * table shared with the kernel - does not.
 *
 * Bit 11, not bit 9, and the difference nearly shipped. The plan said bit 9
 * because bit 9 was documented as free; it is not - PTE_COW in the x86-64 code
 * is 0x200, which is bit 9. Sharing them makes every mapped page read as
 * copy-on-write, so a write fault on a genuinely read-only page would be
 * resolved by granting the write instead of killing the process. Sixteen clean
 * boots did not show it, because the two bits only disagree on a path that
 * needs a fault on a page a program is not allowed to write.
 *
 * The distinction matters most where it is least obvious. A PROT_NONE guard
 * page has no PTE_USER and is still owned, which is why "not user-reachable"
 * was never a safe test for "not ours". An identity-split entry has PTE_WRITE
 * and is not owned, which is why "writable" was not one either. */
#define VIBEOS_PTE_OWNED (1ull << 11)

typedef struct vibeos_vmspace {
    uint64_t root_phys;     /* what goes in CR3 */
    uint64_t *root;         /* the same table, through the backend's mapping */
} vibeos_vmspace_t;

/* Everything machine-specific, supplied once at boot - and supplied by a host
 * test as a handful of malloc'd pages, which is what lets the walking logic be
 * tested without a virtual machine. The page-table *format* is real x86-64 in
 * both cases; only where the memory comes from differs. */
typedef struct vibeos_vmspace_backend {
    /* A physical address as something this code may read and write. Null means
     * not addressable, and every path treats that as a failure rather than
     * writing somewhere else. */
    void *(*map_phys)(uint64_t phys);

    /* A zeroed frame for a page table, and its release. These go to the frame
     * layer in the kernel; a test can hand out anything it likes. */
    uint64_t (*alloc_table)(void);
    void (*free_table)(uint64_t phys);

    /* Installed in slot 0 of every new address space: the kernel's identity
     * map, shared and never freed. Zero leaves the slot empty, which is what a
     * host test wants. */
    uint64_t kernel_pml4e;

    /* Below this address, the kernel reaches memory by identity mapping, and a
     * user page there has to be carved out of tables shared with every other
     * address space. Zero disables the low window entirely. */
    uint64_t identity_limit;

    /* The shared tables that carving has to un-share first. `shared_pd` returns
     * the shared page directory for a given GiB, or null if that GiB has none.
     * Both may be null when there is no low window. */
    const uint64_t *shared_pdpt;
    const uint64_t *(*shared_pd)(uint32_t gib);

    /* Invalidate one address on this core, and tell the other cores running
     * this address space that a translation narrowed. Either may be null; a
     * host test supplies neither. */
    void (*invlpg)(uint64_t va);
    void (*shootdown)(uint64_t root_phys);
} vibeos_vmspace_backend_t;

int vibeos_vmspace_init(const vibeos_vmspace_backend_t *backend);

/* A fresh address space sharing the kernel's identity map. */
int vibeos_vmspace_create(vibeos_vmspace_t *out);

/* Release everything this address space owns, and the tables it owns privately.
 *
 * "Owns" is the ownership bit and nothing else. The kernel's shared tables are
 * left alone because they were never marked, not because of a test on where
 * they sit or what permissions they carry - which is the distinction that took
 * four attempts to get right. */
int vibeos_vmspace_destroy(vibeos_vmspace_t *as);

/* Map one 4 KiB page, taking a reference on the frame.
 *
 * Chooses between the two windows by address: above `identity_limit` this walks
 * and creates tables normally; below it, the shared identity tables are
 * un-shared and a 2 MiB leaf split into 512 identity entries first, so the
 * kernel's own view of that region is unchanged and only the one entry the
 * process asked for is replaced. That used to be a second public function every
 * caller had to remember to choose. */
int vibeos_vmspace_map(vibeos_vmspace_t *as, uint64_t va, uint64_t pa,
                       vibeos_prot_t prot);

/* The same, with the hardware leaf bits given directly.
 *
 * This exists for one reason: copy-on-write is marked with an
 * available-to-software bit that vibeos_prot_t has no word for, and fork must
 * install a mapping that carries it. Rather than widen the portable protection
 * type with an x86-64 detail, the architecture passes the leaf it wants and
 * this layer adds the ownership bit and takes the reference exactly as the
 * prot-based form does.
 *
 * It is not a way round the layer. The reference and the ownership mark are
 * still written here and nowhere else - which is the whole property being
 * defended. Phase P5 moves the copy-on-write logic itself in, and this can go
 * when it does. */
int vibeos_vmspace_map_raw(vibeos_vmspace_t *as, uint64_t va, uint64_t pa,
                           uint64_t leaf_flags);

/* Change the permissions on a page that is already mapped.
 *
 * Read-modify-write, touching only the bits that describe access. Everything
 * else in the entry - the frame, the ownership mark, the copy-on-write mark -
 * is left exactly as it was, because those record facts that a change of
 * permission does not alter.
 *
 * Write access to a copy-on-write page is refused rather than granted, and
 * that is deliberate: such a page is read-only *because it is shared*, not
 * because the program may not write it. Handing out the write bit would let
 * one process write into a page another one is still reading, which is
 * precisely the corruption fork's sharing exists to prevent. The mapping stays
 * read-only, the write faults, and the fault handler makes the copy and grants
 * the access - so the program sees what it asked for, one fault later.
 *
 * `mark` is set on success to the pending access, so the caller can record
 * that the page will still take a fault. Pass null to ignore it. */
int vibeos_vmspace_protect(vibeos_vmspace_t *as, uint64_t va, vibeos_prot_t prot);

/* Unmap one page, releasing the reference if this address space owned it.
 * Returns 1 if an owned entry was released, 0 if there was nothing mapped or
 * the entry was not ours, and negative on a bad argument. */
int vibeos_vmspace_unmap(vibeos_vmspace_t *as, uint64_t va);

/* Give `dst` a copy-on-write view of everything `src` owns. This is fork.
 *
 * No page is copied. Both sides end up mapping the same frames read-only and
 * marked copy-on-write, and the first write from either side faults and
 * duplicates. That is what makes fork cheap enough to use: a shell forks for
 * every external command and the exec that follows throws the copy away, so
 * eager copying is work guaranteed to be wasted - two megabytes of it per
 * command for a program the size of BusyBox.
 *
 * Which pages get cloned is no longer a question with two answers. It is the
 * entries carrying the ownership mark, in both windows, full stop. The
 * previous code walked the high window taking everything present and the low
 * window taking everything marked PTE_USER, and the second test silently
 * excluded PROT_NONE guard pages - so a forked child of a threaded process got
 * an address space with holes where its thread guards belonged.
 *
 * One shootdown at the end, not one per page: every entry has just had its
 * write permission revoked in the parent, and until the other cores are told,
 * a thread of the same process can still write through a cached entry into a
 * page the child now shares. Once per fork is also the difference between a
 * shootdown and a stall. */
int vibeos_vmspace_clone_cow(vibeos_vmspace_t *dst, vibeos_vmspace_t *src);

/* The entry for an address, or null. Read-only; callers must not write through
 * it - that is the point of this layer. */
uint64_t *vibeos_vmspace_entry(vibeos_vmspace_t *as, uint64_t va);

/* How many entries in this address space carry the ownership bit. Walks. Used
 * by the tests and by the inspection layer, never on a hot path. */
uint64_t vibeos_vmspace_owned_count(vibeos_vmspace_t *as);

/* Translate the layer's protection into a hardware leaf, and back. Exposed
 * because the arch code still builds a few entries during bring-up and they
 * must agree with what this layer produces. */
uint64_t vibeos_vmspace_leaf_flags(vibeos_prot_t prot);

#endif /* VIBEOS_VMSPACE_H */
