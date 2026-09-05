# P7's soak property, and the premise it was measured against

**Status: the soak property is proved.** The frame loss across a boot does not
grow with the work: 26 frames at 120 stress rounds, 28 at 12000, measured twice
at the high count with the same answer. A leak of one frame per fork would put
that in the thousands. The twenty-six are a fixed startup cost, which is a
different problem from a leak and is still open.

Getting there took finding a defect in the machine, not in the counter.

## The measurement

`svc-stress` runs randomised rounds of fork, mmap, copy-on-write and pipe work.
Its round count is a build knob (`-DVIBEOS_STRESS_ROUNDS=`) and a second
argument for a human replaying a failure, because a failure at round 9000
cannot be reproduced by a default 120-round run.

`scripts/dev/soak.sh` runs the boot at two round counts and prints both losses.
One measurement proves nothing here: twenty-six frames is a fixed cost or a
per-round leak, and those are the same number with opposite consequences on a
machine that stays up. The comparison is the test.

## The defect it found

The first high-round runs came back red and self-contradictory:
`serial_log_interleaved`, `mm_rmap_mismatch=1`, `meminfo_frames_missing`, and a
loss that grew with the rounds — 26, then 43, then 53. That reads as a leak.

It was not. The log's tail said so: `userland_finished` at 55.4 seconds, and
`svc-stress`'s forks still retiring after it, with the kernel CLI already up.

The wait loop that decides userland is over asked whether any user task was
`READY` or `RUNNING`. A **blocked** task — in `waitpid` after a fork, on a
pipe, on a futex — is neither. So a moment when every user task happened to be
waiting read as "nothing is running, we are done": the machine printed
`all user tasks retired` with tasks very much not retired, went on to the
console, and `kmain` sampled the frame accounting whose own comment states that
every user process has exited by that point.

It stayed hidden because an ordinary boot's programs are short. At 120 rounds
the stress run is usually over before the window can open. At 12000 it opened
on nearly every boot, and the mid-flight numbers looked exactly like a leak
that scaled with the workload.

`BLOCKED` counts as alive now. `ZOMBIE` deliberately does not: it holds a slot,
not an address space, and the caller goes on to reap it.

## The check that was missing

The accounting had a precondition and nothing verified it. That is the same
shape as the defect it hid, so the gate now asserts the ordering:
`FRAMES_AT_USERLAND_DONE` printed before `STRESS_OK` fails the boot.

It is a function with self-tests rather than an inline comparison, for a reason
worth recording. Sabotaging the kernel side — putting the blindness back and
booting at 12000 rounds — came back **green**: that run did not happen to open
the window. A single red run cannot prove the check fires and a single green
one cannot prove it does not, which is this project's own rule about counts
applied to a check instead of to a boot. Three synthetic logs can, and removing
the comparison turns them red.

So the sabotage case is recorded as environment-dependent, in the same spirit
as the AHCI cases that QEMU cannot exercise: "no case exists" and "the case
exists and this run could not tell" are different things.

## What is still open

The twenty-six frames. They are a fixed cost, not a leak, and the boot gate
holds them under a ceiling of 64 so they cannot grow unnoticed. Naming them is
its own piece of work.
