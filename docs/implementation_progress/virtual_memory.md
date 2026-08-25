# Virtual Memory Progress

Status: In Progress (hardware paging, per-process isolation and copy-on-write fork verified)
Last review: 2026-08-25

## Implemented
- Address-space model and map/unmap/protect primitives in `kernel/mm/vm.c` (portable model).
- Lookup/validation helpers and overlap checks.
- Structural compaction helpers for adjacent compatible mappings.
- Read-only clone primitive for early copy-on-write modeling.
- Gap finder API (`vibeos_vm_find_gap`) for deterministic first-fit virtual range selection.
- Per-process address spaces now map user images separately while retaining the required kernel mappings; task creation and teardown switch and reclaim those spaces in the x86_64 runtime.
- **Real x86_64 hardware paging on metal** (`kernel/arch/x86_64/arch_hw.c`): kernel builds its own 4-level page tables, identity-maps the first 4 GiB with 2 MiB pages, and loads CR3 with kernel-owned tables — the kernel now runs on page tables it controls instead of the firmware's. Verified in QEMU: `PAGING_OK`, `CR3=0x4012000` (kernel BSS). A non-identity mapping (VA 0x8000000000 -> a backing frame via a full PDPT/PD/PT walk) is written through and confirmed translated (`MAP_TEST_OK`), proving arbitrary VA->PA control, not just an equivalent identity swap.

- **Copy-on-write fork on the real page tables.** `fork` duplicates the parent's
  mappings without duplicating its pages: each writable user leaf is rewritten
  read-only with a copy-on-write bit (`PTE_COW`, one of the entry's
  available-to-software bits) in both address spaces, and the frame's reference
  count goes up. A write then takes a #PF that `hw_handle_cow_fault` resolves -
  by simply taking the write permission back if this process turned out to be
  the only owner left, and otherwise by allocating a fresh page, copying, and
  remapping. Measured at boot: 1221 pages shared, 24 later copied.

  Three details are what separate working from nearly working, and none of them
  is visible from the fault handler alone:
  - the handler must accept a fault raised in ring 0. The kernel writes into
    user memory on a process's behalf - `read()` filling a buffer, a syscall
    storing a result - and with `CR0.WP` set those writes fault on a read-only
    page exactly as ring 3 would. Requiring the user bit refuses precisely the
    faults that happen while serving a syscall, which is a freshly forked shell
    dying without printing anything.
  - `hw_user_range_ok` must count a copy-on-write page as writable. It is
    read-only because it is shared, not because the program may not write it,
    and a syscall that rejects the buffer up front never lets the fault happen.
  - a second `fork` must recognise an already-COW entry. It sees the first
    fork's pages as read-only; filing them under "read-only, so it never needs
    duplicating" drops the mark and makes the page permanently unwritable for
    everyone - a shell that runs two commands and dies on the third.

  A read-only page that was never writable is still shared as-is: marking it
  COW would turn a genuine protection violation into a silent success.
- Both user windows are handled: the high one VibeOS programs link into and the
  low one a Linux `ET_EXEC` image lives in, inside the kernel's identity map.
  A fork that skipped the low window would hand the child an address space with
  no program in it.

## Pending
- Route `kernel/mm/vm.c` map/unmap/protect through the runtime page-table implementation so the portable model becomes the live authority. `vibeos_vm_clone_readonly` remains the portable model's early sketch; the live copy-on-write path is the x86_64 one described above.
- Demand paging. Nothing is faulted in from a file, so `mmap` has no file backing and there is no page cache.
- TLB shootdown strategy for SMP correctness. Copy-on-write currently invalidates only on the CPU taking the fault.

## Next checkpoint
- Unify the portable VM API and runtime page-table backend, then add a gate on the copy-on-write counters so a regression to eager copying fails the boot.
