# C6 - diagnostics become a module

Status, 2026-09-22: **done.** The kernel log and the crash records live in
`kernel/diag`; the UART and the disk are registered sinks; the arch layer keeps
the mechanisms that are different on another machine.

The plan (`docs/core/phases.md`) called this the one move that can invalidate its
own evidence: these two are what every other phase is verified *with*. So the
order of work was the plan's own - verify the inherited tree green first, write the
module and its tests, break the tests, then boot - and three things that moving it
found are recorded below because they were defects, not tidying.

## What was there

In `arch_hw.c`, under a banner that said "the kernel log":

- `g_kernel_log`, a `vibeos_log_t` ring, written by `hw_log` from every core;
- `hw_log_format`, `hw_log_emit` (serial), `hw_log_to_sink` (disk) and its per-core
  reentrancy flags, `hw_log_dump` (panic) and `vibeos_x86_64_log_dump_recent`
  (the console's `log`);
- the crash ring `g_crashes[4]`, `g_crash_next`, `g_crash_count`, written from
  `task_life.c` and printed by `vibeos_x86_64_crash_dump`.

The plan's 724 and 56 lines were measured before C4/C5 had already moved some of
it; what was actually left was about 470 lines of log section plus the crash ring.
`arch_hw.c` is 6,031 lines after this phase (6,231 before it).

## What it is now

`include/vibeos/klog.h`, `kernel/diag/klog.c`:

- **The ring has its own lock** (`vibeos_klog_set_lock`, registered by the arch as
  `hw_spin_lock_named(..., "vibeos_klog")`). The lock covers the ring only; sinks
  are written outside it.
- **A sink is a registration**: a name, the quietest level it wants, an optional
  prefix, whether its lines are numbered and newline-terminated, and
  `write(ctx, line, len)` that takes one complete line and says whether it took it.
  The serial port (`"serial"`, INFO and up, `[LOG]`, numbered) is registered by
  `hw_klog_init`; the disk (`"disk"`, every level) by `hw_logsink_bringup`, and
  only once the medium has taken its boot mark - before that it would refuse every
  line, and every refusal is a must-be-zero.
- **One line, one call.** The module formats the whole line and hands it to each
  sink once; the serial sink passes it to one `serial_puts`, which is one critical
  section on the console lock. The "multi-part message left unbracketed" defect is
  not guarded against here, it is not expressible: there are no parts.
- **A refused line is counted**: per sink (`lost`) and in the registry as
  `klog_line_lost`, which the boot gate asserts through `[MBZ] MUSTBEZERO total`.
- **A line the device lost while saying it had not** is the half no counter inside
  the kernel can see. Every serial line carries ` ln=0x...`, one sequence per sink;
  the `[KLOG]` line reports how many were given out; the gate requires every number
  from one to that count to be in the log (`klog_line_missing`,
  `klog_counters_missing`, `klog_serial_unproven`).
- **Reentrancy** is the module's, per sink and per core: an event raised inside a
  sink's own write (the block layer logs when it refuses a request) is recorded and
  reaches the other sinks, is not offered back to the sink that raised it, and is
  counted as `reentered`.
- `vibeos_klog_panic(why)` writes "[FATAL] PANIC: why" to every sink with no lock
  and no level; `vibeos_klog_dump_unlocked` replays the ring for a panic with no
  lock; `vibeos_klog_dump_recent` is the console's `log`, one event per critical
  section.

`include/vibeos/crash.h`, `kernel/diag/crash.c`: the ring of four, its own lock, a
record copied whole in and out, registers as name/value pairs the architecture
fills, and a portable dump that prints exactly what `crash` printed before. The
capture - reading the trap frame, and the user stack through `vibeos_uaccess_copy`
- stays in `task_life.c`, because that is what a trap frame is.

What stayed in the arch layer, deliberately: the UART itself, the frame-pointer
walk (`hw_backtrace`), parking every core on a panic, the raw deadlock reporter
that must not take the console lock, and the `[HW][TRAP]` line.

## What moving it found

**1. The ring was never locked.** `hw_log` called `vibeos_log_record` from every
core with no lock, then asked the ring for `vibeos_log_latest` to decide what to
print. With two cores logging at once, "the latest" is whichever landed second: one
line printed twice, the other never, and both could claim the same slot. The event
printed now is the copy taken inside the same critical section as the record.
Same shape as `backing.c` in CLAUDE.md - a layer serialised by accident - and the
fifth layer in this project to need its own lock.

**2. The crash ring was not locked either.** The recorder read the cursor, filled
the slot and advanced the cursor as three steps; two processes faulting on two cores
(which is what a broken library does to everything linked against it) wrote one
record over the other and advanced past a slot nobody filled.

**3. The ring held a false FATAL every boot.** The trap model was handed the ring
directly, so it wrote behind the lock and past every sink - and, asked with pid 0
and knowing nothing of privilege, it recorded `kernel_fault_panic` at FATAL for
*every* ring-3 fault, including svc-crash's deliberate one on every boot. A panic
dump would have shown a FATAL for a fault the machine had survived. The model is
now given no log; the arch logs `kernel_fault_panic` itself, only on the path that
actually panics, with the same code (`TTFK`) and `a1 = rip` that the gate's
poison-executed detector reads.

And one the new torture found in the new code on its first run: the panic path had
its own copy of the line builder, and that copy dropped the newline when a line had
to be cut. Unreachable with today's `[LOG]` prefix, and still two copies of one
rule already disagreeing. There is one builder now.

## How it is proved

- **Host tests** (`tests/kernel/klog_tests.c`, `crash_tests.c`): one write per
  event per sink with the exact text; the ring locked exactly once per event and a
  sink never written under it; a second core simulated at the unlock, so "print the
  latest" is caught deterministically; levels; refusals counted with the witness
  naming the sink; reentrancy; truncation keeping its newline; both dumps; panic;
  a full table; lock balance across the whole test. Crash: the exact dump, ring of
  four, clamping of a record that lies about its sizes, one critical section per
  record.
- **Torture** (`tests/kernel/diag_torture.c`, nightly job `diagnostics-torture`,
  `# module: diag`): random sinks, levels, refusals, nested reentry, a simulated
  second core, panics, dumps and crash records, every line checked against a model
  that formats with `snprintf` and shares no code with the module; long runs must
  wrap the 2048-event ring (the first version never did - a reset every hundred
  rounds kept it under capacity, and the torture now fails a long run that did not
  wrap).
- **Sabotage**, all red *by name*:
  - `diag-klog.txt` (8 cases: an unmatched unlock; a line in two writes; a refusal
    ignored; the ring unlocked; "latest" read after the unlock; sinks written under
    the lock; no reentrancy guard; one line counter shared by all sinks) - red
    under the host tests **and** under the torture alone.
  - `diag-crash.txt` (4 cases) - red under both. The register-count case first went
    red under the torture as a segfault with no message: the model had named it, and
    the buffered line died with the process. The torture flushes on failure and
    stops dumping a record it already knows is wrong.
  - `diag-sink.txt` (4 boot cases: the serial sink drops its fifth line and
    reports success; the `[KLOG]` line disappears; the sink is offered nothing;
    the sink logs from inside its own write) and `crash-recorder.txt`
    (re-pointed: its first case targeted the arch's own ring, which no longer
    exists) - red under the boot gate. The dropped-line case first dropped one
    line in sixteen and went **NOT RED**: a boot hands the serial sink about a
    dozen live lines, and the sixteenth call was a replay from the console's
    `crash`/`log` dump, which carries no `ln=`. That is the detector's real
    limit and is recorded in the case file rather than tuned away: `ln=` covers
    live lines; a lost replay line is seen only by the markers the gate waits
    for on those commands.
- `check-subsystem.py` covers `kernel/diag` (`diag/klog` reports through the
  registry; `diag/crash` is exempt with its reason, the exemption count raised from
  23 to 24 as a decision).

The plan names the file `core-diag.txt`; `check-subsystem.py` pairs a module with
its cases by area, so they are `diag-klog.txt`, `diag-crash.txt` and
`diag-sink.txt`.

## Open, and recorded rather than forgotten

- **The reentrancy guard is per core, and a task can move.** Between the module
  setting a sink's busy flag and the sink's write, interrupts are on (the ring's
  lock is already released), so a task preempted there and resumed on another core
  leaves the flag set on the core it left; that core's lines to that sink are then
  counted as `reentered` and not written. The old disk guard had the same shape.
  Masking interrupts across a disk write is not the fix - a core with IF clear
  cannot answer a TLB shootdown (CLAUDE.md). What was done instead is to make it
  visible: the serial sink never logs from inside itself, so its `reentered` must
  be zero and the gate asserts it (`klog_serial_reentered`). If it ever fires,
  this is the paragraph to read.

- **There are still two logs.** `kmain` keeps `vibeos_kernel_t.log` for its eight
  boot stages; everything the machine does is in the klog ring. The console's `log`
  prints kmain's last five and then the klog ring's last 24; `status` counts only
  kmain's. Folding kmain's into
  klog is the right end state and belongs with the ordering problem CLAUDE.md
  describes (the portable kernel is initialised after the machine has done its
  work) rather than with this move.
- The panic line reaches every sink, so the serial log now carries
  `[LOG][FATAL] PANIC: <why>` just before the existing ` FATAL: <why>, halting`
  line. The gate reads the second; the first is what the disk has always received.
