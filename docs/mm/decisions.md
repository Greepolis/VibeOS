# Memory Management: Decisions, Risks and Open Questions

*Part of the [memory management rewrite plan](README.md): [architecture](architecture.md) · [observability](observability.md) · [maintainability](maintainability.md) · [phases](phases.md) · [decisions and risks](decisions.md).*
| Risk | Mitigation |
| --- | --- |
| A rewrite mid-flight breaks a working system | Phases are shippable and ordered so P2 - the repair - lands before any new capability |
| `arch_hw.c` is 7800 lines and the memory code is threaded through it | P1 and P2 extract rather than rewrite in place; the wrappers keep call sites compiling while they are moved |
| The low window's identity split is subtle and easy to get wrong twice | It is the first thing `PTE_OWNED` removes the guesswork from, and it gets its own sabotage case |
| The frame table grows from 107 KiB to 1.7 MiB | 0.4% of RAM, and it is what makes L3 and L4 possible at all |
| Scope creep into an allocator rewrite | Explicit non-goal in §3 |



1. **Where does the portable kernel fit?** `kernel/core/` has a memory model
   that is host-tested and initialised after userland has already run, so it has
   no runtime role. L0 and L2 are exactly the sort of thing it should own. This
   plan puts them in `kernel/mm/` for now; whether that is `kernel/core/` is a
   decision for P1.
2. **Does the TLB shootdown design survive contact with L1?** The known problem
   is that a synchronous barrier cannot be met while `syscall` masks interrupts
   for its whole duration. L1 concentrates the question in one place; it does
   not answer it. The honest fix is likely to stop masking interrupts for the
   whole syscall, which is a scheduler change, not a memory one.



These are the points where I would otherwise choose on your behalf. I will stop
and ask at each one rather than pick.

| # | Decision | Why it is not mine | When it is needed |
| --- | --- | --- | --- |
| D1 | ~~Where does the new code live?~~ | **Decided 2026-08-30: `kernel/mm/`**, linked into both the kernel image and the host test binary. See below. | — |
| D2 | ~~How large is the frame descriptor?~~ | **Decided 2026-08-30: 16 bytes**, the complete form. See below. | — |
| D3 | Bit 9 for `PTE_OWNED`, or a side table? | Bit 9 is free today; a side table costs memory but survives a future use of the bit | Before P2 |
| D4 | Does P2 land as one commit or seven? | Seven is safer to bisect, one is easier to review | Before P2 |
| D5 | Is the boot allowed to get slower, and by how much? | The frame walk and the region lookups are not free | Before P3 |
| D6 | Does the block cache in `kernel/fs/` get merged into the page cache, or kept? | Merging touches the filesystem layer, which is outside this plan's scope | Before P4 |
| D7 | Which device backs swap, and is it configured or discovered? | A product decision about how VibeOS is deployed | Before P5 |
| D8 | What happens when swap is full — kill the allocating process, or fail the allocation? | A policy question with no technically correct answer | Before P5 |

Anything not on this list and not specified above, I will implement as written.
If the plan turns out to be wrong about something, I stop and say so rather than
improvise a different design.

## Decisions taken

### D1 — `kernel/mm/`, decided 2026-08-30

A dedicated module, compiled into the kernel image and into the host test
binary. The name says what it contains and it inherits nothing.

The alternative was `kernel/core/`, which would have given the portable kernel
the runtime job it has never had - and that argument got stronger this week,
because `vibeos_kmain` now runs *before* userland rather than after it. It was
rejected because `kernel/core/` already holds the boot state, the log, the timer
and the interrupt controller; adding memory to it turns a module into a
container.

This does not close the "two kernels" question recorded in the plan. It is
noted there as still open, deliberately, rather than being quietly answered by
where a file was put.

### D2 — 16 bytes per frame, decided 2026-08-30

The complete descriptor: `owners`, `state`, `flags`, `backing`, `lru_next`,
`lru_prev`. 1.7 MiB against the 440 MiB this kernel sees, or 0.4%.

The smaller forms - 8 bytes without the reclaim lists, 4 bytes with only what
the repair needs - were rejected for the same reason: each buys back well under
a megabyte and costs a second migration of the structure and its tests, at P4 or
P6, in a subsystem whose whole problem has been changing the same thing four
times. The point of this rewrite is not to do that again.


