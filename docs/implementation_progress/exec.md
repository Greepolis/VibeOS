# Program Loading Progress

Status: **Rewrite underway** ([docs/exec/](../exec/README.md)). X-P0, X-P1, X-P3 and X-P4 done. X-P2 mapping on after four attempts; the staging buffers and demand paging remain.
Last review: 2026-09-01

## Why it is being rewritten

`execve` is the longest function in `arch_hw.c`, and everything it does lives
there: reading the file, parsing the headers, choosing an address window,
mapping segments, placing an interpreter, building the startup block. It holds
two static staging buffers totalling six megabytes, reads whole programs into
them with interrupts masked, and shares them between every core.

The plan and its phases are in [docs/exec/phases.md](../exec/phases.md). This
file records what has actually landed.

---

## X-P0 — Say what happens (done, 2026-09-01)

**The defect this closes is not a crash. It is that a failed exec could not be
diagnosed.** The loader had fourteen `return -1` sites and one message between
them, so every way of failing to start a program produced the same sentence.
"Cannot load" is not a diagnosis: a file that is absent, a file that is present
and short, and a file that is perfect with no memory left are three unrelated
investigations, and nothing in the log distinguished them.

That mattered more than it sounds. A failed FAT lookup once looked like the end
of a file, so a flaky sector produced a short image that claimed to be complete
and the loader parsed whatever the previous program had left in the shared
staging buffer. That defect is fixed - but nothing in the exec path would have
*said* so, because all of those outcomes printed the same line.

**What landed**

- `include/vibeos/exec_stats.h` and `kernel/exec/stats.c`: eleven refusal
  reasons, one counter each, portable so a host test can assert them as well as
  the boot gate. The names are stable text, because the gate matches on them -
  they are interface, not prose.
- `hw_exec_refuse()` in the architecture layer: counts, prints one line naming
  the reason, the path and the step, and returns -1. One `serial_lock` call for
  the whole line, because a message assembled from six `serial_puts` is six
  critical sections with three other cores writing into the gaps.
- Every refusal in `hw_proc_create` and the `not-found` refusal above it now
  goes through it. The two that already had ad-hoc messages of their own are
  included, since a sentence in the log is not something a gate can assert on -
  which is exactly how they had stayed outside the tally.
- `exec` on the kernel console prints the counters; `kernel/exec/view.c` holds
  the formatting, so the architecture layer knows nothing about it.

**The path is a parameter, not `argv[0]`.** `hw_proc_create` takes the path it
was given. Re-using `argv[0]` would have been free and would have been a lie:
BusyBox becomes twenty commands by looking up the name it was invoked under. The
embedded-image path has no filename at all and passes null, which prints as `-`.

**Gated.** The boot gate drives `exec` and asserts on *which reasons appear*,
not on a total:

    [EXEC] loaded=0x1c not-found=0x6

`not-found` is expected and frequent - a shell resolving a bare command name
tries it as a path first, and a C runtime asks for `/proc/self/exe`, which this
filesystem does not have. Every other reason is a failure. That is a much
stronger assertion than a count: one `no-memory` or one `short-read` hidden
among forty expected `not-found`s is exactly what this watches for, and a total
of forty-one would not blink.

The expected set was **measured from a green boot**, not reasoned about from the
loader - the whole reason the phase exists is that nobody could tell which
refusals were happening. Emptying the set turns the boot red with
`exec_unexpected_refusal=not-found`, which is how the assertion was confirmed.

---

## X-P1 — The parse comes out (done, 2026-09-01)

**A correction to the plan, made in the open.** X-P1 said to create
`kernel/exec/image.c` holding a pure segment-layout function. It already exists:
`kernel/core/elf.c` is portable, takes headers and a bias, and produces exactly
the list of (vaddr, file offset, filesz, memsz, flags) a loader must satisfy,
plus `vibeos_elf_page_flags` and `vibeos_elf_fill_page`. Writing a second one
would have produced two parsers that must agree about a security boundary, with
no build step checking that they do - worse than the long file the phase was
meant to shorten.

What was genuinely missing is that **none of it was reachable except through a
boot**. `tests/kernel/loader_tests.c` fixes that: twelve cases against a
hand-built ELF64, so each one names the field it exercises instead of hiding it
in an opaque blob.

