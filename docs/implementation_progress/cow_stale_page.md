# The copy-on-write page that keeps the parent's contents

The oldest open defect here, and the one the memory-manager rewrite was written
for. This file records what is now *known* about it, because four earlier
investigations each ended at a plausible pointer with no mechanism behind it,
and two of them were wrong in ways that cost days.

## The symptom

`svc-stress`, round 5 or 6, reports:

```
STRESS_FAIL: the child's own copy-on-write page at offset 0: found 0x33 expected 0xcc
STRESS_FAIL: verdict=the page was private and changed anyway
```

`0xcc` is `~0x33`, and the program computes `after = ~before`, so the child is
reading back exactly the value the page held *before the fork*.

## What has been established

A reproduction came first, and everything below rests on it rather than on
reading the code.

**It reproduces on demand.** `check.sh all build-clang-Release` fails three
times out of three. Local gcc builds boot 8/8 clean, which is why this went
unreproduced for so long - and it is the second time here that a defect lived
in the configuration nobody ran by hand.

**It is not a race.** Forced to a single core with `-smp 1`, it still fails
three times out of three. That removes every concurrency explanation, including
the TLB-shootdown family that had been the leading theory, and makes the defect
deterministic enough to reason about.

**The copy does happen.** The four page samples say so:

| Sample | frame | owners |
| --- | --- | --- |
| parent, before the fork | 3005 | 1 |
| child, after the fork | 3005 | 2 |
| child, after its write | 3020 | 1 |
| child, at the check | 3020 | 1 |

The frame changes on the child's first write, which is copy-on-write working,
and it does not change again.

**The frame is private and stays private.** `owners == 1` at the check, and the
frame is the same one the write went to.

**The whole page is stale, not one byte.** 4096 bytes of 4096 differ. This is
the measurement that mattered most and it was the last one taken: the check
reports the *first* byte that differs, which is always offset 0, and that was
read for an entire session as "only offset 0 is wrong". It is equally
consistent with every byte being wrong, and every byte is. Those are different
defects - a few stale bytes is a copy racing a store; a wholly stale page is a
write that went somewhere else entirely.

## What has been ruled out, each by a detector rather than by argument

- **A frame freed while still mapped.** The existing free-side watch was turned
  up from one free in sixty-four to *every* free. Silent for the whole boot.
- **A frame handed out while still owned.** A new counter in `frame_take`
  (`double_allocs`, printed in `MM_STATS` and asserted zero by the gate)
  watches the allocation side, which nothing watched before. Zero.
- **A write to a freed page.** `poison_hits` is zero.
- **Global TLB entries surviving a CR3 reload.** There is no global bit
  anywhere in this kernel and CR4.PGE is never set.
- **The two user windows having different mapping rules.**
  `hw_map_low_user_page` is a thin wrapper on `hw_map_page`; they are the same
  code.

## Where the next attempt should start

The child writes 4096 bytes and reads back 4096 bytes of the pre-fork value,
from a frame that the page tables and the ownership count both say is its own,
on a single core, deterministically. Either the stores did not land in frame
3020, or the reads did not come from it.

The next measurement should therefore not be about ownership or lifetime - both
are now instrumented and clean - but about *which frame the CPU actually used*:
have the child write one byte, then ask the kernel to read that page through
the identity map and report what it sees. If the kernel sees `after` and the
child sees `before`, the mapping is right and the child's translation is stale.
If the kernel also sees `before`, the store never reached the frame at all.

Note also that the write loop is compiled by clang at `-O2` and the defect is
clang-only so far; whether that is a vectorised store pattern or simply a
different allocation order has not been established, and should not be assumed
either way.

## The sharpest fact so far: mappers=2, owners=1

A `repeat-boot` run caught the release-side watch firing, and its message is
the most precise statement this defect has produced in five investigations:

```
[MM] FREE_WHILE_MAPPED frame=0x2355000 still mapped by pid=0xb
     during destroy mappers=0x2 owners=0x1 owners_now=0x1
```

Two address spaces map the frame; the ownership count knows about one. That is
not an arithmetic error in the count - it is a **mapping that was installed
without taking a reference**, which is exactly the failure the memory-manager
rewrite was proposed to make impossible.

It is reported during a `destroy`, and specifically a destroy of an address
space that is *not* the current task's - the arch-level short circuit for "the
dying task legitimately still maps what it is freeing" did not apply. So the
path is a task tearing down somebody else's address space: exit being reaped,
or execve discarding an old image.

