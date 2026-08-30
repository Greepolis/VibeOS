# Diagnostics and Observability Progress

Status: In Progress (six runtime detectors, all gated; they found and closed the premature-free defect that had been chased three times from the far end)
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

**Found, over three runs:** eight multi-part kernel messages assembled without
bracketing - `[EXEC]` three times, then `[SIG] deliver`, `[SIG] tkill`,
`[SIG] tgkill`, `[SCHED] fork lost its slot` and the unimplemented-syscall
notice. `serial_puts` and `serial_print_hex` each take the lock on their own, so
an unbracketed six-part line is six critical sections. All eight fixed; the
check kept finding the next one each time the previous was fixed, which is what
a working detector looks like.

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

### Panic records

A kernel panic prints which core noticed, and then one line per online core:
its task, pid, state, `cr3`, and the executable that task was running.

A ring-3 fault has had a full crash record for a while; a panic had a backtrace
of the single core that noticed and nothing at all about the other three. Every
wedge investigated in this project needed exactly the missing part - which core
was on which task, and on whose address space - and it had to be dug out of
QEMU's monitor from the host afterwards, when it could have been printed at the
time for the cost of a loop.

The per-CPU blocks are read directly rather than stopping the cores with an
IPI. This is a machine that is about to halt, and a round trip is the kind of
thing that hangs instead of reporting; a slightly stale line is worth more than
no line.

Verified against a real panic, not a synthetic one: removing the branch that
kills a faulting ring-3 task turns `svc-crash` back into a machine halt, and
the record then names it -

    FATAL: unrecoverable CPU exception, halting on cpu 0x2
    [PANIC] cpu=0x2 ... pid=0xa state=0x2 cr3=0x21d5000 exe=/EFI/BOOT/SVC_CRSH.ELF

## What these found

| Detector | Defect | Status |
| --- | --- | --- |
| Log integrity | eight unbracketed multi-part kernel messages | fixed |
| Seeded stress | `munmap` freed copy-on-write pages without consulting the reference count | fixed, but the family is **not closed** - see below |

### The premature free, and what closed it

`hw_sys_munmap` freed the frame behind every user page it unmapped, outright,
with no `frame_ref_dec`. That is correct only for a page nobody else has - and
after a fork, that is the rare case rather than the common one. Unmapping a
copy-on-write page therefore put it straight back on the freelist while another
process was still running from it.

This is the same defect that had already been chased three times from the far
end, each time presenting as something else entirely: a musl program tripping
over its own malloc bins, init printing a pointer where a pid belonged, a forked
child reading back a value it had not written. Each investigation ended in a
plausible-looking garbage pointer and no mechanism.

What closed it was two detectors meeting. The stress run produced the failure on
demand instead of one boot in thirty; the free-page poison told it *what* the
wrong bytes were. The message it printed named the mechanism outright:

    STRESS_FAIL: the child's own copy-on-write page at offset 0:
      found 0x0 expected 0x96 - this is the kernel's free-page poison:
      the page was reclaimed while still mapped here

From there the fix was one call, and 48 local boots passed without a
recurrence.

**It came back.** The nightly of 2026-08-30, running a commit that contained
the fix, reproduced it: seed 170000012, round 18, the same free-page poison in
a child's own copy-on-write page. So `munmap` was one premature-free path and
not the only one, and the claim that the family was closed was made on the
strength of local boots that could not see it. Both statements have been
corrected here rather than left standing.

Freed pages now also record *who freed them*: the freelist link takes the first
word of a reclaimed page and the caller's name goes in the second, so the next
occurrence names the function that released it instead of only proving that
something did.

### Driving VirtualBox from the harness

`scripts/dev/vbox-run.py` imports the shipped appliance, boots it headless with
the serial line to a file, watches for the boot markers, and takes the guest
apart if it stops talking: per-core registers through `debugvm getregisters`,
symbolised with `addr2line` against the kernel ELF, the guest paging mode, and
optionally a full ELF core through `debugvm dumpvmcore` (about a gigabyte).
It removes the VM afterwards unless asked to keep it.

A second hypervisor is not a luxury here. VibeOS could only talk to virtio-blk
for months: the appliances imported, booted, and could not read their own disk,
and nothing caught it because nothing ever ran the artifact by hand. And
VirtualBox inspects a stopped guest better than the QEMU monitor does - a full
guest core is the right answer for a wedge, where pushing megabytes through a
serial port is not.

    python scripts/dev/vbox-run.py            # boot, watch, clean up
    python scripts/dev/vbox-run.py --core     # write a guest core if it hangs
    python scripts/dev/vbox-run.py --keep     # leave the VM registered

