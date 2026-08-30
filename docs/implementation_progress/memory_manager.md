# Memory Manager Progress

Status: In Progress (frame ownership redesigned and counted at the mapping; a frame is still occasionally released while another process maps it, and the kernel now says so)
Last review: 2026-08-25

## Implemented
- Early physical memory manager initialization in `kernel/mm/pmm.c`.
- Boot memory map interpretation and usable-range selection.
- Initrd overlap avoidance logic during PMM bootstrap.
- Page allocation primitive for early kernel/runtime test path.
- Deterministic multi-page contiguous allocation primitive (`vibeos_pmm_alloc_pages`) plus allocation footprint introspection.
- x86_64 runtime frame allocation now obtains page frames from the PMM for task and address-space bring-up rather than relying solely on static test storage.
- **Frame reference counting, which is what makes copy-on-write safe.** One byte per frame over the allocator's region, taken out of that region at boot. Zero means "one owner", so the ordinary unshared case costs no bookkeeping at all and only sharing writes to the table. The count saturates at 255 rather than wrapping: past that a frame is simply never reclaimed, which leaks a page instead of freeing one somebody is still using. Address-space teardown and the copy-on-write fault both drop a reference and free only when they were the last owner.
- **Copy-on-write accounting.** `g_cow_shared` and `g_cow_copied` are printed at the end of the boot self-test as `[MM] COW_STATS shared=... copied=...` - 1221 shared to 24 copied on the boot this was written against. The report exists because a mechanism that is never exercised and a mechanism that is broken look identical from outside. No gate asserts the numbers; what is gated is that forking and exec'ing programs keep working.

### Frame ownership, redesigned

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

## Pending
- A gate on the copy-on-write counters, so a fork that silently went back to eager copying would fail the boot rather than be noticed by someone reading the log.
- Buddy or segregated allocator for long-lived runtime.
- Zone policy (DMA/normal/highmem-like partitions where applicable).
- Allocation diagnostics for fragmentation and pressure trends.
- Ownership for frames shared by anything other than fork: file-backed mappings and DMA are not refcounted, because neither exists yet.
- A refcount wider than a byte, so a heavily shared frame is reclaimed rather than deliberately leaked at 255 owners.

## Next checkpoint
- Add refcount diagnostics and memory-pressure stress coverage before replacing the current allocator backend.
