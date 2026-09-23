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
- **Reentrancy** is the module's, for the sinks that ask for it and keyed by
  execution context (see "The limits it shipped with", below): an event raised inside a
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

## The limits it shipped with, closed (2026-09-23)

The first version of this file listed three limits as open. All three are closed;
closing them found one more defect.

- **The reentrancy guard was per core, and a task can move.** Between the module
  setting a sink's busy flag and the sink's write, interrupts are on, and kernel
  tasks are preempted and migrated; a task moved there left the flag set on the
  core it had left, and every other context on that core had its lines to that
  sink skipped as `reentered`. Masking interrupts across a disk write was not an
  option - a core with IF clear cannot answer a TLB shootdown. Two changes instead:
  the guard is **opt-in per sink** (`guard_reentry`: on for the disk, whose block
  layer logs; off for the serial port, which cannot recurse - so nothing can ever
  skip a serial line), and it is **keyed by execution context**
  (`vibeos_klog_set_context`): the arch answers with the running task's slot, read
  through the per-CPU block only once GS.base is installed, and one number per core
  before that. A migrated task takes its flag with it; an interrupt nested in a
  task is that task, so a nested write is still caught. The gate still asserts the
  serial sink's `reentered` is zero.
- **Replays had no detector.** `ln=` covers live lines only. Each replay now states
  its length - `log` and the panic dump already did in their header; `crash` now
  ends `[CRASH] end lines=0x..` - and the gate's `replay_incomplete` counts the
  lines against it (`diag-sink.txt` drops one line of the `crash` dump: red as
  `replay_incomplete=crash_dump`).
- **There were two logs.** kmain kept a ring of its own for its boot stages, so
  `status` counted one log, `log` printed both, and a panic dump never showed a
  boot stage. kmain now logs into klog, which on the machine the architecture has
  already created (`entry.s` runs `hw_early_init`, whose "log" stage creates it,
  before `vibeos_kmain`); kmain only creates it when it is missing, which is the
  host tests. `vibeos_kernel_t.log` is gone. The host boot test logs one event
  "as the architecture" first and fails if kmain throws it away
  (`diag-kmain.txt`).

**Found while closing them: the console's `log` showed the wrong events under
load.** `dump_recent` fixed what to show as a range of ring *indices* and re-read
them one lock at a time. With the ring full - every boot past 2,048 events - each
event another core logged during the dump pushed the oldest out and moved every
index by one, so the dump showed a neighbour instead: the same event twice, one
skipped, nothing said. It now fixes a range of sequence numbers under one lock and
fetches each by number; an event overwritten before its turn prints as
`[LOG] #<seq> overwritten before it could be shown`, so the line count is still the
one the header announced. A host test fills the ring and logs during the dump; the
torture does the same with bursts large enough to overwrite announced events, and
requires a long run to have done so.

## Code scanning

The nightly torture's first version built its expected lines with
`n += snprintf(buf + n, cap - n, ...)`, three alerts at high severity. The analysis
was right about the mechanism: snprintf returns the length it *would* have written,
so after one truncation `n` passes `cap` and the next `cap - n` wraps to an
enormous size_t. No expected line came near its buffer, so it never fired - and
nothing stopped the next longer dump from overwriting the torture's stack. One
bounded `app()` helper replaces every such call, and a model text that does not
fit is a test failure rather than a silent cut.

Triaged in the same pass: the ABI registry test registered stack arrays with a
registry that keeps the pointer (the tables are `static` now, and `abi.h` states
the lifetime contract); `ntfs.c` named `$INDEX_ALLOCATION`'s length `attr_len`
inside a function whose outer `attr_len` is `$INDEX_ROOT`'s - renamed
`alloc_len`, because in a parser that is how one attribute's bound gets checked
against the other's.

## Still open

- The panic line reaches every sink, so the serial log carries
  `[LOG][FATAL] PANIC: <why>` just before the existing ` FATAL: <why>, halting`
  line. The gate reads the second; the first is what the disk has always received.
  Recorded, not a defect.