Windows-side, because that is where VBoxManage lives; `addr2line` is called
through WSL, because that is where the toolchain that built the kernel is.

### Static analysis

CodeQL runs on every push. Four alerts were open and each was answered on its
merits rather than silenced:

| Alert | Verdict |
| --- | --- |
| Comparison result is always the same (`arch_hw.c`) | real. A negate-if-negative in `kill()` that could never run, because the line above had already forced the value positive. Removed. |
| Declaration hides variable (`hello.c`) | real, and harmless as it stood. The futex test's `word` shadowed an outer declaration; renamed. |
| Local variable address stored in non-local memory (`storage.c`) | real observation about a contract that was nowhere in the code: the block cache must outlive the storage struct. Documented at the assignment; the ownership is still not expressed in the types. |
| Unused static function (`bootloader_core.c:35`) | **not reproduced.** `region_end` has seven callers in that file, and no static function there is unused. Not acted on: deleting a used function to quieten a checker is worse than the alert. Needs the CodeQL run's own log to settle. |

### Crash collector

`scripts/dev/report-crash.py` reads crash records out of a serial log and
collects them into a single GitHub issue, one comment per distinct crash. It is
the "send a report" every desktop OS has, with three rules that keep it useful
rather than noisy:

- **The guest never sends anything.** VibeOS writes a crash record to its
  console and stops there. The reporter runs on a host, on an explicit command,
  on a log somebody chose to hand it. An operating system that uploads on its
  own is a different kind of thing from one with a bug reporter.
- **One issue, never more.** Reports are comments on a collector issue found by
  its exact title. The script will not open a second one.
- **Deduplicated by signature** - the program, the vector, and the faulting
  instruction, deliberately not the pid or the stack contents. A crash that
  recurs for a fortnight edits its own comment, bumping a count and a last-seen
  date, instead of adding fourteen. It also skips the crashes the boot provokes
  on purpose (`svc-crash` faults every single run), and refuses to open more
  than three new reports in one invocation.

Nothing is posted without `--post`. Without it the script prints exactly what
it would send, which is also how to check what a dump contains before it
becomes public: a crash record carries register values and a slice of the
process stack.

    python scripts/dev/report-crash.py qemu-cli-serial.log

Wired into the nightly workflow only, and only on failure, with
`issues: write` declared explicitly on the job - the default token permissions
depend on a repository setting, and a reporter that silently cannot post looks
exactly like one with nothing to report.

### A seed with no entropy is not a random test

The stress service seeded itself from `clock_gettime`, and the seeds it
produced here were 220000010, 230000012, 180000010, 160000010 - the same value
give or take a coarse tick. Every boot therefore explored very nearly the same
sequence, which is the one thing a randomised test must not do. Fifty local
boots stayed green while CI failed five times out of five on sequences this
machine had never tried.

It reads `rdtsc` now, which changes every cycle and is available from ring 3;
the clock and the pid are still mixed in to separate two boots that start on
the same cycle count. Seeds are eleven digits and different every run.

**Still not reproduced locally.** The configurations are identical - same
Release flags, same TLS setting, and it fails under both gcc and clang in CI -
so the difference is the machine. Rather than keep guessing from here, the gate
now prints every `STRESS_FAIL` and `STRESS_SEED` line when the stress run
fails, and the crash collector files stress failures as well as faults. The
environment that can see the defect is the one that gets to describe it.

## Pending

- **`munmap` does not shoot down the other cores' TLBs.** The need is real - a
  thread of the same process elsewhere keeps a translation for an address whose
  frame has just been handed back, so it can write into memory that now belongs
  to something else. It was implemented and then removed: two runs in
  twenty-four failed with `tlb_acks below shootdowns`, a core that never
  answered. `syscall` clears IF, so a target cannot take the IPI until it
  returns to ring 3, and `munmap` is called far more often than `fork` - the
  stress run alone calls it a hundred and twenty times. Trading a rare
  correctness gap for a frequent stall is the wrong trade. The honest fix is for
  the syscall path to stop masking interrupts for its whole duration, which is a
  larger change.
- A hang in the bootloader phase, about one boot in twenty-four, still
  uninvestigated. It is not slowness: a 300-second budget leaves the guest
  silent for 296 of them.
- No host-side memory dump wired into the harness. QEMU's `dump-guest-memory`
  and VirtualBox's `VBoxManage debugvm dumpvmcore` both exist and neither is
  used; a full guest core is the right answer for wedges, and pushing megabytes
  through a serial port is not.
