# S-P6 - the quantum is spent, and a thread is not a zombie

Two gaps were named when S-P5 closed, and this is both of them.

## The time slice is spent, not merely declared

The scheduler computed a slice per class and then re-picked on every timer
tick, so the slice was a number nothing read. `hw_schedule` now decrements
`slice_left` and returns without re-picking, unless a higher class became
runnable.

Gated rather than inspected. The boot gate compares accounted ticks against
scheduler runs: with the quantum enforced a boot measures `ticks=491 runs=82`,
and with the check removed it measures `335/335` - one run per tick, which is
what "the slice is a number nothing read" looks like from outside. The
assertion was confirmed to go red by disabling the quantum.

## A thread has no reaper, so it must not become a zombie

`tests/linux/musl_texec.c` was written to cover the one combination nothing
exercised: threads created and joined while children exec. It found a defect on
its first run, and the defect was not in exec.

An exiting task became a ZOMBIE and waited to be reaped. `waitpid` matches on
`ppid`, and a thread inherits its *creator's* parent rather than becoming its
child - so the thing that joins it cannot wait for it, and nothing else is
looking. Every thread that ever exited held its slot for the rest of the boot.

The failure surfaced nowhere near the threads. A program creating and joining
eight threads leaked eight slots; several commands later the shell's fork was
refused on a machine with plenty of memory. Six `too-many-children` refusals in
one boot, then a wedge. The fork guard added in S-P5 is what made it visible -
before that the slots were simply gone, and the table quietly ran out.

Joining is already synchronised without a zombie: exit clears the word the
joiner sleeps on and wakes the futex. The zombie was carrying an exit status
nobody was able to ask for.

Threads are now reaped where they die, and the refusals go from six to zero.

### Two things this cost, both worth writing down

**A slot released outside the transition table stays RUNNING.** The first
version called `hw_task_release` directly, which is `running -> free`. The
table refuses that, correctly, and logged one `[TASKS] ILLEGAL` line per
thread - so instead of leaking a zombie the kernel leaked a task stuck in
RUNNING, which is worse and quieter. The table was right and the fix was wrong.
It goes through ZOMBIE now. Without that table the change would have looked
like it worked.

**One boot argued the opposite of the truth.** Raising the task table from 24
to 32 slots measured a broken boot, and 24 measured a working one, so 32 was
recorded as the cause. It was not: the leak was, and with the leak fixed 32
boots clean eight times out of eight. Two single boots on a machine with a
known intermittent failure decided a question they cannot decide - which is
this project's own rule about counts, applied to a sample of one.

## What is verified

- 8/8 clean boots with `repeat-boot.sh`.
- `TEXEC_OK rounds=8 execs=8` when the program is run.
- Zero `too-many-children` refusals in a boot that previously had six.
- Zero `[TASKS] ILLEGAL` transitions.

## What is not

`musl_texec` is **not** in the boot script. It passes its own rounds, but
running it in the boot wedges the guest in a later phase, and that is an open
defect rather than a passing test. The gate asserts on it whenever it is
started - keyed on the program having been started, not on the binary being
present on the media, because those are different questions and the difference
already cost a run.
