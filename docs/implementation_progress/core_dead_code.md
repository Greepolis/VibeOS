# Taking the dead code out, first increment

`kernel/mm/vm.c` and `kernel/core/interrupts.c` are gone, with their headers,
their three syscall handlers, and their tests. 621 lines of kernel and three
test functions.

## They were not dead. They were built and never consulted

That distinction is the whole point, and it is worse than "dead code".

The live `vibeos_kmain` — the function that prints `BOOT_OK` — called all three
of these on every boot:

```c
vibeos_vm_init(&kernel->kernel_aspace);          /* line 42  */
vibeos_intc_init(&kernel->intc);                 /* line 80  */
vibeos_intc_bind_timer_irq(&kernel->intc, ...);  /* line 91  */
```

And nothing else in either file was ever called from anywhere the machine
reaches. `vibeos_intc_dispatch` in particular has no caller outside its own
file, so **the timer handler that `bind_timer_irq` registered could not fire.**
The arch layer's IDT, initialised a few lines above it, is the one that runs.

So this is not a corner nobody visits. It is *"configured and consulted by
nobody"* — the defect `docs/core/` names as the one this project produces most
often — at the scale of two subsystems, executed on every boot.

## The three syscalls that went with them

`VM_MAP`, `VM_UNMAP` and `VM_PROTECT` operated on `kernel->kernel_aspace`, an
abstract `maps[]` array that was never the machine's page tables. Whatever they
returned said nothing about any mapping any process runs on. An external review
flagged `vm.c` for exactly this — that an abstract model should not be read as
hardware enforcement — and classified it correctly as informational.

## What it cost on the test side, measured before cutting

`kernel_tests.c` had 64 references. They split cleanly:

| | |
|---|---|
| `test_vm`, `test_vm_user_address_space_contract`, `test_interrupts` | only touched the removed subsystems — **0** other `vibeos_*` references — removed whole |
| `test_syscalls` | 2 references against 290 others — the VM syscall block removed |
| `test_timer_and_idt` | 4 against 22 — rewired |

The timer coverage is preserved rather than lost: `vibeos_timer_on_irq` is what
the removed handler called, and the test drives it directly now.

### And a mistake made rewiring it

`vibeos_timer_on_irq` returns **1** when the divider is satisfied and it ticks,
**0** when it only accumulates, −1 on error. The removed handler discarded the
value with `(void)` and the test checked `vibeos_intc_dispatch` instead, so the
three-valued return was nobody's problem until it became the call being made.
Replacing it with `!= 0` turned a tick into a failure. It is `< 0` now.

## What the removal proved about the tool that should have found it

`check-reachable.py` never flagged either file, and could not have.

It asks *"is this function named anywhere outside the file that defines it?"* —
and these were named, by `kmain.c` and `syscall.c`, both of which are themselves
unreached. The question cannot see **"named only by code that is itself dead"**.

That limitation was written into its docstring as a deliberate trade: a real
call graph would have to understand function pointers, which every registration
seam uses, and would report the frame layer's lock as unreachable. The trade was
the right one and it still cost this. The stronger version — a transitive walk
seeded from the arch layer's entry points, treating any function whose address
is taken as reached — is now recorded in that file as its next improvement.

The baseline is tightened to 12 in this change, from 27. It has been 12 for
days; lowering it is a decision, and this is the commit that earns it.

## What is left, and the order

| | lines | |
|---|---:|---|
| `kernel/core/syscall.c` | 1502 | **C3's decision** — this is where the ABI vocabulary might live |
| `kernel/proc/process.c` | 1142 | same call |
| `kernel/core/kmain.c` | 914 | **this claim was wrong** - see the second increment below |

`kernel_tests.c` is 9,028 lines and still holds roughly 330 references to that
half — most of the largest test file exercises the kernel that does not run,
which is part of why it is always green.

C3 says the choice between deleting and integrating is made by asking which
produces the smaller diff in C4. The measurement for that now exists.

# Second increment: kmain.c, and the claim it corrected

The previous section said `kmain.c` has "17 dead functions around one live entry
point". **That was wrong**, and it was wrong by the exact method this file had
just finished criticising.

It came from grepping which of its functions the arch layer names. Only
`vibeos_kmain` matched, and the conclusion drawn was that the other seventeen
were dead. But `vibeos_kmain` *calls* them. The question "who does the arch
layer name" answers reachability only for the first hop.

A transitive closure seeded from `vibeos_kmain` alone reaches **16 of the 18**.
`kmain.c` holds the kernel CLI - the `vibeos>` prompt, `status`, `meminfo`,
`log` - the boot log, and the boot-failure path. All of it runs.

Acting on the earlier claim would have deleted the CLI.

## What was actually unreached: two functions, 47 lines of 915

**`vibeos_kernel_dispatch_trap`** wrapped `vibeos_trap_dispatch_ex` and added a
`KILL_CURRENT -> vibeos_proc_terminate` step. The arch layer calls
`vibeos_trap_dispatch_ex` directly and kills through
`hw_fault_kill_current_user`, so this duplicated live functionality and no fault
on the machine could reach it. It is also where the third review's process-slot
finding lived.

