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
| `kernel/core/kmain.c` | 914 | trim: `vibeos_kmain` is live, the other 17 functions are not |

`kernel_tests.c` is 9,028 lines and still holds roughly 330 references to that
half — most of the largest test file exercises the kernel that does not run,
which is part of why it is always green.

C3 says the choice between deleting and integrating is made by asking which
produces the smaller diff in C4. The measurement for that now exists.