`vibeos_vmspace_map_raw` takes its reference before publishing the entry, and
`clone_one` goes through it, so neither fork nor an ordinary map is the
uncounted one. The next attempt should look for a page-table entry written
without going through that function.

## Why these counters exist

The condition appears about one boot in sixteen. A message alone therefore
makes every run a lottery in which "nothing reported" and "fixed" are
indistinguishable - which is how three earlier fixes here were believed. Two
counters now make each boot a measurement:

- `double_allocs` - a frame handed out while somebody still owns it. Nothing
  watched the allocation side before, and it is the blind spot that matters:
  a double allocation frees nothing, so every release-side watch stays silent.
- `free_while_mapped` - more mappers than owners, counted rather than only
  printed.

Both are on the `MUSTBEZERO` line and the boot gate fails if either is
non-zero. The assertion was confirmed to go red by making `frame_take` count
unconditionally: `mm_double_allocs=4813`.

The free-side walk now samples one release in eight rather than one in
sixty-four. A counter that only sees one free in sixty-four cannot be compared
between two states, which is the whole point of having it. Eight clean boots
out of eight at the higher rate.

## Asking a second observer, and what it settled

`pageinfo` now returns `first_word`: the first eight bytes of the frame as read
by the *kernel*, through its own mapping rather than through the caller's. The
whole value of the field is that it does not use the translation under
suspicion. Two runs settled two questions that five investigations had not.

**The store never reached the frame.** In the failing round the child wrote
`188` to all 4096 bytes; immediately afterwards the kernel read `239` at byte
zero of the frame the page tables point at - and `239` is the pre-fork value.
So the copy is intact and untouched, and the child's writes are simply not in
it. The reads are fine: child and kernel agree on what the frame holds.

**The page itself works.** After the failure, the child stores one ordinary
byte, `0x5a`, and samples again: child reads `90`, kernel reads `90`, same
frame. The mapping is right, the frame is right, the permissions are right, and
an ordinary store lands immediately.

So nothing is wrong with the page. What is lost is specifically the *write
loop*.

## The write loop, and why this is not "a test bug"

`for (i = 0; i < 4096; i++) mem[i] = after;` is not 4096 byte stores by the
time clang has had it. Making that loop `volatile` - so each byte is a separate
ordinary store - makes the failure vanish: three runs out of three clean, where
the same tree failed three out of three.

What the binaries do contain is `movaps` and `movdqa` - ordinary 16-byte
aligned SSE stores - and **no** non-temporal stores at all, so no missing fence
explains this. Ordinary SSE stores are cache-coherent and atomic with respect
to faults: architecturally there is no reason for one to be lost.

One measurement here was taken wrongly and is recorded so it is not taken
again. Counting wide stores across the whole binary gives gcc 18 and clang 172,
and that was briefly written down as evidence that clang vectorises this loop
and gcc does not. It is not evidence of anything: the same two counts come back
*unchanged* when the loop is made volatile, so they are counting other code
entirely and never described the fill loop. The restore that was supposed to
put the loop back had failed silently first - the backup was in `/tmp`, which
WSL had cleaned, which is a trap this project has already written down once.
The volatile result itself was measured correctly and stands; the disassembly
comparison did not and does not.

**The loop has therefore not been made volatile, and must not be.** A volatile
loop would turn the boot green while leaving the defect exactly where it is,
and this file already records three fixes that were believed on that kind of
evidence. musl's own `memset` uses the same instructions, so if a wide store to
a freshly copy-on-write-resolved page can be lost, it is lost for every program
on this machine and not only for the one that noticed.

## Where the next attempt should start

The question is now narrow and mechanical: what happens when a 16-byte SSE
store, rather than a byte store, takes the copy-on-write fault?

The child's first `movaps` faults on a read-only shared page. The handler
allocates a frame, copies the old one into it, installs the entry and
invalidates. The instruction is then retried by hardware. Something in that
sequence loses the retried store while a byte store survives it, and the
candidates are few enough to check one at a time:

1. Whether the fault is even reported the same way - the error code, and
   whether `hw_handle_cow_fault` accepts it.
2. Whether the retry happens at all, or the handler returns to the following
   instruction. A skipped `rep`-like vectorised block would lose the whole
   fill, which is precisely the symptom.
3. Whether the copy runs after the retried store rather than before it.

Instrument the fault handler to log the faulting `rip` and the fault count for
this page. If the page faults once, the store was lost after a successful
resolution; if it faults twice, the second fault is the interesting one.

