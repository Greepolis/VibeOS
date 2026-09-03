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

## Open decision: the second syscall layer (deferred 2026-09-02)

`kernel/core/syscall.c` and `kernel/proc/process.c` are a
complete syscall dispatcher and process table that **nothing in ring 3 reaches**.
The real path is `vibeos_x86_64_syscall_entry`, wired through MSR_LSTAR
into the `hw_sys_*` functions in `arch_hw.c`; the portable
dispatcher is called only from `user/lib/user_api.c`, which nothing else
in `user/` uses, and from the host tests.

An external review found three real defects in it, all latent for that reason:

- `vibeos_thread_create` takes a raw pid with no caller/target
  relationship check, unlike `PROCESS_TERMINATE` beside it - a thread
  created that way inherits the target process's security token.
- `vibeos_proc_terminate` marks a slot TERMINATED and never clears
  `in_use`, so after 32 process creations every further one fails
  permanently.
- The caller id used for every privilege check comes from
  `frame->arg2`, supplied by the caller: passing 0 impersonates the
  kernel.

**Deferred until the memory-manager plan is finished, deliberately.** The
question is not "are these bugs" - they are - but whether that layer should
exist at all, and that depends on what the memory manager ends up exposing.
Deciding now means deciding with less information than we will have in two
phases.

The risk of deferring is bounded and worth stating plainly: none of it is
reachable from ring 3 today, so this is a decision about architecture rather
than an open hole. The risk of *not writing the deferral down* is the real one -
a month from now this is simply code that is there, and nobody remembers it was
a choice.

**When it is taken, the options are:** remove it, wire it and fix it first, or
keep it as the host-tested model it currently is with a comment saying so. What
must not happen is that it becomes reachable before the three defects above are
fixed.

