# P7 — the acceptance properties

The phases above build the memory manager; this one is meant to prove it is
ready. What makes these tests different from every other test in the subsystem
is that each exercises a path an ordinary run never reaches: the unwind after a
failed allocation, the state of a machine that has run out, the contents of a
frame handed to its second owner. None of those happen on a boot that goes
well, so none of them had ever been executed until something went wrong in
front of somebody.

## What is proved

**Exhaustion.** Allocate until the allocator refuses, then give everything back
and check the machine is where it started. The property is not that allocation
fails — it is that nothing changed. An allocator leaking one frame per refusal
looks identical from outside until the leak matters, and by then there is
nothing to point at.

The test also **re-allocates everything afterwards**, and that half was added
because a sabotage case walked through the first version. The free *count* and
the free *list* are separate: a release that decrements one without linking
into the other leaves a machine that believes it has memory and cannot hand any
out. "The count came back exactly" was true and meant nothing.

**Isolation.** A frame's second owner never sees the first one's data. The fake
RAM is filled with `0xCD` rather than zero, deliberately — on zeroed memory the
test passes whether or not the kernel clears anything.

**Fault injection, and invariant I5 proved rather than asserted.**
`vibeos_frame_fail_after(n)` refuses the nth allocation from now. Building an
address space and mapping a page allocates several times — tables at three
levels, then the frame — so sweeping n exercises a different unwind path each
time, and after each one the free count must be exactly what it was.

A count rather than a rate, because "the third allocation of this operation" is
reproducible in a way that "one in twenty" is not. And the sweep ends by
checking that the injection **fired at all**: a sweep that never triggers
passes every assertion while proving nothing, which is the failure this project
has hit repeatedly in the last few phases.

The hook is compiled in, deliberately. The alternative is a build the tests
cannot reach, which is how a subsystem comes to have unwind paths nothing has
ever executed — the exact situation this phase exists to end.

**No mapping survives a failed address space.** The reverse map is the one
structure that could keep a record of a mapping whose page tables have been
freed, and a stale holder there is worse than a leaked frame: compaction would
follow it and write into memory that now belongs to something else.

## What is not proved here

**Soak.** The plan asks for the stress service with its round count raised by
two orders of magnitude, and `frames_free` matching its value at
`USERLAND_START`. The measurement exists — both numbers are printed, with the
page cache's resident count named alongside so the arithmetic is honest — and a
boot currently shows twenty-six frames unaccounted for. Raising the round count
needs a boot budget this gate does not have.

**Latency.** "No path reachable from a syscall waits for another core without a
bound." Several bounds exist and are asserted — the TLB shootdown's
acknowledgement count and timeout, virtio-net's transmit bound, the console
lock's bad-unlock count — but there is no systematic sweep of every wait, and
claiming one would be claiming more than has been done.

## Verified

Four host-test groups and three sabotage cases confirmed red, with a fourth
recorded in `scripts/dev/cases/mm-acceptance.txt` as behaving differently from
expected: it turns the suite red by segfaulting an earlier group rather than by
failing an acceptance assertion. That is written down rather than counted,
because "the suite caught it" and "this test caught it" are different claims.

One test was badly written first and is worth remembering: the isolation check
asserted per byte with an empty message, so a failure printed four thousand
blank lines. A test whose output cannot be read is a test nobody will use.
