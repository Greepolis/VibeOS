# Memory Manager Progress

Status: In Progress - rewrite phases P0 and P1 of [docs/mm/](../mm/README.md) are done. Physical frames have one owner, in one file; the bootstrap bump allocator is closed; `meminfo` reports a breakdown the boot gate checks adds up. The premature-free family is not closed - P2 is the phase that repairs it.
Last review: 2026-08-30

## Implemented
- Early physical memory manager initialization in `kernel/mm/pmm.c`.
- Boot memory map interpretation and usable-range selection.
- Initrd overlap avoidance logic during PMM bootstrap.
- Page allocation primitive for early kernel/runtime test path.
- Deterministic multi-page contiguous allocation primitive (`vibeos_pmm_alloc_pages`) plus allocation footprint introspection.
- x86_64 runtime frame allocation now obtains page frames from the PMM for task and address-space bring-up rather than relying solely on static test storage.
- **Frame reference counting, which is what makes copy-on-write safe.** One byte per frame over the allocator's region, taken out of that region at boot. Zero means "one owner", so the ordinary unshared case costs no bookkeeping at all and only sharing writes to the table. The count saturates at 255 rather than wrapping: past that a frame is simply never reclaimed, which leaks a page instead of freeing one somebody is still using. Address-space teardown and the copy-on-write fault both drop a reference and free only when they were the last owner.
- **Copy-on-write accounting.** `g_cow_shared` and `g_cow_copied` are printed at the end of the boot self-test as `[MM] COW_STATS shared=... copied=...` - 1221 shared to 24 copied on the boot this was written against. The report exists because a mechanism that is never exercised and a mechanism that is broken look identical from outside. No gate asserts the numbers; what is gated is that forking and exec'ing programs keep working.

### The frame layer (rewrite phase P1)

Everything in the section below describes the scheme this replaced. It is kept
because the reasoning is still the reason the new one is shaped as it is.

`kernel/mm/frame.c` is now the only code that owns a free list or a reference
count, and it is host-tested: allocation, sharing, double release, a release of
something the table does not describe, poison verified on reuse, reservation,
contiguous allocation broken by a reservation, and a never-freed frame that must
*not* be reported as corrupt. Each was confirmed to go red by breaking the code
it protects.

What changed in substance, rather than in location:

- **An allocation owns its frame** (decision D9). A caller that allocates in
  order to map hands the frame to the address space and lets go; a caller that
  allocates for the kernel holds the only reference. Releasing the last
  reference and reclaiming the frame are one operation, so there is no longer a
  moment in which a frame has no owners and is not yet free - the state every
  defect here has lived in.

- **The free list moved out of the pages.** It used to be a pointer in the first
  word of every reclaimed page, so the poison could not cover the whole page and
  the check had to skip the first two words. The list is threaded through the
  frame descriptors now; a freed page is poison end to end.

- **The bootstrap allocator is closed, not merely unused.** It was still serving
  the GUI back buffer after the frame layer had adopted the region - several
  megabytes the frame table described as free and handed to a process, with the
  desktop rendering over its memory. No detector fired: nothing had been freed
  early, the frames were given out twice. That is the failure mode of having two
  allocators, and it is why closing it is a correctness rule.

- **`meminfo` is one picture.** The states partition the total exactly and the
  boot gate fails if they do not. The same walk reports `largest_free_run`, so
  free memory that is too scattered to satisfy a contiguous request is a number
  somebody can read rather than an allocation failure nobody can explain.
  Compaction itself is phase P6.

### Frame ownership, redesigned (the scheme P1 replaced)

The reference count now means what it says: the number of address spaces that
map a frame. One owner is 1, nobody is 0, and a frame is freed at 0. The
previous scheme counted "owners beyond the first", so zero meant one owner and
every path carried a mental offset of one - and an untracked frame answered
"yes, free it", so a missed increment produced silent corruption rather than an
error. That default is reversed: a frame the table cannot describe is never
freed. A leak can be measured; freeing a page somebody is running on cannot.

Counting happens where mappings are made and unmade - `hw_map_page`,
`hw_map_low_user_page`, and the paths that clear a user PTE - rather than being
the caller's responsibility to remember.

Two mistakes made during this change are worth keeping, because both were
caught by the detector rather than by reading:

- Counting on `PTE_USER` missed `PROT_NONE` mappings, which a C library uses to
  reserve a thread stack and only later makes user-accessible with `mprotect`.
  211 frames per boot were reported as having no owners.
- Then releasing on `PTE_PRESENT` in the low window released the 511 identity
  entries left behind when a 2 MiB leaf is split - kernel memory, never counted,
  but pointing at frames inside the allocator, so the release decremented
  *other people's* counts.

**Still open.** `hw_aspace_destroy` occasionally frees a frame that another live
process maps, about three boots in sixteen, and the count agrees it was the last
owner - `owners_after_put=0`. So the count is consistent with itself and wrong
about the world: something maps a frame without the count knowing. No leaf write
outside the two mapping functions accounts for it, which points at shared page
tables rather than shared leaves.

The deeper answer is that there is no single owner of a mapping. Address spaces
are built in `hw_proc_create`, `hw_aspace_copy_user`, `hw_aspace_copy_low_user`,
the copy-on-write fault and `mprotect`, and torn down in two places with
different rules for the two windows. Counting was added to two of those; making
it reliable means giving mappings one owner, not more counters.

## Superseded by a rewrite

The arrangement described above is being replaced rather than repaired. One
defect - a frame released while another address space maps it - has survived
four fixes, because ownership is inferred from hardware bits rather than
recorded. See [ADR-0007](../adrs/ADR-0007-centralised-memory-management.md) and
the plan in [docs/mm/](../mm/README.md).

Work proceeds in phases P0 to P7; P2 is the phase that repairs the defect.
Nothing in this file is being changed in the meantime, so that it remains an
accurate description of what is running.

## Pending
- A gate on the copy-on-write counters, so a fork that silently went back to eager copying would fail the boot rather than be noticed by someone reading the log.
- Buddy or segregated allocator for long-lived runtime.
- Zone policy (DMA/normal/highmem-like partitions where applicable).
- Allocation diagnostics for fragmentation and pressure trends.
- Ownership for frames shared by anything other than fork: file-backed mappings and DMA are not refcounted, because neither exists yet.
- A refcount wider than a byte, so a heavily shared frame is reclaimed rather than deliberately leaked at 255 owners.

## Next checkpoint
- Add refcount diagnostics and memory-pressure stress coverage before replacing the current allocator backend.
