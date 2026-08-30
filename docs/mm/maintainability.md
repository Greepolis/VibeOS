# Memory Management: Maintainability and Verification

*Part of the [memory management rewrite plan](README.md): [architecture](architecture.md) · [observability](observability.md) · [maintainability](maintainability.md) · [phases](phases.md) · [decisions and risks](decisions.md).*
This has to be code that either of us can come back to in a month and change
safely. That is a design constraint, not a wish, and it is currently failing:
the memory manager lives inside a 7800-line file, next to the scheduler, the
signal code, the syscall table and two device drivers. Nobody reads that file;
people search it. Searching is how the same ownership question got answered
three different ways in three different places.

So the rules below are part of the design, and most of them are checkable by a
script rather than by review.

### Structure

- **One concern per file, none over 800 lines.** `kernel/mm/frame.c`,
  `kernel/mm/vmspace.c`, `kernel/mm/vma.c`, `kernel/mm/backing_*.c`,
  `kernel/mm/reclaim.c`. If a file passes 800 lines it is doing two jobs.
- **A header per layer, stating its contract.** The header is where the
  invariants of §2 live, next to the functions that must uphold them.
- **No layer calls upward.** L0 never mentions an address space; L1 never
  mentions a file or a region; L2 never touches a page table entry.

### Enforced, not intended

A CI check (`scripts/dev/check-mm-layering.sh`) fails the build on:

| Rule | The grep |
| --- | --- |
| Only the frame layer touches the free list | `hw_free_page\|g_free_pages` outside `kernel/mm/frame.c` |
| Only the vmspace layer writes page tables | assignments to `pt[`, `pte`, `pml4[` outside `kernel/mm/vmspace.c` |
| Nobody reinvents ownership | `PTE_USER` or `PTE_PRESENT` used to decide *ownership* outside vmspace |
| No layer calls upward | `vibeos_vma_` in frame.c, `vibeos_backing_` in vmspace.c, and so on |

This is the same discipline as the sabotage cases: a rule nobody can violate by
accident is worth more than a rule everybody agrees with. It is also how the
two mistakes in §1 would have been caught the moment they were typed.

### Readability

- **A reader must be able to answer "who owns this frame?" from one file.**
  That is the single test of whether this rewrite succeeded.
- **Comments say why, and name the failure.** The existing convention in this
  project - a comment that exists because of a specific bug says which bug -
  carries over. Several of them are the only surviving record of an
  afternoon.
- **No function over about 60 lines**, and no nesting past three levels. The
  current teardown is four nested loops with two different release rules; that
  is precisely the shape that hid the defect.
- **Names say the layer**: `vibeos_frame_*`, `vibeos_vmspace_*`,
  `vibeos_vma_*`, `vibeos_backing_*`. A call site should say which layer it is
  in without looking anything up.

### Testable without a machine

L0 and L2 are pure data structures and go in the portable kernel, where
`ctest` already runs. This matters for maintenance more than for correctness:
changing an allocator you can unit-test takes minutes, and changing one you can
only test by booting a virtual machine takes an afternoon and a stress run.



The project's existing culture applies unchanged, and this plan leans on it:

- **Every guard has a sabotage case.** A check nobody has seen go red is a check
  nobody should trust. Each phase lists its cases in `scripts/dev/cases/`.
- **Detectors before features.** The free-page poison and the free-while-mapped
  walk stay, and move into L0 where they belong.
- **Host tests where the code is portable.** L0 and L2 are pure data structures.
  This is the largest single gain: today's memory code can only be tested by
  booting a machine.
- **The stress service grows with each phase** - swap round trips in P5, cache
  pressure in P4 - and keeps printing its seed.

