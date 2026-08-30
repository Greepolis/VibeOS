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
| D1 | Does the new code live in `kernel/mm/` or in `kernel/core/`? | It settles what the portable kernel is *for*, which is an architecture decision, not a file layout one | Before P1 |
| D2 | 16 bytes per frame (1.7 MiB) or a smaller descriptor without the reclaim fields? | Trades memory now against a second migration later | Before P1 |
| D3 | Bit 9 for `PTE_OWNED`, or a side table? | Bit 9 is free today; a side table costs memory but survives a future use of the bit | Before P2 |
| D4 | Does P2 land as one commit or seven? | Seven is safer to bisect, one is easier to review | Before P2 |
| D5 | Is the boot allowed to get slower, and by how much? | The frame walk and the region lookups are not free | Before P3 |
| D6 | Does the block cache in `kernel/fs/` get merged into the page cache, or kept? | Merging touches the filesystem layer, which is outside this plan's scope | Before P4 |
| D7 | Which device backs swap, and is it configured or discovered? | A product decision about how VibeOS is deployed | Before P5 |
| D8 | What happens when swap is full — kill the allocating process, or fail the allocation? | A policy question with no technically correct answer | Before P5 |

Anything not on this list and not specified above, I will implement as written.
If the plan turns out to be wrong about something, I stop and say so rather than
improvise a different design.

