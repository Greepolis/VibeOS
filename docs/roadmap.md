# Subsystem Rewrites: Order and Scope

What is planned, what is deliberately sketched rather than planned, and why.

The memory manager is being rebuilt to a written plan with layers, invariants,
host tests and sabotage cases. That treatment came from one decision - stopping
to plan before writing code - and it is the only part of this kernel that has
it. Everything else lives in `kernel/arch/x86_64/arch_hw.c`: 8312 lines, 267
static functions, holding the scheduler, syscalls, program loading, pipes,
signals, the network glue, the interrupt controllers and the console, with no
boundary between any of them.

## Order

| # | Subsystem | Plan | Why here |
| --- | --- | --- | --- |
| 1 | Memory manager | [docs/mm/](mm/README.md) | In progress: P0-P3 done, P4 at steps 1-2 |
| 2 | Tasks and scheduling | [docs/sched/](sched/README.md) | Full plan. Where the most expensive defects have been, and P4 step 3 needs its ownership layer |
| 3 | Program loading | [docs/exec/](exec/README.md) | Full plan. **Blocks memory P4 step 3**: execve cannot fault pages in while it copies through a staging buffer |
| 4 | Syscalls and the Linux ABI | sketched below | After the three above, because it sits on top of all of them |
| 5 | Network and pipes | sketched below | Least entangled with memory and lifetime; least urgent |

## Why 4 and 5 are sketched and not planned

A plan written for work that starts in a month is a prediction, not a design.
The memory plan has already been corrected twice at contact with reality - the
ownership bit it specified was already taken by `PTE_COW`, and its completion
criterion measured a pre-existing flakiness rather than the phase. Both were
found by executing, not by planning harder.

So the two furthest subsystems get their objective, their invariants and their
boundaries recorded now, while the knowledge is fresh, and the phase-level
detail when they are next.

### 4. Syscalls and the Linux ABI

**Objective.** One dispatch table, one place that validates arguments, and a
translation layer that says what it does not implement.

**Invariants worth writing down now**

1. Every syscall argument that names user memory is validated in one place, and
   the reason for a refusal is reported (`hw_user_range_why` exists for this).
2. An `int` argument arrives zero-extended; comparisons happen through
   `VIBEOS_ARG_INT` and never against all 64 bits. This has bitten and is
   recorded in CLAUDE.md.
3. A `sigset_t` is converted at the boundary and nowhere else.
4. An unimplemented syscall is refused and counted, never silently succeeded.
5. A wait status carries the signal in the low seven bits and the exit code in
   the high byte - and every producer and consumer agrees.

**Boundaries.** Dispatch and validation are portable; the register ABI and the
`syscall` entry are not.

### 5. Network and pipes

**Objective.** The receive path stops being reachable from two contexts without
a stated rule.

**Invariants worth writing down now**

1. The stack is entered from syscalls and from the timer interrupt; exactly one
   lock covers both, and nothing slow happens under it.
2. A pipe's end count is a refcount, not a flag: a descriptor can be duplicated
   and inherited, and end-of-file depends on getting that right.
3. Buffers handed to the device are owned by exactly one side at a time.

## The rule that applies to all of them

**Check a criterion against the baseline before using it to judge a change.**
Written here because it cost two days on the memory manager: a phase was
declared unfinished for a day and a half against a boot count that no revision
of this project has ever met.
