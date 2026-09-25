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

## 2026-09-24: the defect is still there, and what it is not

Picked up because nothing on a machine has ever reclaimed an anonymous page
(`mm_swapout_protocol.md`) and svc-press is the obvious load to change that.
It cannot yet: started from init, it still stops the machine - now at **112
blocks, 28 MiB**, not 80, and with a clean log rather than binary (the console
has been fixed several times since). 28 MiB is far above the low watermark, so
**reclaim is not running when it happens**; this is not a reclaim defect.

What the evidence says, from QEMU's exception log (`VIBEOS_QEMU_TRACE=<file>`,
added to the gate for this - every exception with the full CPU state, which a
machine whose console lock is held by a dead core cannot print):

- The first bad event is always a ring-0 fault at a garbage address - `0x0`,
  `0x2`, `0x3001`, the padding between two functions, or a stack address - on
  a core that was **inside the timer interrupt, on the timer's IST**.
- The clearest one: a `pop %rbp; ret` on the timer's IST read this tick's own
  interrupt-frame values - the saved RFLAGS (`0x202`) as the frame pointer and
  the interrupted RSP as the return address - and executed seven bytes of the
  interrupted stack before faulting. Another core resumed a context whose rip
  was a user *stack* address with a kernel code selector.

Ruled out by guards that were added and did not fire (the probes are kept as
`scripts/dev/patches/press-hunt-instrumentation.patch`, applied on top of
arch_hw.c and kernel.ld):

1. the timer re-entered on the same core (a per-core depth counter);
2. the timer path running off the bottom of its 16 KiB IST slice (a guard band
   checked on entry and exit of every tick);
3. interrupts left enabled anywhere on the timer path (RFLAGS.IF checked after
   each of its seven steps);
4. a task interrupted while running on the timer's IST (checked when the
   context is saved);
5. a kernel-mode context whose rip is not kernel code, saved or loaded through
   the scheduler (`__kernel_text_start/_end`).

None fired, and nothing printed. That leaves the IST slice being written by
something other than its own core's timer path, or state the timer path reads
being corrupted before it runs - the obvious candidate for both is a frame that
svc-press is handed while the kernel still uses it. `double_allocs` and the
free-page poison would say so, but the machine stops before either is printed;
the next step is to make those report at the moment they fire without taking
the console lock (the poison watch now records under the frame lock and prints
later, which is half of that).

### Found: the kernel was in its own frame pool

The next step was taken - `scripts/dev/wedge_report.py` now reads `g_mm_stats`
and the poison records out of the stopped guest through QEMU's monitor, which
needs no console - and it said nothing was wrong: no poison hit, no double
allocation, 97,116 frames free. What it did give was `frames_allocated=8552`.

With the frame pool's base (0x1780000) and its reserved prefix (1,817 frames),
8,552 allocations reach frame index 10,369. The kernel is linked at 0x4000000,
which is index **10,368**. The machine stopped on the allocation that reached
the kernel's first page, and a zeroed kernel page is the IDT and the timer's
IST - which is every symptom above, and why no guard in the kernel's C could
fire: the CPU faulted on the way *into* the handler.

The loader took the firmware memory map in phase 2 and handed that copy over.
The kernel's segments, boot_info, the map array and INIT.ELF are all allocated
after it, as loader memory the map would have called reserved - but the copy
called all of it free, and the largest free region, which becomes the frame
pool, contained the kernel. Only the key was refreshed before
ExitBootServices, never the regions. Nothing allocated 40 MiB before svc-press,
so no boot ever reached it.

Fixed in two places, deliberately:

- **The loader refreshes the map** as its last step before handoff
  (`vibeos_bootloader_refresh_memory_map`, host-tested; the array is allocated
  with slack for the longer map). Failing to refresh is fatal - handing over a
  map that calls the kernel free is worse than not booting.
- **The kernel reserves its own image** (`__kernel_image_start/_end` from
  kernel.ld) regardless, and prints `[HW] kernel image ... in_free_map=`. The
  gate asserts 0: the reservation is a second line, and a map that needs it is
  a wrong map.

Shown both ways with svc-press started from init: both halves removed stops the
machine at 112 blocks, the original signature; the reservation alone, with the
refresh removed, runs it to 256 blocks and `PRESS_OK` while the gate goes red on
`kernel_image_in_free_map`. The earlier episode at
80 blocks with binary in the log is very probably the same defect with a
different layout - not reproduced, so not claimed.

### What svc-press found next (M-061)

With the machine surviving, svc-press unmaps its 64 MiB in a loop, and the gate
goes red on `userland_frames_lost=2486`. `tlbq_overflow=2478` in the same log:
a burst of munmap fills the TLB quarantine's 512 slots faster than the other
cores flush, and an overflowing frame is leaked on purpose (H-015 - leaking is
safe, recycling is not). That was the known follow-up of H-015, unreachable
until now. Fixed the next day by parking only frames another core could still
reach (`mm_tlb_quarantine.md`, "M-061"); svc-press now runs in every boot and
the gate reads its verdict. Note also that
256 blocks were all granted: 64 MiB of a ~420 MiB pool never reaches the
watermarks, so a load that forces reclaim has to be larger, or the machine
smaller.

Found and fixed on the way, both real and neither this defect:

- **The page cache handed out bare addresses.** `vibeos_cache_get` returned a
  frame with the cache's lock already released, and the loader mapped it and
  the exec reader copied from it; an eviction in between frees the frame, and
  the mapping then takes its reference on somebody else's page.
  `vibeos_cache_get_ref` takes the reference under the lock.
- **The clean tier never reported a frame freed.** It told an eviction from an
  empty slot by looking at the entry, and the eviction empties the entry it
  frees - so a page just given back looked like a free slot, and the loop
  stopped at its first success having counted nothing. `freed_clean=0` in every
  log this project has is that, not an idle tier.
- **The poison watch printed under the frame lock**, taking the console lock
  inside it - the trap CLAUDE.md records for a watch that called a locking
  accessor. It records now and the allocating core prints outside the lock.
