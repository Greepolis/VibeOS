# Scheduler torture, and the rule it is the first instance of

**Status: in the nightly, four sabotage cases, all of which turn it red.**

## The rule

**Every kernel module gets an intensive test in the nightly.** Not the host
tests — every module has those and they check the cases somebody thought of.
Intensive means long, randomised, and checked against something other than the
code under test.

`check-nightly-coverage.py` enforces it. A job says what it covers with a
`# module: <names>` comment, and the count of uncovered modules may only go
down. Seven of thirteen had nothing when the rule was written: `exec`, `fs`,
`io`, `ipc`, `object`, `proc`, `time`.

## What it tortures

Random sequences of admit, forget, renice, reaffine, charge and pick, with
every pick checked against a model kept independently in flat arrays:

* **class dominance** — a runnable KERNEL task runs before any NORMAL one,
  however long the normal one has waited;
* **affinity** — a slot is only picked on a core its mask allows;
* **admission** — a slot the policy never heard of, or has forgotten, is never
  picked;
* **liveness both ways** — nothing eligible means nothing picked, and something
  eligible means something *is*;
* **weighting** — a favourable `nice` accumulates virtual time more slowly.

The model is deliberately dumb: flat arrays, no cleverness. A model that shares
an idea with the thing it checks stops being independent, and the defects worth
finding here are the self-consistent ones. This kernel has had one: a task's
class was *derived* from `is_idle`, which silently collapsed KERNEL into
NORMAL, so a kernel task could not outrank anything — and it was consistent
with itself for months.

## Two things that had to be fixed in the test, not the kernel

**It reported nine defects that were not there.** On the first sweep, nine seeds
of a hundred and fifty failed the weighting check. They were the model's fault:
`nice` can change mid-run, so the model held the *final* value while the virtual
time had accumulated under earlier ones, and a forget-then-readmit resets the
policy's clock while the model kept adding. A slot only counts for that
comparison now if it has been admitted once, never reniced since and never
forgotten.

**And then it could not observe the property it was checking.** With the model
fixed, the weighting check waited for two slots to arrive *by chance* with the
same class, the same charged total and different `nice`. Sabotaging the charge
to ignore the weight entirely — so `nice` stopped mattering at all — was caught
on **zero seeds out of a hundred and fifty**.

That is this project's "right about the outcome, wrong about the mechanism",
and the only thing that told the two apart was breaking the scheduler on
purpose and watching the test stay green. The comparison is *constructed* now —
fresh slots, same class, charged identically, `nice` values chosen — and the
same sabotage is caught on 150 of 150.

The three picker properties were caught 40 of 40 from the start. That contrast
is what made the fourth's silence worth chasing rather than shrugging at.

## The four sabotage cases

| broken | caught on |
| --- | --- |
| class dominance removed from the picker | 40 of 40 seeds |
| affinity ignored | 40 of 40 seeds |
| a slot the policy never heard of is scheduled | 40 of 40 seeds |
| the charge ignores the weight | 150 of 150 seeds |

Clean tree: 0 of 150.

## What the nightly runs

400 seeds at 6000 rounds under AddressSanitizer and UBSan, then five seeds at
400,000 rounds. Two different things: many seeds reach orderings nobody thought
of, and long runs reach the ones that only appear deep into a sequence. The
sanitizer is the other half — the model catches wrong arithmetic, the sanitizer
catches walking off the slot table while doing it.

Every run prints its seed on the first line, so a failure is replayed with
`vibeos_sched_torture <seed> <rounds>`.
