# Diagnostics and Observability Progress

Status: In Progress (six runtime detectors, all gated; two found defects on their first run)
Last review: 2026-08-29

Everything here exists because of a specific bug that cost hours, not because
it is good practice in general. That is the selection rule: a detector earns a
place by being able to name a failure this project has actually had.

The recurring shape of a hard bug in this kernel is that it surfaces a long way
from its cause - in a different program, minutes later, as a pointer that makes
no sense. Every item below shortens that distance.

## Implemented

### Poisoned freed pages

`hw_free_page` fills the page with `0xDEAD0000DEAD0000` before putting it on
the freelist, and `hw_alloc_page` samples the poison when handing a recycled
page back out.

A freed page used to keep its old contents, so a use-after-free read *plausible*
data and the program carried on until something unrelated broke. The pattern is
non-canonical as a pointer, so the first dereference faults instead of the tenth
one corrupting something. The re-check on allocation reports a write to an
already-free page at the moment it is discovered.

Gate: `use_after_free_detected`.

### Console-lock hygiene

`vibeos_x86_64_serial_unlock` refuses an unlock from a core that does not hold
the lock, and counts the attempt; `bad_unlocks` is printed with the memory
statistics and the boot fails if it is not zero.

One missing `serial_lock()` in `hw_log_emit` freed the lock out from under
another core mid-line. The markers the boot gate matches on were cut in half
and the gate reported failures that had never happened - two crashes were
investigated in full before the split was noticed.

Gate: `console_unlock_by_non_owner`.

### Log-integrity check

`interleaved_lines()` in the boot gate runs **before** any other assertion,
because every other assertion reads the same text: a split line can invent a
failure or hide a real one.

It reports a line when a hex field is cut short (`0x` with no digits and a tag
following - the kernel always prints its values in full) or when a second tag
follows a kernel tag. A line opened by a ring-3 write is exempt: a program may
write without a trailing newline, and the first version of this check called
that a defect and cried wolf on its first run.

It carries self-tests against real lines from real boots, including one that
must *not* be flagged. Known limit, in the docstring rather than left to be
rediscovered: a split inside a ring-3 write that truncates no number is not
caught here - the bad-unlock counter guards that case.

**Found on its first run:** four multi-part kernel messages assembled without
bracketing (`[EXEC]` three times, `[SIG] deliver` once). `serial_puts` and
`serial_print_hex` each take the lock on their own, so an unbracketed six-part
line is six critical sections. All four fixed.

Gate: `serial_log_interleaved(N_lines)`.

### The kernel's own complaints are now assertions

Any `[LOG][WARN]`, `[LOG][ERROR]` or `[LOG][FATAL]` line that is not on a short
allowlist of deliberately provoked ones fails the boot.

The kernel has had levelled logging for a long time and not one of its warnings
had ever failed a build. This is the same lesson as the ring-3 self-test that
printed `abi: ...wrong` for a whole session while the gate stayed green.

The allowlist is deliberately short and specific. Adding a pattern to quieten a
real warning is how this mechanism stops working.

Gate: `kernel_reported_problems(N)`.

### Crash records

A ring-3 fault is captured at the moment it happens - registers, vector, error
code, fault address, the executable's path, and as much of the user stack as is
readable - into a ring of four. `crash` on the kernel console prints the last
one in full.

A faulting process used to leave two numbers behind, `rip` and `cr2`, printed
once and gone. Every hard bug here was then diagnosed by going back for the
registers, the stack and which binary the task was running - by which time the
process no longer exists. The record names the executable because an address
alone identifies nothing when every Linux program links at `0x400000`; that
omission produced three wrong diagnoses of a single address.

The gate drives `crash` every boot, because `svc-crash` faults every boot: a
dumper exercised only when nothing has crashed proves nothing.

Gate: `fault_was_not_recorded`, `crash_record_does_not_name_the_program`,
`crash_dump_found_no_record`.

### Seeded stress service

`svc-stress` (`user/prog/svc_stress.c`, staged as `SVC_STRS.ELF`) runs 120
rounds of randomised fork, anonymous mmap, copy-on-write and pipe work, in a
different order each boot, checking the result of every operation. The seed is
printed on its first line, so a failure replays:
`EFI/BOOT/SVC_STRS.ELF <seed>`.

The boot otherwise runs one fixed script, which produces the same handful of
interleavings every time and can only find a race by luck - which is how the
last three were found, one boot in thirty, after a lot of waiting.

Its failures name what they found rather than that something was wrong, and
they recognise the free-page poison: "the page was reclaimed while still mapped
here" is a different investigation from "the data was wrong".

Gate: `stress_seed_not_reported`, `stress_run_did_not_finish`,
`stress_run_found_a_defect`.

## What these found

| Detector | Defect | Status |
| --- | --- | --- |
| Log integrity | four unbracketed multi-part kernel messages | fixed |
| Seeded stress | a page freed while a forked child still had it mapped | **open**, see below |

## Pending

- **A page reclaimed while still mapped, about one boot in twenty.** The stress
  run says so outright now: seed 230000012, round 58, the child's own
  copy-on-write page reads back as the kernel's free-page poison. This is the
  same family as the musl-heap and `fork`-return corruptions chased earlier -
  premature free - but it is the first time the evidence names the mechanism
  instead of leaving it to be inferred from a garbage pointer.
- A hang in the bootloader phase, about one boot in twenty-four, still
  uninvestigated. It is not slowness: a 300-second budget leaves the guest
  silent for 296 of them.
- No panic record: a kernel panic still prints a backtrace and stops, without
  the per-core register state a crash record gives for ring 3.
- No host-side memory dump wired into the harness. QEMU's `dump-guest-memory`
  and VirtualBox's `VBoxManage debugvm dumpvmcore` both exist and neither is
  used; a full guest core is the right answer for wedges, and pushing megabytes
  through a serial port is not.
- No crash reporter. The intent is a single fixed GitHub issue as a collector,
  deduplicated by signature (`exe + vector + rip`) so a recurring crash updates
  a count rather than filling the thread, posted by host-side tooling on an
  explicit command - never by the guest itself.
