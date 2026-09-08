# The store that never landed

Continues `mm_ring3_write_nul.md`. That entry established the signature; this
one removes five explanations and confirms the process migrates between cores.
Every step is a measurement. The mechanism is **not** settled, and the
explanation that fitted best is refuted below by one grep.

## The signature, restated

Three to five boots in ten, a user buffer whose first sixteen bytes read as NUL
while the rest is correct:

```
[HW][SYS] write(ring3): ^@×16=120
```

`say("STRESS_OK rounds=", ...)` builds that in `char line[128]` on the **user
stack**, and `STRESS_OK rounds` is exactly sixteen characters.

## What each measurement removed

| Question | Answer | Instrument |
|---|---|---|
| Is the page moving while the kernel reads it? | No | the same eight bytes read twice in `write`: `first8 == again` |
| Does the buffer cross a page boundary? | No | page offset `0xc60`, 21 bytes — one page |
| Does the copy-on-write copy lose data? | **No** | a fold of the whole page before and after the resolution: `cow_copy_changed=0`, on every boot including corrupted ones |
| Does the page fault only once? | **No** | the fault ring shows svc-stress itself faulting on that page four times |
| Are frames handed out twice? | No | `double_allocs=0`, `backbuf_shared=0`, `backbuf_lost=0` |
| **Does it need more than one core?** | **Yes** | eight boots at `-smp 1`: zero corrupted writes, `STRESS_OK` every time |

The single-core result is the one that closes it. Four cores: three to five
failures in ten. One core: zero in eight.

## What that leaves

The copy is faithful and the data is not in it, so the store never reached the
frame the copy read. The defect needs a second core. The process is
single-threaded and forks about a hundred times before printing.

### The migration is measured

The core identity is recorded on both sides now, and it says outright that the
process moves:

```
write on cpu=1   faults resolved on cpu=0, cpu=0, cpu=1, cpu=0
write on cpu=2   faults resolved on cpu=1, cpu=2, cpu=2, cpu=2, cpu=1
```

svc-stress migrates, and its stack page is resolved on cores other than the one
that later writes to it.

### The explanation that fitted, and why it is wrong

`CLAUDE.md` describes a hole that fits this shape exactly:

> `hw_share_user_leaf` revokes write permission to mark a page copy-on-write and
> then invalidated the calling core's TLB - which is complete only if no other
> core is running that address space.

with the optimisation built on it: *"A single-threaded fork now sends nothing at
all, which is nearly all of them."* "Single-threaded" is true at an instant and
says nothing about which cores have held a translation over time - so a core
that ran the process before the fork could keep a writable entry for a page the
fork has since made read-only.

**That explanation has a false premise and is discarded.** There is no
`PTE_GLOBAL` and no `CR4.PGE` anywhere in this tree, so every CR3 write flushes
the whole TLB. A core the process migrates onto reloads CR3 to get there. A
stale entry cannot survive the trip.

It is written down rather than deleted because it fits every symptom, it is the
first thing the next reader will think of, and it takes one grep to refute.

### So the mechanism is open

What is established: the store is lost before the syscall, the copy-on-write
copy is faithful, the page faults several times in the same process, the defect
needs more than one core, and the process migrates between them.

What is not: how a store executed on one core fails to be visible to the same
process on another, given that CR3 reloads flush. The remaining candidates - and
this is a list to test, not a conclusion:

- **two cores inside `vibeos_vmspace_fault` on one frame.** The sole-owner fast
  path grants write on the *existing* frame while the copy path reads that same
  frame into a fresh one. The undo window is guarded by a re-read of the owner
  count, and its comment records a real defect fixed there before; whether the
  guard covers a parent and a child faulting simultaneously is not something
  reading settles.
- **the copy racing a writer.** `copy_frame(fresh, phys)` reads `phys` with no
  lock. A core holding write permission on `phys` at that moment produces a
  copy that is faithful to *some* state and not to the one the fold compares -
  the fold reads the page through the mapping after the resolution, so it cannot
  see a source that changed during the copy.
- **the fold itself.** It compares the page before and after through the same
  virtual address. If the defect is that the process ends up reading a
  *different frame* than the one it wrote, the fold would agree and still be
  measuring the wrong thing. This is the first question to settle, because it
  decides whether `cow_copy_changed=0` means what it appears to.

That last point is the one to take first, and it is this project's own rule:
when a detector reports zero, ask whether it could have reported anything else.

## Two things this corrected in the tree

**A comment that was wrong.** Beside the copy-on-write trace:

> What it answered once already: the page that loses a wide store faults exactly
> once, so the store is lost after a successful resolution rather than to a
> second fault nobody handled.

The fault ring shows that page faulting four times in the same process. The
conclusion drawn from "exactly once" was the premise of the previous
investigation.

**A fault nobody had separated.** One boot's ring carries `err=0x3` — present
and write with **no user bit** — at a kernel `rip`. That is the kernel writing
into user memory on the process's behalf with `CR0.WP` set, on the same page,
and it is a different event from the ring-3 stores around it.

## Instruments added, and why each is shaped as it is

- **The double read** in `write`. Two reads of the same eight bytes separate "the
  page is moving under the kernel" from "the data was already gone". It fires
  only on leading NULs followed by real text, because a detector that flags any
  NUL anywhere would catch every program writing a binary byte, and this project
  has a rule about detectors that report healthy behaviour.
- **The fault ring**, sixteen entries, filtered to the corrupted page and printed
  in the same critical section as the write report. The existing trace floods -
  its own comment says it changes the timing of the defect it looks for - so the
  faults are recorded always and printed only by whoever finds a corruption.
  That is the rule about taking the state at the crash rather than going back
  for it.
- **The page fold**, before and after resolution. A fold rather than a byte
  sample, because guessing which offset matters is how a detector ends up
  measuring its own assumption - which happened earlier the same day, when a
  frame-ownership probe asked for `owners != 0` and reported 1000 frames of
  1000 because `frame_take` sets one owner when it hands a frame out.
- **`VIBEOS_SMOKE_CPUS`** on the boot gate. Used as an experiment, not a
  configuration: a one-core boot fails other assertions honestly and still
  prints the counters. A defect that needs two cores in one window and one that
  does not are different defects, and taking the second core away is the
  cheapest way to tell them apart.

## What is next

Settle whether the fold can fail before trusting that it did not, then work down
the candidate list above.

The fix that was written here as "leading" - shoot down every core that has ever
run the address space - has been removed, because it repairs the explanation
that has just been refuted. A fix aimed at a mechanism nobody has demonstrated
is how this project ends up with a mechanism configured and consulted by nobody.

`ring3_write_nul` and `cow_copy_changed` are both reported and both belong in
the boot gate's must-be-zero set once there is a fix, so that a regression is a
number rather than a story. They are not asserted yet: `ring3_write_nul` is
non-zero on three to five boots in ten today, and a check that starts red is a
check nobody reads.
