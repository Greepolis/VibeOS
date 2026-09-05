# Maintainability — the rules, and what enforces them

A rule nothing checks is a preference. This project already has the counterpart
in `scripts/dev/check-exec-layering.sh`, which enforces that exactly one
function may name an interpreter path — and it exists because that rule was
broken twice by people who agreed with it.

## The structural rules

**R1 — one place reaches the disk.** No file outside `kernel/io/` calls a
driver. Today `fat.c` calls `vibeos_x86_64_blk_read` directly, which is how the
only filesystem that runs came to bypass the only cache there is.

*Enforced by* `check-io-layering.sh`: grep for driver entry points outside
`kernel/io/`, fail on any hit.

**R2 — one cache below the file layer.** A filesystem does not keep its own
sector buffer. `fat.c`'s two static buffers are the current violation.

*Enforced by* the same script: a static array of sector size in a filesystem
source is a finding, listed with an allow-list that must be empty by the end of
I2.

**R3 — a filesystem knows nothing about partitions, and a volume knows nothing
about filesystems.** A filesystem is handed a volume and reads from offset
zero of it. The volume layer never parses a boot sector; it asks each
filesystem's `probe`.

*Enforced by* review and by the shape of the ops table — a `probe` that takes a
volume rather than a device cannot look outside it.

**R4 — every error carries a reason.** No function in this subsystem returns a
bare non-zero for a failure that has more than one cause.

*Enforced by* the sabotage cases: each phase has one that reports the wrong
reason, and it must turn a test red.

**R5 — every extension point is a table, never a weak symbol.** The PE/COFF
lesson: a weak definition in a different object from its caller resolves on
Linux and fails on Windows, and the Linux build says nothing.

*Enforced by* the Windows CI job, which already exists.

**R6 — every wait is bounded and the bound is counted.** Including
`max_wait_iterations`, so a bound that is one retry from firing is visible
before it fires rather than after.

## How each phase is verified

The same ladder the memory-manager phases used, and in this order:

1. **Host tests**, against a fake device. A layer that cannot be host-tested
   has its boundary in the wrong place — that is the test of a boundary, not a
   consequence of one.
2. **A sabotage case file** under `scripts/dev/cases/`. Every case applied, the
   tests confirmed red, and *the tree confirmed green again afterwards*. A
   sabotage run that starts on a tree which does not pass proves nothing: eight
   cases once "passed" here having executed nothing, because the verify script
   lived in `/tmp` and WSL had cleaned it.
3. **A boot-gate assertion**, where the phase reaches the running machine.
4. **`repeat-boot.sh`**, at 24 for I2 and at 8 elsewhere. One clean boot proves
   nothing; this machine has a background failure rate of roughly one boot in
   eight, and a criterion has to be checked against the baseline before it is
   used to judge a change.

## What a test must not be

Three failure modes, all of which have happened here, and all of which look
like a passing test:

**A test that asserts a condition it never reaches.** Two NTFS cases once
passed vacuously; a swap-map pool was sized by guessing the node at 32 bytes
when it is 24, so the exhaustion case never exhausted anything.

**A test that is right about the outcome and wrong about the mechanism.** Three
times in the memory-manager work a sabotage walked through a green test,
because the test asserted the correct property about an arrangement in which
the defect could not show. Only breaking the code on purpose separates them.

**A test arranged differently from the code it exercises.** The compaction
tests held an allocation reference the kernel hands over, so every move was
correctly refused and the tests were wrong rather than the layer.

## Definition of done, per phase

- Host tests pass, and each has been seen to fail.
- The sabotage file exists, every case has been applied and confirmed red, and
  any case that *cannot* be turned red here is recorded as unverifiable rather
  than counted — the distinction the AHCI cases already carry.
- `check.sh all` green on gcc **and** clang. gcc accepts an implicit
  declaration that clang rejects; that has broken CI here before.
- The docs updated: a row in `docs/implementation_progress.md`, a detail file,
  and `make-book-summary.py` re-run so it reaches `SUMMARY.md`.
- What the phase did *not* do is written down by name, not implied.
