# P6 steps 2-4 — watermarks, admission and pinning

## What the layer decides, and what it deliberately does not know

`kernel/mm/reclaim.c` is small on purpose. Everything it decides is a policy
question and every fact it needs belongs to somebody else: how much is free is
the frame layer's, which pages are droppable is the cache's. The one thing kept
here is which frames are pinned — because "pinned" is not a fact about a
frame's contents but a statement about who else holds an address for it, and
nothing else is in a position to know.

## Two questions, not one

**When to reclaim.** Below the low mark a reclaim runs; below the minimum an
unprivileged allocation is refused. The reserve is the point: privileged
allocations — a page table the teardown itself needs, the kernel's own
bookkeeping — go through at any level. A minimum that refuses everybody is a
machine that deadlocks at exactly the moment it needed to free something.

Marks are a fraction of what the machine actually has (a 64th and a 256th)
rather than constants, because a number that suits a large machine starves a
small one and this kernel runs on both.

**What to reclaim.** Cheapest first, never anything pinned. A clean page-cache
entry costs nothing to drop because the file still has it. Anything else costs
a write, and there is nowhere to write yet — so this phase evicts the clean
tier and *says so*: `skipped_no_swap` counts what it was not allowed to take,
so "reclaim did nothing" and "reclaim had nothing it could take" are different
numbers rather than the same silence. A gate that could not tell them apart
would be satisfied by a reclaim that had quietly stopped working.

The clean tier is the page cache's own eviction, exposed rather than
reimplemented. It already owns the clock hand; a second copy of that knowledge
here is how two structures come to disagree about the same fact.

## Pinning is a safety property, not a tuning one

An eviction that reaches a page table, a DMA buffer, or a frame a device holds
the address of does not make the machine slow. It corrupts it, asynchronously,
which is the hardest kind of defect this project has. Page tables are pinned
where they are allocated and unpinned before release — the second half matters
as much: a pin that followed a frame onto the free list would give the next
tenant a frame that can never be reclaimed, which is a leak no counter would
report because the frame is perfectly accounted for.

## Verified

Ten host-test groups, ordered so the two properties that *corrupt* when wrong —
pinning, and the reserve admitting privileged work — come first. Five sabotage
cases, each confirmed red and the tree confirmed green again:

- the reserve refuses privileged allocations too (a deadlock)
- the minimum is off by one
- a pin does not take
- the shortfall is not counted
- a transition is counted on every query rather than once

## The watermarks were configured and read by nobody

The first version of this set the marks, wired the clean tier, and then never
consulted either from the allocation path. They were a number nothing read —
this project's most repeated defect wearing new clothes, and the same one S-P6
had just found in the scheduler's quantum.

The pressure service found it in one boot. `hw_alloc_page` is split now: an
unqualified door for the kernel's own work, and `hw_alloc_user_page` for pages
a process asked for, which is what the reserve is held back from. Below the low
mark the allocator tries the clean tier before refusing anybody.

## What the pressure service found next, and why it is not in the boot

`user/prog/svc_press.c` allocates 256 KiB at a time and touches every page. At
**80 blocks — twenty megabytes, on a guest with four hundred** — the machine
stops answering and the serial log fills with binary.

That is not exhaustion and it is not the watermarks refusing anything: twenty
megabytes is five per cent of memory. It is a defect this boot has had all
along, which nothing had ever asked it for enough pages in a row to find. The
region pool is 2048 entries against the 5120 pages that run wants, and
`vibeos_vma_insert` refuses cleanly and counts the refusal, so the pool running
out is handled — but the log is too corrupted by then to read the counter,
which is itself a fact worth recording.

The service is built and shipped and **not started by init**. Starting it would
turn every boot red on a defect that is not reclaim's, and this project has a
rule about gates people learn to ignore. Run it by hand from the shell.

Two things to do next, in this order: find what corrupts the console at that
point — it is the only lead that explains binary in the log rather than a
refusal — and then decide whether the region pool should grow or whether mmap
should record a range as one region instead of one per page.
