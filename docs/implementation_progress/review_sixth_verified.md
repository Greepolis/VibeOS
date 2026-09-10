# Sixth review: all three findings are in a file that no longer exists

Three findings, titled "soli problemi verificati e raggiungibili" — only
verified and reachable problems. All three name `kernel/core/syscall.c`, which
was deleted earlier the same day in `4462174`.

That is not a gotcha about timing. It is the **fourth review in a row** to land
entirely in that file, and the reason the file was deleted.

| | finding | location | verdict |
|---|---|---|---|
| H-001 | THREAD_CREATE bypasses ownership on `target_pid` | `kernel/core/syscall.c` | file deleted; shape cannot exist in the live path |
| M-001 | userspace reaches the global `boot_event` | `kernel/core/syscall.c` | file deleted; no live syscall touches it |
| M-002 | lazy init of the global waitset is not SMP-safe | `kernel/core/syscall.c` | file deleted; **the identifiers no longer exist anywhere** |

## Verified rather than asserted

**The dispatcher was unreachable from ring 3 before it was deleted.** At the
parent commit, `git grep vibeos_syscall_dispatch -- kernel/arch/` returns
nothing. The arch layer never named it. Nothing a user process could do reached
any of these three.

**M-002 is gone outright.** `waitset_initialized` and `kernel_waitset` have zero
matches in the tree. The race was real in the code as written and there is no
longer any code.

**M-001 is gone outright.** `boot_event` survives in exactly two places, both in
`kmain.c`: `vibeos_event_init` at startup and one `vibeos_event_signal` during
boot. No syscall reaches it. The user-facing `vibeos_user_signal_boot_event` was
removed in the same commit.

**H-001's shape cannot exist in the live kernel.** `vibeos_thread_create` in
`kernel/proc/process.c` now has no caller outside the host tests. The path a
real program takes is `hw_sys_clone_thread`, Linux `clone()`, whose parameters
are flags, stack, ptid, ctid and tls — **there is no target process**. The
parent is `g_current_task`. A caller can only create a thread in itself, so
there is nothing to check ownership of.

## But the principle behind H-001 is right, and the live kernel does implement it

"Check ownership before acting on somebody else's pid" is a real requirement,
and the live kernel does have syscalls that take a target pid. They go through
one function:

```
hw_signal_permitted  — 1 definition, 4 call sites (kill, tgkill, tkill, sig 0)
```

`check-chokepoints.py` declares that count and fails if it moves **in either
direction** — a fifth caller that copied the guard, or a fourth that quietly
stopped checking. So the property this finding is really about is enforced and
watched, in the layer that runs.

## What is worth taking from this review

Nothing to fix, and one thing to notice.

Four consecutive reviews have concentrated on unreachable code, and the reason
is structural rather than careless: `kernel/core/` **reads like the kernel**. It
has a syscall dispatcher, a security layer, a process table, a waitset. A
reviewer given this tree will study it, and every hour they spend there is an
hour not spent on `arch_hw.c`, which is where every security defect this project
has actually had was found.

The banner added to those files after the third review helped whoever read the
file top to bottom. Deleting the dispatcher helps more. What remains in that
shape is `kernel/proc/process.c` — 1142 lines with a process table, thread
creation and a capability model that nothing on the machine consults. It is
C5's subject and it is deliberately still here, which means **a seventh review
can still land in it**.

That is now an argument for taking C5 sooner rather than later, and it is the
first argument for the ordering that came from outside the project.
