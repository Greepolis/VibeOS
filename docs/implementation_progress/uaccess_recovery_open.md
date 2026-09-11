# The user-access recovery that never worked, and the probe that said so

**Status: OPEN.** Reverted, not shipped. This file exists because the diagnosis
is worth more than the attempt.

## The defect it was meant to close (still open)

An external review found it on the futex path and was right. Validating a user
pointer and dereferencing it are two instants, and nothing makes the pair
atomic: `hw_user_range_ok` walks the page tables, and a sibling thread on
another core can munmap the address before the read happens.

`hw_futex_wait` is the sharpest example - it validates, takes `g_futex_lock`,
which is not the VM's lock and can **spin**, widening the window as far as an
attacker likes, and only then reads. A ring-0 fault panics, so an unprivileged
multi-threaded process can stop the machine by racing against its own memory.

## The attempt

An exception table with one entry per core: `hw_user_read32` records a recovery
point in `hw_cpu_t.uaccess_recover` using the GNU address-of-label extension,
and the trap handler rewrites `frame->rip` to it when a ring-0 page fault
arrives with one armed - the shape `fixup_exception` has elsewhere.

## Why it does not work here, exactly

The recovered `rip` was **`0x000000aa020`**, and the kernel is linked at
`0x4000000`. `0x4000000 + 0xaa020` is a valid text address. The stored value had
**lost its base**: `&&label` produced a section-relative offset rather than an
absolute address under this freestanding link, so the handler resumed at an
unmapped low address and the machine took a `#UD`.

That is not patchable by adding the base - a constant that has to match the
linker script is a silent breakage waiting for the next layout change. The
correct implementation is a real exception table: an assembly stub whose
recovery address is emitted with a proper relocation, and a section the handler
searches. That is the shape to build when this is taken up.

## The part worth keeping: three green boots proved nothing

Before the probe existed, this mechanism measured **3/3 green** on the boot
gate. It had never once been exercised, because the only caller was the futex
path and the futex word is always mapped there - the recovery arm was dead code
that happened to compile.

The probe - read an address in the user window that nothing ever maps, from
ring 0, every boot - turned that into a `#UD` in one run.

It also had to be moved before it could say anything useful. The first version
ran from `vibeos_kmain`, inside the assembly of a report line, and the machine
went **silent**: 551 lines of log, no trap dump, no panic. `kmain` runs after
every user task has exited, so the CR3 loaded there can belong to a torn-down
address space, and a core faulting with no kernel mapped to fault from produces
exactly that silence - the shape CLAUDE.md already describes. Moving the probe
into bring-up, where the kernel's own address space is loaded, turned the
silence into the `#UD` that named the real defect.

## What this cost, and the rule it earns

An afternoon of bisection, because the symptom was "userland dies at line 200 of
1107" and the cause was a probe that runs after userland. Two hypotheses were
eliminated by reading rather than by booting - the brk change (the only one on a
normal path, and wrong) and the per-CPU struct offsets used by `isr.S` at
`%gs:0` and `%gs:8` (my fields are after them) - and the decisive step was the
one that should have come first: **boot HEAD and check it is still green.** It
was, 3/3, which turned "something is wrong" into "something of mine is wrong"
in four minutes.
