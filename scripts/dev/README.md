# Development Scripts

Tools for working on VibeOS, as opposed to building or gating it. The gates
live in `scripts/` and in `.github/workflows/`; these are for the loop between
"change something" and "know whether it worked".

| Script | Use |
| --- | --- |
| `check.sh [build\|tests\|smoke\|all] [dir]` | Build, run the host suites, boot. Prints the return code, the count of real warnings, the test verdict, and every ring-3 self-check line the guest produced. |
| `repeat-boot.sh [dir] [n]` | Boot `n` times and report each verdict. |
| `trace-linux-binary.sh <binary> [args]` | Trace a real Linux program with `strace` and list which of its syscalls VibeOS does not serve yet. |
| `catch-hang.py [dir] [attempts]` | Boot until it hangs, then ask the QEMU monitor where every core is, with symbols. |

## Why each exists

**`repeat-boot.sh`** — one clean boot proves very little. A lost TLS base, two
CPUs running one task, and a console deadlock each reproduced in roughly one
boot out of three. A single green run and a broken kernel look identical.

**`trace-linux-binary.sh`** — this is the method behind every syscall in the
kernel's Linux layer. The set a program needs is a property of its libc and of
what it does, not of the kernel, so it cannot be derived by reasoning about
the kernel. Guessing produces stubs nobody calls sitting next to gaps that
stop everything. Running the binary on Linux under `strace` produces the truth
in one command.

**`catch-hang.py`** — a hung guest has stopped producing serial output, which
is this system's only diagnostic channel. `info registers -a` on the QEMU
monitor dumps every core's state, and resolving each RIP through `nm` turns
"it froze" into a function name. Two intermittent hangs were diagnosed this
way that no amount of added logging would have found, because the stuck code
held the lock the logger needed.

Note that it forces `-accel tcg`. Verifying under KVM while CI runs pure
emulation once produced about twenty clean local runs against a hang CI was
hitting two times in three - local verification and CI must exercise the same
configuration or the local result means nothing.

## Verifying a fix

Green is a claim. Before believing one:

1. Break the thing the test covers and watch it go red. A test that has never
   failed has not been shown to work.
2. Check the effect, not the return code. A syscall reporting a success it did
   not perform is worse than one that fails.
3. If the bug was intermittent, repeat. `repeat-boot.sh` exists for this.

The kernel sources contain several bugs found exactly this way, and at least
one wrong diagnosis that was only caught because the evidence was checked
against the hypothesis rather than the other way round.
