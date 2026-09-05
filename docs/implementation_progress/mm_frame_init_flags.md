# The use-after-free that was never there

**Status: closed.** `vibeos_frame_init` now initialises the descriptors it is
handed, a host test represents the defect, and the two exec staging windows have
shrunk from six megabytes to a hundred and twenty-eight kilobytes — which is the
memory manager's P4 step 3, held up by this for a phase.

## What it looked like

After I3 of the I/O plan separated a file's length from its staged byte count,
nothing needed the four-megabyte exec staging buffer any more. Shrinking both
windows to 64 KiB ran every program on the media and turned the boot gate red:

```
reason=invariant_failed:mm_poison_hits=3019
```

Every boot. The same number. This project's long-standing intermittent
corruption — a musl program tripping over its own malloc bins, roughly one boot
in sixteen — appeared to have become reproducible on demand.

Three facts made it look like an allocator-layout bug, and they were all true:

* shrinking *either* window alone was clean;
* only both together failed;
* deliberately wasting the six megabytes the two shrinks freed made the poison
  disappear **while the windows stayed small**.

The only thing the shrink changes is which physical memory gets used for what.
So the conclusion drawn — and written into the source beside the constant — was
that something writes to a frame it has already released, and the buffers must
stay large until that was closed.

## What it was

The frame layer's poison detector reports *how often*. It could not say
**which** frame, **what** was there, or **who** released it, so every reading of
this was an argument about mechanism with no evidence about mechanism. Adding
`vibeos_frame_set_poison_watch` — one line per event, capped at eight — answered
it on the first boot that got far enough:

```
[MM] POISON_BROKEN frame=0x1e99000 word=0x0 found=0x0 freed_by=0x0 owners=0x0
[MM] POISON_BROKEN frame=0x1e9a000 word=0x0 found=0x0 freed_by=0x0 owners=0x0
...
```

Three things in that line settle it:

* **`freed_by=0`.** The release writes a tag into word 1 of every frame it
  poisons. Zero means no release ever ran. A frame nobody released cannot be a
  use-after-free.
* **`found=0`.** Not garbage, not a live structure — nothing.
* **`frame=0x1e99000`**, and the frames after it, consecutive. The boot's own
  banner says `base=0x1780000 reserved_prefix=0x719000`; their sum is
  `0x1e99000` exactly. These are the *first* frames the pool ever hands out.

So virgin frames were being judged against a poison that was never written.
`frame_check_poison` guards on `VIBEOS_FRAME_WAS_FREED` precisely to avoid that.
The guard was right; the flag was lying.

`vibeos_frame_init` set every field of a descriptor except `flags`, and
`frame_push_free` preserves that bit on purpose — "the one fact about a frame
that must outlive its contents". On a table full of the bump allocator's
leftovers, `flags &= WAS_FREED` preserves *garbage*: every frame whose stale
byte happened to have bit `0x10` set came up claiming a release that never
happened.

And the staging windows decide where in physical memory that descriptor table
lands. That is the whole of "either alone is clean, both together fail", and the
whole of why wasting the freed six megabytes made it go away.

## Why the existing test could not see it

`tests/kernel/frame_tests.c` already had the right case — "a frame that has
never been freed is not judged by the poison" — and it passed, because its
`setup()` starts with `memset(g_table, 0, ...)`. The kernel does not. The
property was correct and the arrangement made the defect impossible to observe:
CLAUDE.md's "a test can be right about the outcome and wrong about the
mechanism", for the fourth time.

The new case fills the table with `0xFF` instead — `0xFF` rather than a chosen
value, so the test does not encode which bit the layer happens to use — and
asserts three things: no poison hits, no double allocations (a stale owner count
is a frame handed to a second holder), and a correct free count. Removing the
one line of the fix turns it red; putting it back turns it green.

## What is kept

* `vibeos_frame_set_poison_watch` and its printer. The counter says how often;
  this says what. It is the reason this closed after three failed readings.
* `vibeos_frame_dirty_at_init()` — how many descriptors arrived already marked.
  Reported rather than silently cleared, because "the table was clean" and "the
  table was dirty and we coped" are different states.
* The windows at 64 KiB each. Eight boots: 7/8, the one failure being the
  pre-existing `busybox_cat` wedge, which is the same background rate as every
  other measurement here.

## Two things the tools got wrong on the way

**The first report deadlocked the machine it was explaining.** The watch runs
inside the frame layer's lock and called `vibeos_frame_owners`, which takes it
again. The boot stopped mid-line, at `owners=0x`. `vibeos_frame_owners_locked`
is the one a watch may call, and it is now declared in the header saying so.

**`gcc` accepted that call undeclared and `clang` did not** — the warning count
moved from 0 to 1, which CLAUDE.md says to read the way `rc=` is read. It was
read.

**The banners lied about the sizes.** `[HW] exec staging buffer: 4 MiB` was a
string literal, printed while the constant said 64 KiB, and for a moment the
first run looked like the built image had not booted. They print the constant
now.

**And a check had been sitting red, unread.** `check-mm-layering.sh` allows
three calls to the bootstrap allocator; the reverse map's node pool was moved
before `vibeos_frame_init` a phase ago and made it four, and the limit was never
raised with it. It is four now, with a note that a fifth should be a decision
rather than a discovery.
