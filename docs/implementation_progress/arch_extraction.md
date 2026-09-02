# Splitting arch_hw.c

Status: **first cut done.** 9088 → 8544 lines.
Last review: 2026-09-02

## Why

`kernel/arch/x86_64/arch_hw.c` reached 9088 lines. Worse than the number is how
it got there: during a session spent explicitly on "reducing the monolith" it
**grew by 633 lines**, and nobody noticed until the person paying for the work
asked why a file being reduced had gone from seven thousand to nine.

The growth was not careless. Every addition was a diagnostic or a fix that
earned its place — `hw_ctx_check`, `hw_cache_audit`, `hw_exec_refuse`, the panic
that parks every core. The problem is that the plans which were supposed to
shrink the file only ever added portable code *beside* it. The task state
machine became portable; `g_tasks[]`, `hw_task_alloc` and `hw_pick_next` stayed
exactly where they were.

**So this plan has one criterion the others lacked: the line count of
`arch_hw.c` is a completion condition, not a side effect.**

## Why it was hard to start

Everything in that file is `static` over shared file-scope globals. Nothing can
move until somebody decides what the rest of the file is allowed to see. That
decision is now `kernel/arch/x86_64/arch_hw_internal.h`, and it is deliberately
not a public API: nothing outside `kernel/arch/x86_64/` includes it, and a
declaration arriving there should be read as a cost — a thing two files must now
agree about — rather than as progress.

## Cut 1 — the Linux socket syscalls (done, 2026-09-02)

`kernel/arch/x86_64/linux_socket.c`. None of what it does is architecture:
reading a `sockaddr` out of user memory, allocating a descriptor, blocking until
a connection arrives is Linux ABI translation over the portable TCP/IP stack in
`kernel/net/inet.c`. It sat beside the GDT because that is where the file
started.

**The numbers, including the part that is not flattering:**

| | lines |
|---|---|
| `arch_hw.c` before | 9088 |
| `arch_hw.c` after | **8544** (−544) |
| `linux_socket.c` | 416 |
| `arch_hw_internal.h` | 258 |
| the three together | 9218 (+130) |

The monolith lost 544 lines and the tree gained 130. That 130 is the seam, and
it is the right thing to watch: a cut that costs more header than it removes
source is a rename.

**The seam is ten functions and six globals wide** — and it is wider than the
socket code alone needed, which is worth saying out loud. The socket file wants
`hw_task_t`; a task holds a saved register frame, an address space and a
descriptor table, so moving the task moved `vibeos_x86_64_isr_frame_t`,
`hw_proc_t`, `vibeos_hw_aspace_t`, the Linux errno values and five constants
with it. That is the honest cost of the first cut, and most of it is paid once:
the next files to leave will find those types already on the far side.

One deliberate narrowing: `g_current_task` is a macro over per-CPU state.
Exporting it would have put `hw_cpu_t` on the header for the sake of one
integer, so the seam has `hw_current_task()` instead. A file that has been
lifted out has no business knowing how this kernel finds the current core.

## Next cuts, in the order they look worth doing

Measured against the file as it stands now:

| section | lines | goes to |
|---|---|---|
| delivering a signal | ~915 | `linux_signal.c` |
| the Linux syscall layer | ~693 | `linux_syscall.c` |
| what a C runtime asks for | ~408 | with the syscall layer |
| the kernel log | ~434 | `kernel/core/` — portable |

The log is the only one of those that can become genuinely portable rather than
merely a separate file in the same layer. The other three stay architecture-
adjacent, and that is fine: a 900-line file that does one thing is not the
problem this plan exists to solve.