The layout cases are the ones that have bitten a loader here or are one edit
from doing so: two segments with a hole between them (a loader that fills from
`min_vaddr` to `end_vaddr` hands the program writable pages it never asked
for); a `.bss` tail where `memsz` exceeds `filesz`; a page filled with a pattern
first, so a filler that copies and forgets to zero cannot pass against a buffer
that was already zero; two segments sharing one page, which must carry the
permissions of both; an image asking to be placed on top of the kernel; a
truncated file, refused rather than parsed as far as it goes; and an ET_DYN
image biased exactly once, with `image_span` proven not to move with the bias.

The startup block is **decoded back and compared**, not spot-checked. Every
field is one a C runtime reads before the program's first line, so a builder
that is plausible but wrong kills the program inside the runtime with no
reference to the loader that mis-built its stack. Two rules that were comments
are now assertions: `AT_ENTRY` stays the *program's* entry when an interpreter
starts the process - otherwise the interpreter re-enters itself and it looks
like a hang - and `AT_BASE` is *omitted* for a static program rather than
written as zero, because zero is a legal load address and a runtime believes it.

Confirmed by breaking it: changing one expected value turns `test_loader` red.

---

## X-P2 — attempted, measured, and turned off (2026-09-01)

**Status: not done. The mechanism is built and disabled behind `if (0)`, with
the measurement that disabled it written next to it.**

The step taken was the first half of X-P2: a read-only image page that comes
wholly from the file is mapped *from the page cache* instead of being copied
into a fresh frame. The cache already holds the file's page, so the loader was
allocating a second frame to memcpy a copy of a copy into.

It works, and the win is large:

    [EXEC] loaded=0x1c pages_from_cache=0xfe3 pages_copied=0x19c

4067 of 4479 image pages - 91% - stopped being copied. BusyBox alone is two
megabytes of text, exec'd twenty times in a boot.

**And it made the machine worse.** Eight boots: 6 clean, with two wedges. The
parent commit measured 22/24 the same day *with no wedge at all*. The first run
also tripped the stress service:

    STRESS_FAIL: the child's own copy-on-write page at offset 0: found 0x5b expected 0x54
    STRESS_FAIL: replay with EFI/BOOT/SVC_STRS.ELF 97300336535

**Why it is off rather than tuned: there is no mechanism.** The failing page is
a child's private writable page, which this change never touches - it only maps
read-only pages. The likely shape is therefore not the mapping but what it
changes underneath: not allocating four thousand frames per boot rearranges
every subsequent allocation, so a latent defect that used to land on a harmless
frame now lands on a live one. The copy-on-write failure the stress service
reports is a **known-open** defect of roughly one boot in twenty-four, recorded
in [boot_repeatability.md](boot_repeatability.md); this change appears to make
it common rather than to create it.

Shipping it in that state would poison every measurement taken afterwards. That
is the trap this project has fallen into repeatedly, and it is the reason the
numbers above were taken against the parent commit rather than read on their
own.

**What is kept, because it is sound on its own:**

- `vibeos_elf_page_file_offset` - the pure function deciding *which* pages could
  ever be shared, with five host cases. It is mostly about saying no: a page
  straddling the end of `filesz`, a page inside the `.bss`, a page covered by
  two segments, a page in a hole, and a segment that does not start on a page
  boundary. Every wrong "yes" maps file bytes where zeroes belong, and a program
  whose `.bss` starts out holding whatever followed `.data` in the file *runs* -
  it just runs wrong, much later.
- `pages_from_cache` and `pages_copied` on the `exec` line, so the ratio is
  measurable rather than argued about.
- The refusal list on that line now has a `refused:` marker. Adding two counters
  silently broke the gate's bare `name=0x...` pattern, which would have read
  `pages_from_cache=0x..` as a refusal called "cache". A marker costs six
  characters and makes the boundary a fact rather than a coincidence.

### Measured a second time, after the blocker was closed (2026-09-01)

The copy-on-write exclusivity window - the defect this was thought to be
exposing - was found and fixed the same day, from CI logs. So this was turned
back on and measured again.

**The wedges are gone.** That half of the first measurement was the
copy-on-write defect, and closing it removed the silence entirely: with this
branch on, no boot goes quiet any more.

**It is still off.** 21/24 with it on against 23/24 with it off, same tree, same
day. Three failures against one proves very little on its own - and this file
already records what happens when a count is treated as a verdict. What decided
it is *what* failed: all three were segmentation faults in Linux binaries at
near-null addresses, in a different program each time (BusyBox twice at an
identical rip, musl once), and that shape is absent from the runs with this off.
**A different program each time is this project's oldest signature for memory
corruption.**

