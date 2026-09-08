# The failure that had not happened, and the one underneath it

Going after the argv defect produced a different one, characterised far better,
and showed that the boot gate had been naming the wrong subsystem for it.

## What was being looked for

`scripts/dev/cases/mm-argv-poison.txt`: a process's argv reads
`0x00ffffff00ffffff` — two white pixels, `COL_TITLETXT` from the desktop — with
every memory counter at zero. The case file's stated next question was whether
`hw_alloc_pages_contig` reserves every frame in the run it returns.

## Four hypotheses, each killed by a number

Nothing here was settled by reading. Each measurement was built, run, and in
three cases refuted the thing that motivated it.

| Hypothesis | Instrument | Result |
|---|---|---|
| The desktop's buffer shadows the low user window a Linux program is mapped into | print the buffer range against `VIBEOS_HW_LOW_USER_*` | `SHADOWS_USER=0`. `vibeos_pmm_reserve` **truncates** the region, so `pmm_base` is `0x1780000`, above the window entirely |
| The firmware framebuffer sits inside RAM the allocator hands out of | print `framebuffer_base` against the PMM region | `FB_IN_RAM=0`. Framebuffer at `0x80000000`, PMM ends at `0x1bb5d000` |
| The desktop writes past its buffer | a canary page after it, checked at end of boot | `guard_broken=0`, including on failing boots |
| The frames are handed out twice | owners and state of every frame in the range, at end of boot | `backbuf_shared=0`, `backbuf_lost=0` |

The last one was **already answered by a counter nobody had connected to this
question**: `frame_take` increments `double_allocs` when it takes a frame that
is not FREE or already has an owner, every allocation goes through it including
the contiguous path, and the gate asserts it. It reads zero. The case file's
stated next step had been answered before it was written.

### The detector that was wrong first

`backbuf_shared` initially asked for `owners != 0` and reported **1000 of 1000**
frames shared, on every boot. That is the shape of the 3,019 use-after-frees
that cost a phase: a detector firing on everything is reporting its own
baseline. `frame_take` sets `owners = 1` when it hands a frame out, so one owner
*is* the allocated state. The test is `> 1`.

## What eighteen boots said about the argv defect

**It did not reproduce once.** The case file records roughly one boot in six;
the probability of none in eighteen at that rate is about 3.6%. That is not
proof of absence, and it is not treated as one — but the defect cannot be
studied at this rate, and the four instruments above now stand as a permanent
answer to four of its explanations.

## What did reproduce: three boots in ten, byte-identical

```
reason=invariant_failed:stress_run_did_not_finish
```

**The stress run had finished.** It completed 120 rounds and exited zero —
`SVC_EXIT svc-stress 10 0` is in the same log, the machine reached its CLI, ran
commands and halted cleanly. What went missing was the marker, and the log held,
identically on all three failures:

```
[HW][SYS] write(ring3): ^@^@^@^@^@^@^@^@^@^@^@^@^@^@^@^@=120
```

Sixteen NUL bytes, then `=120`. The program calls
`say("STRESS_OK rounds=", rounds, 1)`, and `STRESS_OK rounds` is **exactly
sixteen characters**. Bytes 0–15 of the buffer read as zero; bytes 16–20 read
correctly.

`say` builds its text in `char line[128]` — on the **user stack** — and hands
that address to `write`. So this is a user stack page whose first sixteen bytes
the kernel did not see.

Byte-identical repetition is what makes this worth chasing over the argv defect.
Random corruption is not identical three times, and this project's own rule is
to look for a signature present in one state and absent in another rather than
reaching for a ratio.

## The reading, kept separate from the measurement

`frame_take` hands a frame out zeroed. A page reading zero where a process wrote
is therefore a *fresh* frame, and the obvious way a process's page becomes a
fresh frame is the copy-on-write copy. Sixteen bytes lost followed by four
correct would mean the copy landed between two stores — the earlier ones going
to the frame that was abandoned, the later ones to the one that was kept.

**Consistent and unproved.** The next step is to make the kernel say it rather
than reason about it: record, for the frame behind a suspicious user read,
whether it was copy-on-write resolved during this process's life, and what the
old and new physical addresses were. This project's rule that a reason must name
a mechanism rather than a situation applies — "read as zero" is a situation.

## The check that should have caught it, and why it is not at fault

`interleaved_lines` runs first, before every other assertion, precisely so
nothing is concluded from an untrustworthy log. It reported nothing.

Its docstring already states the limit that let this through: *"a split that
lands inside a ring-3 write and truncates no number is not caught here."* And
this is not a split — the console lock was never involved. A NUL in a text write
is a different mechanism, and widening the interleaving check to cover it would
have conflated the two.

So it got its own check, `ring3_writes_with_nul`, beside that one and before
everything downstream, with self-tests including a healthy line it must not
flag. The gate now says `ring3_write_corrupted` and names the memory manager,
instead of `stress_run_did_not_finish` and naming a program that did its job.

That is the whole value of this entry. Three boots in ten were failing with
evidence that pointed at `svc_stress.c`, which is correct, and would have kept
pointing there for as long as it took someone to notice that the marker was
mangled rather than absent.

## Also fixed: a script that broke the rule it was written to serve

`hunt-argv.sh` kept a serial log only when it found an argv refusal, so a sweep
that produced two failures of another kind overwrote both before anybody read
them. That is this project's rule about not destroying evidence, broken by the
script written to gather it. It keeps every failing boot now.
