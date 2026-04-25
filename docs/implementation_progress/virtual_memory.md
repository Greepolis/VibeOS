# Virtual Memory Progress

Status: In Progress
Last review: 2026-04-25

## Implemented
- Address-space model and map/unmap/protect primitives in `kernel/mm/vm.c`.
- Lookup/validation helpers and overlap checks.
- Structural compaction helpers for adjacent compatible mappings.
- Read-only clone primitive for early copy-on-write modeling.

## Pending
- Hardware page-table backend wiring.
- Real fault path for copy-on-write and protection traps.
- TLB shootdown strategy for SMP correctness.

## Next checkpoint
- Connect VM mappings to architecture page-table implementation and add fault-path tests.
