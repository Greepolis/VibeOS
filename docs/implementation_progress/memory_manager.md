# Memory Manager Progress

Status: In Progress (runtime PMM allocation verified)
Last review: 2026-08-01

## Implemented
- Early physical memory manager initialization in `kernel/mm/pmm.c`.
- Boot memory map interpretation and usable-range selection.
- Initrd overlap avoidance logic during PMM bootstrap.
- Page allocation primitive for early kernel/runtime test path.
- Deterministic multi-page contiguous allocation primitive (`vibeos_pmm_alloc_pages`) plus allocation footprint introspection.
- x86_64 runtime frame allocation now obtains page frames from the PMM for task and address-space bring-up rather than relying solely on static test storage.

## Pending
- Buddy or segregated allocator for long-lived runtime.
- Zone policy (DMA/normal/highmem-like partitions where applicable).
- Allocation diagnostics for fragmentation and pressure trends.
- Formal ownership/refcounting for frames shared by fork, file-backed mappings and DMA.

## Next checkpoint
- Add frame ownership/refcount diagnostics and memory-pressure stress coverage before replacing the current allocator backend.