## The page faults once, and there is more than one failure here

The fault handler can now log every copy-on-write fault with its faulting
`rip` - built with `-DVIBEOS_COW_FAULT_TRACE=1`, off otherwise, because a line
per fault is hundreds of lines in a boot: it floods the log the gate reads and
it changes the timing of the defect it is looking for. An *unhandled* fault is
rare and always worth a line, so that half is unconditional.

It answered the question it was built for. The page that loses a wide store
faults **once**: `va=0x8004000000 ... handled`, and nothing further. So the
store is lost after a successful resolution, not to a second fault that nobody
handled.

It also showed something that changes how these runs should be read. The
failing round is **not always the same defect**. One run reported:

```
STRESS_FAIL: the child's own copy-on-write page at offset 0:
  found 0x00 expected 0x68 - this is the kernel's free-page poison:
  the page was reclaimed while still mapped here
```

All zeroes, which is what `frame_take` fills a page with - so that page was
properly freed and then legitimately reallocated to somebody else while this
process still mapped it. `double_allocs` is zero, so the allocator was not at
fault; something released the frame without the ownership layer noticing.

That is the *same family* as the `mappers=2, owners=1` event, and a different
defect from the lost wide store. Both hide behind one sentence from ring 3 -
"the bytes are not what I wrote" - which is exactly the confusion `pageinfo`
was added to end, and it is why the four verdicts the stress service prints
matter more than the byte values.

Two defects, then, not one:

1. **A wide store lost after a resolved copy-on-write fault.** One fault, page
   healthy afterwards, an ordinary byte store lands. clang only so far.
2. **A frame released while an address space still maps it.** Reported both as
   `mappers=2 owners=1` at a destroy and as a page of zeroes handed to a live
   process.

Previous investigations treated these as one, which is a large part of why each
of them ended somewhere plausible and wrong.

## The lost wide store is TCG, not the kernel

Two more measurements finished this half.

**The child reads the old value from its own page with no kernel in between.**
Every earlier observation went through `page_sample`, which is a syscall - so
"the child sees the wrong value" had always been seen on the far side of a
kernel entry. Reading `mem[0]` straight after the loop, before any syscall,
returns `before`: the loop wrote 60 to all 4096 bytes and the next instruction
to look reads 195.

**The parent's page is untouched.** 0 bytes of 4096 differ, parent and kernel
agree, so the stores did not go to the shared frame either. They went nowhere.

**And it does not happen on real hardware.** Same binary, same kernel, three
runs each:

| accelerator | outcome |
| --- | --- |
| TCG | 21 STRESS_FAIL lines, 3 runs of 3 |
| KVM | clean, 3 runs of 3 |

The argument that settles it does not rest on that table, because this project
has been misled by a KVM-clean result before - twenty clean runs once validated
a setup that pure TCG broke in two runs out of three, which is why every script
here forces TCG. It rests on where the store happens: **once the fault handler
returns, the faulting instruction is retried by the CPU with no kernel
involvement at all.** The kernel cannot lose a store it does not take part in.
Everything the kernel is responsible for has been shown correct - one fault,
handled, a private singly-owned frame, the right contents copied into it, and
an ordinary byte store landing in it immediately afterwards.

The last piece came free from the disassembly. `svc-stress` has another
operation that fills a fresh `mmap` page with the *same* `movdqu` instructions
and checks it, and that one passes. So:

- wide store, no fault - works
- byte store, copy-on-write fault - works
- wide store, copy-on-write fault - lost

which is a statement about restarting a 16-byte store after a page fault, and
that is the emulator's job.

### What this means for the gate

The boot gate forces TCG, deliberately and for good reasons, so CI will keep
failing here until something is done. The options are not equal and the choice
is not obvious:

- Make the fill volatile. One line, and it is exactly the kind of change this
  file warns about three times: the boot goes green and nothing is understood.
  It is defensible *only* because the kernel has now been shown not to be
  involved, and it should carry a comment saying so.
- Keep it and accept a permanently red gate, which trains people to ignore it.
- Narrow the assertion so this specific operation is reported but not fatal.

This is a maintainer's decision, not the investigation's, and it is left open
here rather than settled quietly.

### What is still open

The *other* defect is untouched by all of this: a frame released while an
address space still maps it, seen as `mappers=2 owners=1` at a destroy and as a
page of zeroes handed to a live process. That one is real, it is the kernel's,
and it is what the memory-manager rewrite exists for.
