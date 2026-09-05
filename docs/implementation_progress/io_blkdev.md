# I0 and I1 — the block layer

## What was there

Three functions with three shapes — `read(lba, buf)`, `read_many(lba, buf, n)`
and `write(lba, buf)` — bound to one global device, each returning a bare
`int`. The asymmetry was the informative part: there is no `write_many`, and
nobody had noticed.

## I0 — the contract

A request is a value now, with a result that names the reason. Six of them, so
a timeout, an absent device, a medium error, a short transfer, a malformed
request and a range past the end of the disk are six different things instead
of one `-1`.

This project has already paid for the old shape once. `fat_next_cluster`
reported a failed table read as the end-of-chain marker — the same value a
healthy last cluster returns — so a flaky sector produced a short file that
said it was complete, and `execve` parsed whatever the previous program had
left in the shared buffer.

The layer is three things: validate, dispatch, and **compare what came back
against what was asked for**. Only the third is new, and it is the one the
above needed: a driver that moves four sectors of eight and returns success is
reporting a success that lost half the data.

**The bounds check lives here, not in the driver.** A driver that checks its
own bounds protects the *device*; it cannot protect a neighbouring partition,
whose sectors are perfectly valid addresses on that device. The same argument
the swap area makes about the filesystem, one layer down.

### A defect the sabotage found in the layer itself

The first version indexed the statistics array with the driver's result
directly. A driver is not trusted to return a value from that enum: an
out-of-range result wrote past the end of the array, and the suite *segfaulted*
instead of failing an assertion. `count_result()` bounds it now, and counts an
undefined value as `BAD_REQUEST` rather than dropping it — a driver returning
something nobody defined is itself worth knowing about.

The same case exposed a test that could not reach what it claimed. Every
refusal path sets a reason explicitly, so removing the reset at the top of
`submit` changed nothing the tests looked at. The path that needs it is a
driver failing *silently*, where the layer decides "did the driver say why?" by
asking whether the result is still `NOT_ISSUED` — and a caller's stale reason
would otherwise be attributed to this request.

## I1 — the layer becomes load-bearing

`blk.c` is an adapter now: everything above still calls
`vibeos_x86_64_blk_read`, and underneath that is a request through the new
layer.

An adapter rather than a rewrite of the callers, deliberately. `fat.c` is how
this machine boots and has nine call sites; changing the layer beneath it and
its own calls in one step would mean a failure could be attributed to either.
The callers move later. This makes the new layer load-bearing today, on every
boot, with nothing above it changed.

### AHCI learned how big its disk is

It had never issued IDENTIFY, so **every AHCI read was unbounded** — an LBA
past the end of the medium went straight to the controller. The new layer
refuses to register a device that cannot state its size, which turned that
from an improvement into a prerequisite.

`ahci_xfer` was generalised into `ahci_cmd` rather than duplicating forty lines
of FIS construction. Two things that file had already learned the hard way live
in that construction — the PRDT count is one *less* than the byte count, and
the error bit must be checked inside the wait rather than after it — and a
second copy would have had to learn them again.

LBA48 is read only when word 83 says the 48-bit fields mean anything;
otherwise LBA28. A driver that reads them unconditionally gets a plausible
number from a disk that never filled them in.

## What a boot now says

```
[IO] MUSTBEZERO medium=0 short=0 timeout=0 bad_request=0 out_of_range=0 register_refused=0
[IO] BLK reads=0x4f95 writes=0x1e sectors_read=0x4f95 sectors_written=0x1e devices=1
```

Twenty thousand reads and thirty writes, with every error counter at zero. All
six of the first line are asserted; `NO_DEVICE` deliberately is not, because a
machine with no disk is a configuration rather than a failure.

### The counter corrected the plan within minutes of existing

`docs/io/README.md` stated, as a surveyed fact, that no write is ever
exercised. That was wrong. A boot does **thirty sector writes** — the shell's
`mkdir DOCS` reaches `vibeos_fs_mkdir`, which reaches FAT, which writes both
copies of the table.

The claim had been made by reading code and grepping for callers, and it
survived being written into a plan. What caught it was a number. The plan is
corrected, and the accurate statement is narrower and still worth acting on:
writes reach the medium and **nothing verifies that what was written is what
comes back**.

## Verified

- Ten host-test groups for the contract, six sabotage cases in
  `io-contract.txt`, each confirmed red.
- Three boot-level cases in `io-blkdev.txt`. One of them is recorded as
  behaving differently from intended: breaking every transfer stops the machine
  before the counters are printed, so it is indistinguishable from a broken
  disk — which is arguably the honest outcome.
- The gate assertion proved by a request with a null buffer at bind time:
  `invariant_failed:io_bad_request=1`. That case was chosen over breaking a
  transfer precisely because a sabotage that destroys the thing it is testing
  proves nothing about the test.
- 8/8 clean boots, gcc and clang green.

## Not done

The callers still use the old three-function API. `fat.c` reaching the disk
directly, and the three parallel read paths, are I2.
