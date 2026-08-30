# ADR-0007: Centralised Memory Management

## Status

Proposed

## Context

One defect - a physical frame released while another address space still maps it
- has been diagnosed four times and fixed three times. It is still present.

The fixes were each real: reference counters made atomic, a table whose length
did not cover the region it indexed, a `munmap` that freed frames without
consulting the count at all, and a redesign of what the count meant. None of
them closed the defect, and the last one is the informative case: the count is
now internally consistent - when the free-while-mapped detector fires,
`owners_after_put=0`, so the arithmetic agrees the last owner let go - and still
wrong about the world.

The reason is not arithmetic. **Ownership is inferred rather than recorded.**
The kernel decides "this address space owns this frame" by reading hardware bits
that mean something else: `PTE_USER` means ring 3 may touch the page, and
`PTE_PRESENT` means the translation is valid. Neither means ownership. Two
mistakes three days apart came from that gap - counting on `PTE_USER` missed
`PROT_NONE` guard pages (211 frames per boot with no owners), and releasing on
`PTE_PRESENT` released the 511 identity entries left by splitting a 2 MiB leaf,
decrementing other address spaces' counts.

Compounding it, there is no single owner of a mapping. Page table entries are
written directly in eight places and torn down in two, with different rules for
the two user windows. Adding counters to two of those cannot make the other
eight correct.

## Decision

Rewrite the memory manager as five layers with one owner each, rather than
continue to patch the current arrangement:

- **L0 frames** - the only code that touches the free list or a reference count.
- **L1 address spaces** - the only code that writes a page table entry.
- **L2 regions** - what a process asked for, so `munmap` stops walking page
  tables and freeing what it finds.
- **L3 backing stores** - anonymous, page cache, swap, behind one interface.
- **L4 policy** - reclaim, LRU, eviction order.

Ownership becomes explicit: a software-available bit, `PTE_OWNED`, set only by
the mapping function and released only by unmap and teardown. `PTE_USER` and
`PTE_PRESENT` go back to meaning only what the hardware says.

The work is phased P0 to P7, each phase shippable and green, with P2 the phase
that repairs the defect and everything after it new capability. The goal is
production ready as defined in the plan: correct under concurrency, bounded,
survives exhaustion, no partial states, isolating, predictable, observable,
documented and tested - with each property mapped to a test that has been seen
to fail.

The full plan is [docs/mm/](../mm/README.md).

## Consequences

**Good.** Ownership is recorded once and cannot be re-derived differently by the
next call site. The frame and region layers become pure data structures and can
be tested without booting a machine, which the current memory code cannot.
Counters for the page cache, swap and reclaim exist from the first phase, so
those subsystems are additions rather than excavations. A layering check in CI
makes the structure enforceable rather than aspirational.

**Costly.** The frame descriptor grows from one byte to sixteen per frame -
1.7 MiB on the 440 MiB this kernel sees, or 0.4%. The memory code has to be
extracted from a 7800-line file while remaining bootable at every commit.

**Risky.** A rewrite of a working subsystem mid-project. Mitigated by ordering:
the repair (P2) lands before any new capability, each phase is revertible, and
the existing detectors - free-page poison, free-while-mapped, the seeded stress
service - stay in place throughout and move into L0 where they belong.

**Deferred.** Allocator sophistication (buddy, slabs, per-CPU caches) is
explicitly out of scope; the plan makes room and stops. Whether the layers live
in `kernel/mm/` or in `kernel/core/` - which settles what the portable kernel is
for - is recorded as a decision for the maintainer, not for the implementer.
