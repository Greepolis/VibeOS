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

The table above was the whole list for a long time while the directory grew to
fifty scripts, which is how a tool gets written twice. The rest, by what they
are for:

**Repeating a boot, and keeping the evidence**

| Script | Use |
| --- | --- |
| `hunt-boot.sh <dir> <n> <regex> [--stop]` | Boot until a signature appears; keep every log that matches *and* every boot that failed, under `.boot-evidence/hunt/`. The general form of the one-off hunts (`hunt-argv.sh` is one). |
| `boots.sh [dir] [n] [parallel]` | Several boots at once. |
| `until-wedge.sh [dir] [n]` | Boot until one wedges and print what it said last. |
| `bisect-boot.sh <rev> [n] [dir]` | Build another revision, boot it n times, come back. The baseline a criterion has to be checked against. |
| `soak.sh`, `soak-report.py` | The memory soak, and the one definition of whether it passed. |

**When something went wrong**

| Script | Use |
| --- | --- |
| `symbolize.py [dir] < serial.log` | Kernel backtraces to functions and lines, with addr2line. |
| `symbolize-user.sh <binary> <rip> [va] [dirs]` | A ring-3 address named against the guest binary that was running it - every Linux program links at 0x400000, so an address alone names nothing. |
| `wedge-now.sh [out] [dir]` | Ask a guest that is stuck *right now* (under the boot gate in another terminal) what every core is doing, before whatever drives it gives up. |
| `wedge_report.py <sock> <kernel>` | The monitor-side report the gate runs on every wedge; the two above call it. |
| `report-crash.py` | Crashes in a serial log gathered into one issue. |
| `screenshot.py [dir] [out] [s]` | The screen, since the GUI's pixels are not in any log. |

**Verifiers for `sabotage.py`** (`SABOTAGE_VERIFY=...`; without one it runs the host suite)

| Script | Scores a case by |
| --- | --- |
| `verify-boot.sh [dir]` | Building and booting; prints the gate's own reason. |
| `verify-host.sh [dir]` | The host suite and short runs of the memory-manager and GUI tortures. |
| `verify-gui-torture.sh [dir]` | The GUI torture on three seeds, threads included. |
| `verify-host-sanitized.sh` | The CI "clang Debug" job as CI runs it - ASan and UBSan, its own build directory. For what only a sanitizer sees. |

`sabotage.py` refuses to start while a `<source>.sabotage-backup` exists: that
file means an earlier run stopped with a case applied, the source holds the
sabotage, and carrying on would overwrite the only good copy with it.

**Other**

`vbox-run.py` and `check-vm-images.py` (the shipped appliance, on VirtualBox and
as an importable file), `mm-torture.sh` (many seeds), `post-finding.sh` and
`make-book-summary.py` (the docs), and the `check-*.py`/`check-*.sh` family,
which `check.sh` runs and each of which says in its own header what it guards.

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