## D9 — the ownership contract of an allocation

**Decided A, 2026-08-30.** Landed in `29a6e29`. What follows is the question
as it was put, kept because the reasoning is the record of why the kernel
counts the way it does.

It surfaced while writing the wrappers, and it is not a detail: today
`hw_alloc_page` hands back a frame with a reference count of **zero**, and the
count only becomes one when `hw_map_page` maps it. The new layer hands back a
frame with **one** owner, because "allocated and owned by nobody" is exactly the
state that made the old scheme unsafe - it is indistinguishable from free.

Both are coherent; they are not compatible, and the mismatch is silent in the
worst way. Wire the wrappers without settling it and every user page ends up
with two owners instead of one (allocation plus first mapping), so nothing is
ever reclaimed - a leak. Settle it the other way carelessly and a kernel page
table, which is allocated and freed without ever being mapped, is freed at a
count that was never incremented - a double free.

### Option A — an allocation gives the caller one owner (Linux's model)

`alloc` returns owners = 1. The caller holds that reference. Mapping the frame
takes another; unmapping drops it. A caller that allocates a frame purely to map
it drops its own reference once the mapping exists, exactly as
`alloc_page`/`put_page` do.

- Correct for kernel page tables with no change: allocate, use, `put`, freed.
- Every path that allocates a frame *for a mapping* gains one `put`. There are
  about nine such sites.
- A missed `put` leaks one frame and shows up in `meminfo`. A leak is the
  failure direction this whole rewrite chose on purpose.

### Option B — an allocation gives the caller nothing, mapping owns the frame

`alloc` returns owners = 0, as today. Only mappings count.

- No call site changes at all.
- Keeps the state that has caused every defect in this subsystem: a frame that
  is allocated and owned by nobody reads as free to anything that asks, which is
  precisely what the invariant "a frame is freed only at zero" was written to
  make impossible.
- Kernel page tables live permanently at zero owners, so the count cannot be
  used to check anything about them.

**My recommendation is A**, and it is not close. B is cheaper this week and
re-creates the exact ambiguity that cost four fixes; A costs nine `put` calls
and makes every wrong one a measurable leak rather than a corruption that
surfaces in an unrelated program three boots later.

What I need from you is a yes to A, or a reason for B. Until then P1 stops at
step 2, which is where it is: the layer is built, host-tested and compiled in,
and nothing calls it yet.

### D9 as implemented

An allocation returns a frame with one owner, and that owner is the caller.

- A caller that allocates **in order to map** - process images, stacks, `mmap`,
  `brk`, the copy-on-write copy - hands the frame to the address space and
  releases its own reference immediately after the mapping succeeds. Nine sites,
  each carrying a comment naming this decision.
- A caller that allocates memory **for the kernel itself** - page tables, the
  PML4, kernel stacks - never maps it, so its one reference is the only one and
  `hw_free_page` releases it.
- Releasing the last reference and reclaiming the frame are now one operation.
  There is no longer any moment in which a frame has no owners and is not yet
  free, which is the state every defect in this subsystem has lived in.

Three consequences worth writing down, because each was found by doing it
rather than by planning it:

1. **Contiguous allocation had to exist before any of this could be wired in.**
   The bump allocator underneath served the exec staging buffers and the
   two-page kernel stacks, and it has to stop serving anything at all: while two
   allocators hand out frames from one region, one of them is wrong about what
   is free. `vibeos_frame_alloc_contig` covers those callers.

2. **A second allocator was already live and invisible.** The GUI back buffer -
   several megabytes - was taken from the bump allocator *after* the frame layer
   had adopted the region. The frame table still described those frames as free
   and handed them to a process; the desktop then rendered over its memory. No
   detector fired, because nothing was freed early. The bump allocator is closed
   the moment the layer comes up, so the next such call fails visibly.

3. **`meminfo` was not one picture.** It derived allocated as total minus free,
   which swallowed every reserved frame, and took reserved from somewhere else
   again. One walk of the frame table answers it now and the boot gate fails if
   the states do not sum to the total.
