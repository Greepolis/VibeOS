# Which disk is the boot disk

The Filesystem Runtime Gate had been red for days. The AHCI job wedged; the
virtio job was green. This is what it was, and — more usefully — why nothing in
the I/O stack could have reported it.

## The symptom

```
[QEMU-CLI] FAIL missing:VIBEOS_SELFTEST_DONE verdict=guest_wedged
           phase=kernel_cli_ready
```

Reproduced locally on the first attempt with `VIBEOS_SMOKE_DISK=ahci`.
Deterministic, which is worth saying: most failures in this project are not, and
a deterministic one is a gift.

## Every counter was zero and every counter was right

```
[BLK] disks=0x2 disk=virtio-blk:0x0 disk=ahci:0x1
[BLK] disk driver: virtio-blk
[IO] BLK reads=0x1 writes=0x0 sectors_read=0x1 devices=0x2
[IO] MUSTBEZERO medium=0 short=0 timeout=0 bad_request=0 out_of_range=0 ...
[COMPAT] linux syscalls translated=0x0 denied=0x33
[SCHED] task pid=... exited code=0x7f
[EXEC] refused reason=not-found path=EFI/BOOT/SVC_BOMB.ELF at=read_file
```

**One read. One sector. The entire boot.** Every must-be-zero counter in the
block layer read zero, and none of them was wrong: the read *succeeded*. It
simply was not a filesystem.

That is the general lesson and it is a new one for this project's collection: a
machine that reads the wrong disk perfectly has nothing to report. Every
detector in the I/O stack watches for a transfer that went wrong. None of them
can see a transfer that went right, to the wrong place.

What named it was one line — `disk driver: virtio-blk` on a machine whose ESP
was on AHCI.

## The cause was a sentence that stopped being true

From `blk.c`, above `blk_boot()`:

> "Adapter 0 is the first driver that came up, which is the one the machine
> booted from — every caller above this layer means that one when it says 'the
> disk'."

True for exactly as long as the machine had one disk. **I5b added a second one**,
deliberately on the *other* controller, so that the kernel log lives on a medium
that is writable when the boot filesystem is not. With `VIBEOS_SMOKE_DISK=ahci`
the ESP is on AHCI and the log is on virtio-blk, so adapter 0 is a blank 4 MiB
raw file.

The commit that made the sentence false also restated it, four lines below the
change that broke it:

> "The order still decides which one is 'the disk': virtio binds first and keeps
> that meaning for every caller above."

Written down as a deliberate choice, in the same change that introduced the
configuration it cannot survive.

## The same assumption, twice, one file apart

Fixing `blk.c` moved the failure rather than closing it: the boot volume then
mounted correctly and the *log sink* wrote over the ESP, which QEMU's vvfat
reported as `Tried to write to protected bootsector` and the boot reported as
`io_medium=73`.

`io_bringup.c` had the mirror of the same belief:

```c
/* Adapter 1: the second disk that bound. Adapter 0 is the boot disk and is
 * deliberately not eligible ... */
dev_no = (adapter_count > 1u) ? vibeos_x86_64_blk_adapter_device(1u) : -1;
```

With the ESP on AHCI, adapter 1 *is* the boot disk. Two places encoded the same
false invariant, neither knew about the other, and nothing checked either.

## The fix

Ask the question that can actually be answered. The block layer cannot tell a
boot volume from a blank medium — a layer guessing at something it cannot check
is how this got written in the first place — but the filesystem can. So the
filesystem chooses:

- `vibeos_x86_64_blk_set_boot(n)` points the no-argument disk path at adapter
  `n`; the bring-up tries each until one mounts. Bind order decides nothing.
- `vibeos_x86_64_blk_boot_adapter()` answers "which one is it", and the log sink
  takes the first adapter that is not that one instead of hardcoding 1.
- The `[BLK] disk driver:` line **moved** to after the selection. It used to be
  printed before anything was mounted, so it could not report anything except
  bind order: the field a human reads to check which disk was used was
  structurally incapable of showing the bug. That is not a cosmetic move, and it
  is why this cost days.

Cost: one sector read per rejected disk, once per boot.

## Verified

| | verdict | line |
|---|---|---|
| virtio (default) | `status=pass` | `boot volume on virtio-blk rejected=0x0 disks=0x2` |
| AHCI | `status=pass` | `boot volume on ahci rejected=0x1 disks=0x2` |

Both controllers green. `rejected=1` on AHCI is the selection doing work; `0` on
virtio is it correctly doing none.

## Gated

A new assertion reads the `[BLK] boot volume on` line and fails on a missing
report, on `none`, and on a rejection count that exceeds the number of disks.

It is asserted on **both** runs, not only the AHCI one. The failure it really
guards against is the selection quietly going away: on the default machine the
boot disk is adapter 0 anyway, so a regression there is invisible in the verdict
and visible only in that line.

Deliberately *not* asserting `rejected > 0` — that holds on AHCI and not on
virtio, and an assertion that is only true in one configuration is one somebody
will loosen.

### The sabotage run

Making `vibeos_x86_64_blk_set_boot` return without assigning reproduces the CI
failure exactly:

```
status=fail
reason=missing:VIBEOS_SELFTEST_DONE verdict=guest_wedged elapsed=50s
       quiet_for=45s phase=kernel_cli_ready
[BLK] boot volume on none rejected=0x1 disks=0x2
```

One thing that run showed which the plan for it did not: the report says
`boot volume on none`, so the new `boot_volume_on_no_disk` assertion describes
the state correctly — and never fires, because the wedge is detected first and
short-circuits the invariant checks. The case makes that assertion *true*;
nothing has yet observed it turn a boot red on its own. Recorded rather than
glossed, because "no case exists", "the case exists and this environment cannot
tell", and "the case exists and something else fails first" are three different
things.

Cases in `scripts/dev/cases/io-boot-disk.txt`.

## What this says about the core plan

Two files encoded the same false invariant about which adapter is the boot disk,
and the checker that would have caught it does not exist. The invariant is
exactly the shape `check-blast-radius.py` and `check-subsystem.py` are specified
for in `docs/core/`: a fact about a registry that callers reach in and assume,
rather than ask.

It also strengthens C0's case for itself. This was not found by the plan, by
review, or by reading — it was found by looking at CI, and it had been sitting
red long enough that "the gate is red" had stopped carrying information. *A check
that has been red since before you arrived is a check nobody is reading*, applied
to the boot gate itself.
