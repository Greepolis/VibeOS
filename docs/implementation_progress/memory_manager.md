# Memory Manager Progress

Status: In Progress
Last review: 2026-04-25

## Implemented
- Early physical memory manager initialization in `kernel/mm/pmm.c`.
- Boot memory map interpretation and usable-range selection.
- Initrd overlap avoidance logic during PMM bootstrap.
- Page allocation primitive for early kernel/runtime test path.

## Pending
- Buddy or segregated allocator for long-lived runtime.
- Zone policy (DMA/normal/highmem-like partitions where applicable).
- Allocation diagnostics for fragmentation and pressure trends.

## Next checkpoint
- Land a deterministic buddy allocator backend with stress-test coverage.
