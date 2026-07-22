# Virtual Memory Progress

Status: In Progress (real hardware paging verified in QEMU)
Last review: 2026-07-22

## Implemented
- Address-space model and map/unmap/protect primitives in `kernel/mm/vm.c` (portable model).
- Lookup/validation helpers and overlap checks.
- Structural compaction helpers for adjacent compatible mappings.
- Read-only clone primitive for early copy-on-write modeling.
- Gap finder API (`vibeos_vm_find_gap`) for deterministic first-fit virtual range selection.
- **Real x86_64 hardware paging on metal** (`kernel/arch/x86_64/arch_hw.c`): kernel builds its own 4-level page tables, identity-maps the first 4 GiB with 2 MiB pages, and loads CR3 with kernel-owned tables — the kernel now runs on page tables it controls instead of the firmware's. Verified in QEMU: `PAGING_OK`, `CR3=0x4012000` (kernel BSS). A non-identity mapping (VA 0x8000000000 -> a backing frame via a full PDPT/PD/PT walk) is written through and confirmed translated (`MAP_TEST_OK`), proving arbitrary VA->PA control, not just an equivalent identity swap.

## Pending
- Route `kernel/mm/vm.c` map/unmap/protect through the real page tables (make the portable model the live backend), including a physical frame allocator for page-table pages (currently static BSS tables during bring-up).
- Real fault path for copy-on-write and protection traps (on-metal #PF already reaches `vibeos_trap_dispatch_ex`).
- TLB shootdown strategy for SMP correctness.

## Next checkpoint
- Back `vibeos_vm_map`/`unmap` with real PTEs allocated from the PMM and drive a demand-paging fault test in QEMU.
