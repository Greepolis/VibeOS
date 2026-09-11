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
