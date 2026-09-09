# C1: the checks that watch the contract

First instalment of the core plan's C1. Two checks now run in `check.sh all`
that did not, and the first finding was about the plan's own author.

## `check-reachable.py` was written and never wired in

It was committed two days before this entry and `check.sh` never called it. That
is **"configured and consulted by nobody"** — the defect `docs/core/` names as
the one this project produces most often, committed inside the document that
names it, by the person who wrote that sentence down.

A check nothing runs and a check that passes are the same thing from outside.
This is the fourth instance recorded here and the first where the mechanism was
a *check* rather than a kernel feature, which makes it worse: the whole value of
a ratchet is that somebody would notice it going red.

It reports `unreached=12 baseline=27` and runs on every `check.sh all` now.

## `check-chokepoints.py`, and how measuring changed its design

The plan specified it as: each security check declares how many call sites it
has, so that a *second* one is noticed — the failure where a handler is copied,
its guard is copied, and the two drift. C4 moves 4,169 lines of syscall handlers
out of `arch_hw.c`, which is exactly that operation.

Counting the real numbers changed the shape of the check:

| guard | sites |
|---|---:|
| `hw_user_range_ok` | 44 |
| `hw_user_range_why` | 6 |
| `hw_signal_permitted` | 5 |
| `hw_task_alloc_guarded` | 3 |
| `hw_user_addr_ok` | 2 |

`hw_user_range_ok` has forty-four because every syscall taking a user pointer
validates through it. For that guard the dangerous direction is the count going
**down**: a syscall that quietly stops validating is a hole, and it reads like a
simplification. Nothing else in the tree would see it.

So the check asserts the declared number in **both** directions. It is not a
prohibition — adding a syscall legitimately moves the count, and the fix is to
edit the number in the same commit. Same rule this project already applies to
the mm-layering allowance: raise a limit as a decision, not as a reflex.

### It caught its author first

The declared counts were set from `grep` over `kernel/arch/x86_64/*.c`; the
check walks all of `kernel/`. Two of the five numbers were wrong on the first
run — `hw_user_range_ok` also has sites in `linux_socket.c` and
`linux_signal.c`, and `hw_task_alloc_guarded` had one fewer than assumed. The
numbers are the measurement now.

### Sabotage, both directions

```
unmodified                              chokepoints=ok watched=5
one hw_user_range_ok site removed       FEWER than declared   → red
a second hw_signal_permitted site       more than declared    → red
restored                                chokepoints=ok watched=5
```

Cases in `scripts/dev/cases/core-chokepoints.txt`, including the honest failure
mode of every ratchet here: editing the declared number to match instead of
looking at what moved, which turns the check off for that guard and looks green.

### What it cannot judge

It counts textual occurrences, not calls: a name in a comment counts, a call
through a function pointer does not. Every guard here is called by name today.
Stated rather than worked around, for the same reason `check-reachable.py` asks
"is this named anywhere" instead of building a call graph it would have to fake:
a confidently wrong answer is worse than a coarse one.

## Still to come in C1

- `check-subsystem.py` — the seven parts of a module, ratcheted.
- `check-blast-radius.py` — files touched to add a filesystem (1), a block
  driver (1), a character device, an input device, a network interface, a
  display, a syscall, an ABI (`arch_hw.c` for all six). That table is the plan's
  real progress metric and it does not exist yet.
