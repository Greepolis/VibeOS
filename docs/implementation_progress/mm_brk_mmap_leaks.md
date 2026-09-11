# brk and mmap gave memory back to nobody

Two findings from an external review, both verified against the source before
being believed, both on paths a ring-3 program reaches.

## brk shrinking removed the region and stopped there

The shrink branch called `vibeos_vma_remove` and nothing else. The page-table
entries stayed present and the frames stayed allocated, so memory a program had
handed back to the kernel was still mapped and still readable through the pages
it had just released.

The region list and the page tables are the two sources of truth about an
address space, and this left them disagreeing - the same shape as the defect
that let munmap free frames belonging to somebody else.

Removed from the list first, then unmapped: a region must never be described
after it has stopped existing, which is the publish-last rule munmap already
follows.

### A worse consequence, checked and found not to exist

The obvious follow-on was that shrinking and then growing again would map fresh
frames over still-present entries and leak the old ones. It does not:
`vibeos_vmspace_map_raw` releases the previous frame when it overwrites a
present, owned entry, and records the rmap change beside it. Checked before
writing it down, which is the only reason it is not in this file as a finding.

## mmap left pages behind when it ran out of memory

Two leaks, and only one was obvious.

**The obvious one**: `void *page = hw_alloc_user_page(); if (!page || hw_map_page(...) != 0) return -ENOMEM;` - when `hw_map_page` is what failed, `page` is
non-null and nothing gave it back. That frame was gone for the life of the
machine.

**The one that was not**: the pages already mapped stayed mapped, with no region
describing them, because `vibeos_vma_insert` only runs once the whole loop has
succeeded. They were *recoverable*, because `mmap_cur` is not advanced on the
failure path so the next mmap reuses the same base and `map_raw` releases what
it overwrites. "Recoverable by accident, on a path taken when memory has just
run out" is not a design.

The review called both a leak; the second is narrower than that and is written
down as what it is.

### And the same two leaks in the function the review did not look at

`hw_map_user_pages` had them too, and it is what brk grows through. Grepping for
the shape after fixing the reported site is what turned one fix into two.

## What the impact is not

The leftover pages are mapped `PTE_PRESENT` with no `PTE_USER`, and
`hw_user_range_ok` walks the page tables rather than the region list, so ring 3
cannot reach them. This is consumed memory, not a semantic hole - narrower than
the review stated, and stated here as measured rather than as reported.

# Correction, the same day: the fix covered one of two loops

The section above says mmap's leftover pages "are mapped `PTE_PRESENT` with no
`PTE_USER`" and that ring 3 cannot reach them. **That was true of one loop and
false of the one that matters.**

`hw_sys_mmap` has two allocation loops. The rollback committed here went into
the first - the reservation branch, which maps guard pages without `PTE_USER`.
The second is the ordinary anonymous path that every `malloc` reaches, it maps
`PTE_PRESENT | PTE_USER` (plus `PTE_WRITE` when asked), and it still ended in a
bare `return -VIBEOS_ENOMEM` with no rollback at all. On that path the leftover
pages *are* reachable from ring 3, with no region describing them.

It was found while rewriting the same function for C5, not by any check. The
review's M-002 had described the second loop's shape; the fix was matched
against the first place that shape appeared and stopped there.

It matters more after C5 than before. The leftovers used to be recoverable by
accident, because the mapping cursor was not advanced on the failure path and the
next mmap mapped over them. C5 claims the range before anything is mapped, so
nothing would ever map over them again - without a rollback they become a leak.
Both loops unwind now, in the C5 change.

## M-006: a length that wraps when rounded to a page (fixed 2026-09-11)

`hw_sys_mmap` rounded with `pages = (len + 0xFFF) / 4096`. For a length within
a page of 2^64 the sum wraps and `pages` is zero: `hw_mmap_claim` saw no
overflow in `base + 0`, claimed nothing, and the call returned that base as a
success - with nothing mapped, and the same base handed to the next caller. Both
branches, the ordinary one and PROT_NONE, had the arithmetic. It is refused before
the sum now, with ENOMEM, which is what Linux answers when the aligned length is
zero.

The same shape was one page further on in munmap and mprotect, found while
checking every `+ 0xFFF` in the file. Both refused `addr + len < addr`, but the
aligned end adds 0xFFF more, so a range ending in the last page of the address
space wrapped to `end = 0`. mprotect then changed nothing and reported success.
munmap was worse: it handed the region list `end - addr`, which is 2^64 - addr,
so every region above `addr` was removed while its pages stayed mapped - and the
region list is what munmap consults to decide what to release. The bound is now
`len > ~0 - 0xFFF - addr`, covering both sums. brk was checked and is safe: its
address is already capped below the mmap arena.

The test is in the ring-3 ABI self-test (`user/prog/hello.c`), under the message
the gate already asserts on. Red first. Because the four new checks share one
message, the fix was then applied in halves: with only the mmap half the boot
stayed red, which proves the munmap and mprotect checks fail by themselves and
are not riding on the mmap ones. Both halves: `linux abi ok`. `check.sh all`
green, twelve boots: 11 pass, 1 fail - task_illegal_transition (running->running by hw_task_exit) in THREADS' exit_group stage, a scheduler-exit signature this change does not touch and seen for the first time today; recorded with the exit-window evidence as its likeliest cause, H-007's locked exit_group loop not yet ruled out.
