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