**`vibeos_kernel_boot_health`** was an accessor for `boot_health_flags`. Checked
before removing it, because the CLI prints `health=` on every boot and breaking
that line would have been a silent regression in something visible: the CLI
reads the field directly at `kernel_cli_print_status`, not through the accessor.

Their tests went with them - one whole test function that drove a path no fault
can take, and one call replaced by the field it was reading.

## The point, which is not the 47 lines

621 lines came out of the first increment and 47 out of this one, and the
difference between them is entirely in the analysis. The same file looked like
"834 lines of dead code" under the weak question and is "two functions" under
the right one.

That is the argument for the transitive walk recorded in
`check-reachable.py` as its next improvement, stated more precisely than it was
there: the weak question is not merely *incomplete*, it errs in **both**
directions. It missed `vm.c` and `interrupts.c` entirely - two subsystems built
on every boot and consulted by nobody - and it would have condemned a live
command-line interface.

# Third increment: syscall.c, and a capability model that was never there

`kernel/core/syscall.c` (1502 lines) and `kernel/core/syscall_policy.c` (23) are
gone, with `test_syscalls` (687), four functions in `user/lib/user_api.c` (52)
and 18 lines of surgical edits elsewhere. 2282 lines.

## Why this one needed a decision rather than a grep

The first two increments removed code that was *unreachable*. This one is
different: C3 asks whether the portable syscall layer should be **deleted or
integrated**, and the rule for answering is which choice produces the smaller
diff in C4 - where the Linux ABI stops being 41% of `arch_hw.c`.

So the measurement came first, and it went against integrating:

| | |
|---|---|
| `kernel/core/syscall.c` | a `switch`, dispatching on VibeOS syscall numbers |
| `arch_hw.c`'s Linux table | a `switch`, dispatching on Linux syscall numbers |

Two switches. C4 wants a **table** - a syscall id mapped to the checks that
apply to it - and neither of these is one. Integrating would have meant carrying
1502 lines of the wrong shape into the change that is supposed to fix the shape.

## The 23 lines that were worth keeping, and are kept as prose

`syscall_policy.c` *was* the right shape: `vibeos_syscall_policy_for(id)`
returning the checks a syscall requires. It is now written into
`docs/core/architecture.md` as C4's starting form, because a 23-line file
consulted by nobody is worth less than a paragraph read by whoever does C4.

That is the trade this increment makes explicit: **a design does not have to be
compiled to survive.**

## What the removal established, which is the actual finding

Chasing the callers of `syscall_policy` produced a fact worth more than the
2282 lines: `vibeos_sec_*` appears **zero times in the arch layer**.

The kernel that boots has **no capability model at all**. The one in
`kernel/core/` was a complete, correct, tested model that nothing on the machine
consulted - the project's most-repeated defect, again, at the scale of a
security subsystem.

C4 is therefore not "move the policy layer down". It is "there is no policy
layer on the metal; write one". Deleting the file is what made that sentence
sayable.

## Two failures on the way out, both of the same family

The tests went red, and both reasons were more interesting than the removal.

**A capability flag outlived its implementation.**
`vibeos_user_api_capabilities()` still reported `supports_boot_event_signal`,
`supports_process_security_label`, `supports_process_interaction_check` and
`supports_policy_summary` as 1 after the functions behind them were removed.
This is *"configured and consulted by nobody"* inverted: not a mechanism nothing
reads, but a **declaration nothing implements** - and a capability flag is read
precisely to decide whether a function exists.

The assertion was **inverted, not deleted**: it now fails if any of the four
comes back non-zero. A removed feature that still advertises itself is a defect
whether or not anything currently believes it.

**An assertion on the effect of a removed function.**
`test_user_api_and_bootloader` asserted `vibeos_event_is_signaled(&kernel.boot_event)`
- the effect of `vibeos_user_signal_boot_event`, which had just gone. Nothing
signals that event now, so the assertion was not lost coverage; it was an
assertion about a function that no longer exists. Removed.

## What is left

| | lines | |
|---|---:|---|
| `kernel/proc/process.c` | 1142 | **deliberately not touched** |

`process.c` is the subject of C5 - "one owner of what a task is". Deleting it
now would decide C5 sideways, in a dead-code commit, without the comparison C5
exists to make. The choice between it and `hw_task_*` is the phase, not a
cleanup.

## And the removal was reported finished twice before it was

Both times the evidence was a binary rather than the tree.

The stale assertion above was described as removed while it was still on line
884. What appeared to confirm that was running `build-gcc-Release/vibeos_kernel_tests`
by hand: it printed `ALL_TESTS_PASS`, and it was **built from an older tree** -
the verification script builds clang only, so the gcc binary beside it had never
seen the change. Third instance here of *"a green build is not a build"*, and
the first where the stale artefact was reached for deliberately, as a
second opinion.

The wrapper made it easy. It printed `host-tests=FAIL kernel=1` on line 11 and
`[exited with code 0]` on line 18, because the script's own exit status was the
last `echo`. A summary whose verdict contradicts its exit code is a summary
nobody reads carefully twice. It now checks every line it prints, ends with
`VERDICT=green|RED`, and exits non-zero - the same rule `check.sh` already
follows with `rc=`, applied one level up.
