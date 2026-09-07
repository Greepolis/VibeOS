# Splitting arch_hw.c

Status: **two cuts done.** 9088 → 8359 lines.
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

## Cut 2 — signal delivery (done, 2026-09-02)

`kernel/arch/x86_64/linux_signal.c`. Building a frame on a user stack,
calling a handler on it, and taking the frame back when the handler returns is
Linux ABI, not x86. What is architectural about it - the selectors, the trap
frame layout - is named in the seam header and nothing else in the file needs to
know.

**The two halves were four hundred lines apart**, with the crash dumper and half
the syscall table between them. That is the more useful thing this cut fixed:
`hw_signal_deliver` and `hw_sys_rt_sigreturn` are one
subject and now sit together.

| | lines |
|---|---|
| `arch_hw.c` after cut 1 | 8544 |
| `arch_hw.c` after cut 2 | **8359** (−185) |
| `linux_signal.c` | 154 |
| seam header | 258 → 330 (+72) |

**A correction to this plan's own estimate.** The table below called this
section 915 lines. It is not: the banner "delivering a signal" covers signals,
the memory-diagnostic walk, the panic summary, the crash dumper *and* the whole
Linux syscall table. The signal code proper is 185 lines. Section banners in
that file describe where somebody stopped writing, not what follows.

**The diagnostics group was measured and deliberately left.**
`hw_walk_step`, `hw_frame_still_mapped`,
`hw_panic_cpu_summary`, `hw_dump_vanished` and
`vibeos_x86_64_crash_dump` are ~427 cohesive lines and would make a good
file - but they touch eleven globals including `g_cpus`, which would put
`hw_cpu_t` on the seam. That is exactly the widening avoided in cut 1
with `hw_current_task()`. It needs an accessor pass first, so it waits
rather than being taken at the wrong price.

## Cut 3 — the storage and I/O bring-ups (done, 2026-09-07)

**11166 -> 9995 lines.** And the first number is the point of this section.

Between cut 2 and cut 3 the file went from 8359 to 11166 lines. Not because
anybody decided to grow it: the I5 and I5b work - filesystem images, the log
sink, the scratch device, the volume scan, the mount report - was written
straight into it, one bring-up at a time, over three days in which this
document sat here saying the length of the file is the completion criterion.

That is the same failure this plan already records once ("a session dedicated
to reducing the monolith grew it by 633 lines"), at four times the size, and by
the person who had read the note. Writing it down again rather than quietly
fixing the number, because the lesson is evidently not learned by being written
once: **new code goes to its own file on the day it is written, not after.**

What moved: `hw_swap_bringup`, `hw_write_proof`, the log sink bring-up and its
console tail, `hw_fsimages_bringup` and the filesystem-image table,
`hw_scratch_bringup`, `hw_volumes_bringup`, `hw_mount_report`. All of it runs
once from the boot path and none of it is reachable from a syscall, which is
why it separates cleanly.

The seam measured eight names in and seven out - narrow for a monolith, and
narrow *because the code was new*. It had not yet grown the incidental
couplings that make the older sections hard to separate. That is an argument
for cutting recent additions first: the seam is cheapest on the day the code
lands and gets more expensive every week it sits there.

Six statics had to stop being static (`g_rootfs`, `g_swap_bitmap`,
`hw_alloc_page`, `hw_free_page_why`, `hw_frame_identity_map`, and the swap-slot
constant moved to the header beside the array it sizes). They are grouped under
one banner in `arch_hw_internal.h` so the next cut can see what this one cost:
every line there is encapsulation given up to buy separation.

Verified by the boot, not by the compiler: all six mounts, all four filesystem
images and the log sink read back exactly as before.

## Next cuts, in the order they look worth doing

Measured, not estimated from section banners - which is what cut 2 taught:

| section | lines | goes to | blocked on |
|---|---|---|---|
| crash and wedge diagnostics | ~427 | `arch_diag.c` | needs an accessor for `g_cpus` |
| the Linux syscall table | ~693 | `linux_syscall.c` | - |
| what a C runtime asks for | ~408 | with the syscall table | - |
| the kernel log | ~434 | `kernel/core/` — genuinely portable | - |

And one that is not a move at all: **X-P2 steps 3 and 4**. Demand paging removes
the eager read, and with it `g_exec_elf`, `g_interp_elf`, the staging cache and
the copy loop around them - several hundred lines that leave without needing a
seam, because they stop existing.

The log is the only one that can become genuinely portable rather than merely a
separate file in the same layer. The others stay architecture-adjacent, and that
is fine: a 700-line file that does one thing is not the problem this plan exists
to solve.
