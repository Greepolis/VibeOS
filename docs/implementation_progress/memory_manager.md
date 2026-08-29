# Memory Manager Progress

Status: In Progress (runtime PMM allocation and atomic frame refcounting verified)
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

## Pending
- A gate on the copy-on-write counters, so a fork that silently went back to eager copying would fail the boot rather than be noticed by someone reading the log.
- Buddy or segregated allocator for long-lived runtime.
- Zone policy (DMA/normal/highmem-like partitions where applicable).
- Allocation diagnostics for fragmentation and pressure trends.
- Ownership for frames shared by anything other than fork: file-backed mappings and DMA are not refcounted, because neither exists yet.
- A refcount wider than a byte, so a heavily shared frame is reclaimed rather than deliberately leaked at 255 owners.

## Next checkpoint
- Add refcount diagnostics and memory-pressure stress coverage before replacing the current allocator backend.
