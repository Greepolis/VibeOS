# Memory Management: Rewrite Plan

Status: Proposed
Author of record: written 2026-08-30, after the fourth failed fix of one defect
Supersedes: the frame-refcount arrangement described in
[memory_manager.md](implementation_progress/memory_manager.md)

This plan is split across five documents so that each one covers a
single concern - the same rule it asks of the code:

| Document | Covers |
| --- | --- |
| [architecture.md](architecture.md) | the five layers, their data structures and interfaces |
| [observability.md](observability.md) | every counter and log event, including the ones for subsystems that do not exist yet |
| [maintainability.md](maintainability.md) | the structural rules, the layering check that enforces them, and how each phase is verified |
| [phases.md](phases.md) | P0 to P7, specified to the level where implementing them is mechanical |
| [decisions.md](decisions.md) | the risks, the two questions this plan does not settle, and the decisions reserved to the maintainer |

## 1. Why this is a rewrite and not a fix


One defect - a physical frame released while another address space still maps
it - has been diagnosed four times and fixed three times, and it is still here:

| Attempt | What was wrong | Outcome |
| --- | --- | --- |
| Atomic counters | `(*slot)++` from several cores lost increments | real bug, fixed, defect remained |
| Table coverage | the table's base was the region start, its length the *free* pages | real bug, fixed, defect remained |
| `munmap` | freed frames without consulting the count at all | real bug, fixed, defect remained |
| Ownership redesign | count meant "owners beyond the first"; untracked frames answered "free it" | real bug, fixed, **defect remained** |

The last attempt is the informative one. The count is now internally consistent
- when the detector fires, `owners_after_put=0`, so the arithmetic agrees the
last owner let go - and it is still wrong about the world, because some address
space maps a frame the count never heard about.

That is not an arithmetic problem. **Ownership is inferred, not recorded.**

Today the kernel decides "this PTE represents a frame this address space owns"
by looking at hardware bits that mean something else:

- `PTE_USER` means *ring 3 may touch this*. A `PROT_NONE` guard page is owned
  and has no `PTE_USER`; counting on it missed 211 frames per boot.
- `PTE_PRESENT` means *this translation is valid*. Splitting a 2 MiB identity
  leaf leaves 511 present entries describing kernel memory that nothing owns;
  releasing on it decremented other address spaces' counts.

Both mistakes were made by me, three days apart, for the same reason: the
question "does this address space own this frame?" has no answer stored
anywhere, so every site invents one.

The second, structural reason: **there is no single owner of a mapping.**
Page tables are written directly in `hw_proc_create`, `hw_map_page`,
`hw_map_low_user_page`, `hw_aspace_copy_user`, `hw_aspace_copy_low_user`,
`hw_share_user_leaf`, the copy-on-write fault and `mprotect`; they are torn down
in `hw_aspace_destroy` - with different rules for the two user windows - and in
`munmap`. Adding counters to two of those cannot make the other seven correct.

## 2. The goal: production ready


Production ready is not "it boots". For a memory manager it is a specific list,
and every item on it is something this kernel currently fails:

| Property | What it means here | How it is proved |
| --- | --- | --- |
| **Correct under concurrency** | no frame freed while mapped, no mapping lost, no count wrong | detectors silent across 100 boots, stress service green with printed seeds |
| **Bounded** | no structure grows without limit; leaks are visible, not fatal | `frames_leaked` zero, `frames_free` returns to its starting value after a soak |
| **Survives exhaustion** | running out of memory fails the request, never the machine | an allocator-exhaustion test completes; no panic, no partial mapping left behind |
| **No partial states** | every failure path unwinds what it did | fault injection at each allocation point leaves counts unchanged |
| **Isolating** | no process ever sees another's data, or the kernel's | frames zeroed on allocation, poisoned on free, and the poison verified |
| **Predictable** | no unbounded latency in any path a syscall can reach | no synchronous cross-core wait without a bound and a reported timeout |
| **Observable** | the state can be inspected on a running machine | `meminfo`, the log ring, and gate assertions on the numbers |
| **Documented and tested** | invariants written down; the portable parts tested without booting | host tests for L0 and L2; a sabotage case per guard |

### Invariants

These are the statements the implementation must make true, written here so a
future change can be checked against them rather than against somebody's memory:

1. A frame is freed **only** when its owner count reaches zero.
2. A frame outside the frame table is **never** freed.
3. Every page-table entry describing user memory is written by exactly one
   function, and carries `PTE_OWNED` if and only if that address space holds a
   reference to the frame.
4. Every frame handed out is zeroed; every frame released is poisoned; the
   poison is verified when the frame is handed out again.
5. An allocation failure leaves no mapping, no reference and no counter changed.
6. No path that a syscall can reach waits unboundedly for another core.
7. Every counter in §5 is either asserted by the boot gate or documented as not
   yet meaningful.

Anything that cannot be stated this way is not ready; it is working.

## 3. Design goals


1. **One owner for physical frames.** Nothing outside the frame layer touches
   the free list or the counts.
2. **One writer of page tables.** Every PTE that ever describes user memory is
   written by one function.
3. **Ownership recorded, never inferred.** A dedicated bit in the PTE says "this
   address space owns this frame"; `PTE_USER` and `PTE_PRESENT` go back to
   meaning only what the hardware says they mean.
4. **Regions described, not walked.** `mmap`, `munmap` and `mprotect` operate on
   descriptors of what was asked for, not on a walk of what happens to be
   mapped.
5. **Host-testable.** The frame layer and the region layer are pure data
   structures and belong in the portable kernel, where this project's test
   culture already works. Today the memory code cannot be tested at all except
   by booting.
6. **Observable by construction.** Every layer exports counters and log events
   from its first commit, including the ones for subsystems that do not exist
   yet, so adding a page cache later means filling in a number rather than
   inventing a way to see it.

## 4. Non-goals


- Allocator sophistication. The current bump allocator plus free list is
  adequate; this plan does not introduce buddy allocation, slabs or per-CPU
  caches. It makes room for them and stops there. Note that *predictability* is
  not in this list - an unbounded wait inside a syscall is a correctness problem
  under §2 and has to go.
- Changing the two user windows. The low window at `0x400000` exists because
  Linux binaries link there, and that constraint does not move.
- Touching the portable `kernel/core` boot model, beyond giving it real work to
  do (see §9).

## 12. Tracking


Progress is tracked as a row per phase in
[implementation_progress.md](implementation_progress.md) and in
[memory_manager.md](implementation_progress/memory_manager.md), with the
counters of §5 as the evidence. A phase is not "done" because its code exists;
it is done when its numbers are asserted by the boot gate and its sabotage cases
have been seen to fail.

The area is not described as production ready until P7 passes and every
invariant in §2 is mapped to a test that has been seen to fail. Until then the
status line says what is true: which phases are complete, and which properties
are still claims.