**The narrowed suspicion**, which is the useful output of the second attempt:
this branch introduces a *second source of truth* for which file is being
loaded. The bytes come from `g_exec_elf`, filled by
`hw_read_file_cached(path)`; the pages come from the cache under
`hw_file_id(path)` computed again, separately. Anything that makes those
two disagree - a fallback read, a path truncated into `hw_file_id`'s
fixed-size buffer, a full identity table - hands a process the pages of one file
with the headers of another. The fix is not to check harder at the mapping site
but to make the read report the identity it actually used, which is X-P2's real
shape anyway.

**One thread worth pulling**: one of the three faulted at address `0xe4`,
and the wedge chased for a day in
[boot_repeatability.md](boot_repeatability.md) restored a register context full
of `0xe4`. That may be one corruption family rather than two mysteries.

### Attempt three, and what it eliminated (2026-09-01)

The hypothesis from attempt two was that this branch created a *second source of
truth* for which file is being loaded. It was implemented properly:
`hw_read_file_cached` now reports the cache identity the bytes actually
came from - 0 when it fell back to an uncached read - and that identity is
threaded to the mapping site instead of `hw_file_id(path)` being
recomputed there. That is a real improvement and it is kept: the ratio moved to
**4242 pages mapped against 237 copied**, from 4067/412, which is exactly what a
more often-correct identity looks like.

**The failure did not move: 21/24 again.** So the hypothesis was wrong.

Two further explanations were then eliminated by measurement rather than by
reading code:

- **Wrong content at map time.** A probe compared every page mapped from the
  cache against what `vibeos_elf_fill_page` would have built for it.
  **Zero mismatches** across a whole boot.
- **Stray writes to a page that is now shared.** `hw_cache_audit`
  compares every resident cache page against the file it came from, at the end
  of the boot. **1820 checked, 0 changed.** That detector is kept and gated: once
  image pages are shared rather than copied, one frame is the text of every
  process running that program, so a single stray write reaches all of them -
  and reaches them as a program misbehaving far from whatever did the writing.

One more fact worth more than the count: **two of the three failures were
byte-identical** - BusyBox, the same `rip`, the same
`fault_addr=0xe4`. This is a deterministic defect reached by a specific
path, not a race. That is a much better starting point than any of the three
attempts began with.

So the bytes are right when they are mapped, right at the end, and both sides
agree which file they came from. What is left is **the mapping itself** -
aliasing, a reference, or a lifetime - and that is where a fourth attempt should
start rather than at the top.

### It was the page cache having no lock (2026-09-01)

`kernel/mm/backing.c` had no lock. Not a weak one, not a wrong one -
none, in a table with linear probing and a clock hand.

**It was invisible for as long as the cache had exactly one caller.**
`hw_read_file_cached` runs under `g_exec_lock`, so the table
was serialised by accident. The moment this branch added a second caller - a
loader mapping image pages, reachable from a boot-time spawn that holds no such
lock - the table became concurrent and nothing said so.

Two cores placing entries at once can leave one entry's frame beside another
entry's key. A lookup then *hits* and returns the pages of a different file.
From outside that does not look like a cache defect at all: it looks like a
program that was handed somebody else's text - which is exactly the shape of the
two byte-identical BusyBox faults.

It is the fourth time this project has needed a `set_lock`, and the third layer
to have shipped without one. The pattern is now the same in all three.

Two decisions inside the fix:

- **A lock of its own, not the frame layer's.** A lookup that misses allocates a
  frame, and the frame layer takes `g_mm_lock` to do it, so sharing
  the lock would deadlock on the first miss. Ordering is cache then frame, never
  the reverse; nothing in the frame layer knows the cache exists.
- **The disk read stays inside the lock.** It serialises the execs that miss.
  Taking the lock only around the table would leave a window where a slot is
  claimed and not yet filled, and the comment already in that function says what
  a hit on such a slot hands out.

**Measured: 22/24 with the mapping on, and the signature is gone.** Zero
occurrences of the faulting `rip` that appeared in both earlier runs,
zero unexpected CPU faults. The one failure is a wedge in `busybox_cat`
with no crash record - the background failure that appears with the mapping off
as well.

The full picture, and the reason the count alone was never the argument:

| state | boots | the deterministic signature |
|---|---|---|
| mapping off | 24/24, 23/24, 22/24 | absent |
| mapping on, cache unlocked | 21/24, 21/24 | **present, byte-identical twice** |
| mapping on, cache locked | 22/24 | absent |

