# Two checks about the checks

**Status: both in `check.sh`, both sabotage-verified, and the second found a
real defect on its first run.**

## Why

Counting today's work honestly: of the defects found, more were in this
kernel's *detectors* than in the code they watch — a poison check reading
uninitialised state, frame accounting sampled from a machine that was still
forking, a trap dump reporting the plan instead of the outcome, a deadlock
reporter that took the console lock and therefore deadlocked, a diagnostic that
cleared its own evidence before it was read.

The tempting response is a framework that all detectors are written against.
That is the wrong answer: those five have different causes and only look alike
because they were found together, and an abstraction over them hides them
better rather than preventing them.

Two of the shapes, though, are mechanical, and both were present in the same
defect. `VIBEOS_BLK_TIMEOUT` was defined, printed by kmain, **asserted by the
boot gate**, and **produced by no driver** — so the assertion was green by
construction for as long as it existed, and it took reading the source to find.
Neither half needs judgement to detect.

## `check-assertions-covered.py`

Every name the gate can put in `reason=`, against every sabotage case in
`scripts/dev/cases/`. An assertion no case mentions has never been observed
failing and might not be able to.

**115 assertions; 7 covered.** That number is the finding.

It is a **ratchet, not a target**: zero would start red, and a check that
starts red is one nobody reads — this project has a rule about that. The
baseline is what the tree had when the check was written and may only go down.
What it catches from day one is a *new* assertion arriving without a case,
which is exactly how `BLK_TIMEOUT` happened.

It does not claim that a case was run or that it turned that assertion red —
only that somebody wrote one. That is the cheap half, and the half that hides
an assertion which cannot fire.

## `check-counters-produced.py`

Every field of every reported stats struct, against every `++`, `+=` and
non-zero assignment in the tree. A counter nothing increments is an assertion
that cannot go red.

**It found one immediately: `vibeos_rmap_stats.cycles`.** The boot gate asserts
`rmap_cycles` is zero. Nothing had ever written to it — `vibeos_rmap_init` did
not even reset it — so the check was `BLK_TIMEOUT` again, in a different
subsystem, and had been since the counter was declared.

Worse, the thing it was declared for was real and unguarded: every list walk in
`rmap.c` was `while (cur != RMAP_NONE)` with no bound. A cycle is not a wrong
answer there, it is a core that never comes back — and this kernel has had
exactly that, when the node pool was carved after `vibeos_frame_init` and user
pages overwrote the lists, leaving CPU#0 in `vibeos_rmap_add` for the rest of
the boot. The walks are bounded by the pool size now, and hitting the bound is
what produces the counter.

It also found `vibeos_reclaim_stats.skipped_pinned` dead: `vibeos_anon_reclaim`
does refuse pinned frames and never said so, so "reclaim found nothing to take"
and "everything reclaim looked at was pinned" were the same silence — and one
of those is the kernel holding memory it will not give back.

### The exemption list is the point as much as the failures

Eighteen counters are declared and legitimately unproduced: reporting that
exists ahead of the phase that will fill it, which is the order this project's
plans use deliberately. Each one names its phase. An entry parked there because
it is inconvenient would be this check's own defect wearing a permission slip,
so a **stale** exemption — one the tree now produces — fails too.

### What it cannot do

It matches by field name across every watched struct, so two structs sharing a
name cover for each other: `io_stats.cache_hits` reads as produced because
`mm_stats.cache_hits` exists, and those are different caches. Fixing that needs
to know which struct an expression belongs to, which needs a C parser. The
limitation is named in the tool rather than papered over, because a checker
that quietly gets this wrong is the exact failure the checker is for.

## Confirmed by breaking them

An assertion added with no case: `assertions-covered=FAIL uncovered=109
baseline=108`. A producer removed: `counters-produced=FAIL ... nothing ever
increments: vibeos_rmap_stats.cycles`.

## What is deliberately not automated

"A detector must not depend on the thing it watches." "Evidence must not be
cleared before it is read." "A threshold comes from a distribution, not from
one measurement." Those are judgements, and they belong in CLAUDE.md with the
other lessons — where three of them already were, and were broken today by
somebody quoting them.
