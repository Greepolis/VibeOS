# Tasks and Scheduling: Decisions

Questions I will not settle on my own, and the ones already settled, with the
reasoning kept rather than the conclusion alone.

The rule from the memory manager applies here too: **a decision is asked before
the phase that needs it, not at the start.** Deciding early means deciding with
less information, and this plan has already been wrong twice about things that
looked settled.

## Open

| # | Question | Why it is not mine to settle | Needed before |
| --- | --- | --- | --- |
| T1 | Does a task reference become (slot, generation) everywhere, or only where a stale index has already bitten? | Everywhere is safer and touches perhaps forty call sites; selectively is cheaper and leaves a rule with exceptions, which is how the exec defect happened | S-P1 |
| T2 | `kernel/sched/`, or fold into `kernel/core/`? | The same question D1 answered for memory, and the answer may differ: `kernel/core/` now has real runtime work | S-P1 |
| T3 | Is the task table's size still 24, or does it become a build-time parameter? | 24 is enough for what runs today and a fork storm is the acceptance test; making it configurable is a small change that invites a large one | S-P1 |
| T4 | Does the run queue stay round-robin, or is a ready-list per CPU with work stealing worth it now? | A policy question with a cost this workload cannot measure; my instinct is no, but it is a product decision | S-P2 |
| T5 | Should `execve` kill sibling threads, as POSIX says, or keep refusing to destroy their address space? | POSIX is clear and this kernel is not POSIX; killing threads from exec is a lifetime change with its own races | S-P3 |
| T6 | How is a boot-count criterion expressed, given the pre-existing flakiness? | A ratio against the parent commit is my proposal; the alternative is to fix the flakiness first, which is open-ended | S-P2 |

## Taken

### T0 — Counters before code, decided 2026-08-31

S-P0 lands first, exactly as the memory manager's P0 did.

The argument is not symmetry. The memory manager's counters were what turned
"something is wrong with memory" into "this frame, this process, this
operation", and every hour saved after that was bought by the two hours spent
building them first. The task subsystem currently has three provenance fields,
each added after a bug had already cost a session, and no way to see the table
at all on a running machine.

The cost of doing this first is one phase of no visible progress. The cost of
not doing it is paid the first time a task defect appears and the only tool is
reading the code - which, in this project's own record, has reached the wrong
answer six times in one day.