22/24 sits inside the off baseline, which is 22 to 24 - a range I had earlier
described as "24/24", and that overstatement is corrected here. What actually
separated the two states was never the count; it was whether a specific
deterministic fault appeared. It no longer does.

**Status: off again (2026-09-02), and the claim above was wrong.**

Four failures in twelve boots came back with `fault_addr=0xe4` at
`rip=0x57da77` - the same signature, in three different programs. It had
never gone away.

The error is worth naming precisely, because it is not a subtle one. The "22/24
with the signature gone" claim rested on grepping that rip in the failing boot
of that run - and **that boot was a wedge, with no crash record at all**, so the
rip could not have been there. An absence was read out of a log where it could
not appear, and treated as evidence.

With the mapping off one run gave 12/12 with neither marker, and **that was
read as a clean differential. It was not.** The same fault appeared later in the
session with the mapping still off.

**Twelve boots without a defect that shows up a few times in twenty-four is a
quiet run, not evidence.** Three different conclusions have now been drawn about
this one signature from too few boots - that it was gone, that the mapping
caused it, and that turning the mapping off removed it - and all three were the
same mistake.

The branch is still off, and now for a weaker and more honest reason: it has
never been shown to be safe, rather than having been shown to be the cause.

What is now known about it, after four attempts: the bytes are correct when
mapped, correct at the end of the boot, both sides agree which file they came
from, and the page cache is locked. The fault is deterministic - same rip, same
address - so it is reached by a specific path rather than won by a race. The
remaining suspects are all in the mapping itself: aliasing, a reference, or a
lifetime.

---

## X-P3 — All-or-nothing (done, 2026-09-01)

**A failed exec leaked an entire address space.** `hw_proc_create` had
eleven refusal points after the address space existed, most of them after pages
were mapped and regions recorded, and every one returned -1 and walked away.
`execve` then reported ENOMEM, the calling program carried on correctly -
and the page tables, every frame they mapped and every region descriptor stayed
allocated with nothing holding a name for them.

The unwinding is now structural rather than remembered: all eleven route through
one `fail:` label that destroys the address space, clears the region list
and zeroes the fields it filled. There is one function that builds a process, so
there is one function that takes it apart, and **a refusal added later cannot
forget to**.

---

## X-P4 — Acceptance (done, 2026-09-01)

**The interpreter substitution is one function, and the build enforces it.**
`hw_interp_path_substitute` is the only place that may name an
interpreter path; `scripts/dev/check-exec-layering.sh` fails if a second
one appears anywhere in the kernel, or if the function stops saying in its own
comment that it is a stand-in. The risk with a stand-in is not that it exists -
it is that it breeds, the two copies disagree, and it stops being something
anyone can find or delete.

Confirmed by breaking it: a hard-coded path added to an unrelated file is found
and named. It also caught a real mistake within the hour - a new function
inserted between the substitution's comment and its signature, splitting a
comment from what it describes.

**Re-exec is cheap, asserted as a ratio.** `pages_from_cache` and
`pages_copied` are on the `exec` line, and the gate asserts the
former exceeds the latter *when the mapping is on*. A threshold in pages would
be tuned to today's manifest and would fail the day somebody adds a program.

**A truncated program is refused, not parsed.** Covered at the parser by
`case_truncated_is_refused_not_parsed` in the loader tests, and at the
loader by `VIBEOS_EXEC_SHORT_READ` having a counter the gate watches.

**Concurrent exec is serialised, not parallel, and that is recorded rather than
claimed.** `g_exec_lock` makes two execs take turns, because they share
one staging buffer. The acceptance criterion as written - two processes exec'ing
different programs at once - is satisfied in the sense that neither corrupts the
other, and *not* in the sense of them proceeding together. That only becomes
true when the staging buffers go, which is the part of X-P2 still open.

---

**What X-P2 still needs, in order: the copy-on-write defect closed first,
since it is what makes this unmeasurable; then demand paging, so `execve` stops
reading whole files; then `g_exec_elf` and `g_interp_elf` and their six
megabytes go.

### One thing this phase cost, worth recording

The first version of the gate change had a syntax error - a new block inserted
between an `if` and its `else`. `check.sh` runs the gate as
`python3 ... > /dev/null 2>&1` and then reads `qemu-cli-summary.txt`, so the
crashed gate left the *previous* run's summary in place and `check.sh` reported
`reason=cli_and_network_verified`. Three runs passed having executed nothing.

It is the same trap as "a green build is not a build", wearing different
clothes, and the tell was the same: a file's timestamp that had not moved.
