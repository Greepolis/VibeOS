# I4 step 2: a write that is checked

**Status: done and gated.** A boot writes a file, reads it back and compares it
byte for byte. Steps 1, 1b and 3 — write-back, the barrier, and the
across-reboot check — are not done.

## Why this before the mechanism

The plan lists write-back first. It is done second here, and the reason is in
I2's own note: the block cache was left write-through deliberately, "because
turning write-back on would trade a verified property for an unverified one,
and write-back belongs with I4, where writes are read back and proved". Turning
it on before the proof exists would be doing exactly that. So the proof lands
first and the mechanism gets checked by it.

## What it checks

A 1300-byte file, written, read back, compared.

**Every byte carries its own offset.** A constant survives every interesting
failure this can have — a write that landed a sector early, a read that
returned a neighbouring sector, a transfer that lost its last sector and left
the previous contents — because all of them hand back bytes equal to what was
expected. An offset-dependent pattern fails all three and says *where*.

**1300 is not a multiple of 512 on purpose.** A file of exactly N sectors never
exercises the tail, and the tail is where a length confused with a byte count
shows up. This project has had that defect once already, in the FAT reader that
returned the size the directory claimed.

**The buffer is cleared before the read**, so a read that returns nothing at
all cannot pass by leaving behind what was just written into it.

## What it does not prove

That the bytes reached the *medium*. Everything here could be served from the
block cache; with I2's write-through policy the device was written too, but
this check cannot tell the difference. Only a reboot can, and that is step 3.

## Three things it found immediately

**The write refused, and the refusal said nothing.** `fat_write_file_locked`
had seven `return -1`s: a full disk, a name that is not 8.3, a directory with
no free slot and a medium that would not take the sector all arrived at the
caller as the same value. They name themselves now.

**The parent did not exist.** The first version wrote to `DOCS/`, and at mount
time the root holds exactly `EFI` and `STARTUP.NSH` — the `docs` directory
staged on the host is **not presented to the guest at all**, and
`DOCS/NOTES.TXT` exists later only because the boot script creates it. The
check was wrong about the medium, not about the kernel, and the refusal now
says `parent_directory_not_found` rather than the earlier `path_or_name_refused`
that sent the investigation looking at the name.

**An `[EXEC]` line was assembled without bracketing**, and CI caught it before
this was committed:

```
[EXEC] interpreter EFI/BOOT/LDMUSL.SO[HW][SYS] write(ring3): SVC_EXIT svc-stress 10 0
```

Five `serial_puts` calls are five critical sections, and that one runs inside
execve while services start on other cores. The boot gate's `interleaved_lines`
check failed the run — which is the check working exactly as designed, since a
cut line can invent a failure or hide one.

A scan for its siblings reported forty-nine more. **Most are false positives**:
the scanner treats an `unlock` in an early-return branch as ending the critical
section, so a correctly bracketed block like the `[CRASH]` dump reads as
unbracketed. That is why the forty-nine were checked rather than fixed — a
blanket edit of forty-nine sites on a bad number would have been the expensive
kind of confident wrong answer.

## Confirmed by breaking it

Skipping the data-sector writes while still returning success:

```
[IO] WRITE_PROOF first_bad_offset=0x0 got=0x30 want=0x5a
reason=invariant_failed:write_proof:FAILED:_contents_differ
```

That is the failure the phase is named for: a boot writes about thirty sectors
through the shell's `mkdir` and nothing checked any of them, so a write path
that acknowledged without writing was invisible until some later boot could not
mount.

## And the checks caught their own author

`check-assertions-covered.py`, written earlier the same session, failed on
`write_proof_missing` the moment the assertion was added — before the phase was
committed. The case was written; the count went back to its baseline. That is
the whole of what that check is for, working on the first new assertion after
it existed.


---

## I4 steps 1 and 1b

**`write_many`.** Both drivers already had the multi-sector machinery —
`virtio_blk_rw_n` and `ahci_xfer` each take a `write` flag — and neither
exposed it, so the block layer's adapter wrote a run one sector at a time. The
read side learned that lesson expensively: a 2 MiB transfer done a sector at a
time under a lock that masks interrupts was indistinguishable from a hang and
was reported as one. There was no equivalent on the write side only because
until now nothing wrote enough to notice — a boot's entire writing is about
thirty sectors.

**The barrier.** `vibeos_blk_barrier(device)` is *not* a flush, and the
distinction is the point: a flush empties a cache, this only orders. They
belong to different layers — the block cache above is emptied by
`vibeos_blockcache_flush`, and what this does is tell the *device* to stop
holding writes in its own volatile cache. A real drive acknowledges a write as
soon as it reaches that cache, so a caller that has written and not called this
has no ordering at all.

virtio-blk issues `VIRTIO_BLK_T_FLUSH`, which carries no data — the chain is
the header and the status byte with nothing between them, so it could not be
built out of `virtio_blk_rw_n`. AHCI issues `FLUSH CACHE EXT`, a command that
had been **defined in that file from the beginning and never issued**: one more
of the declared-and-unused family.

**A device with no barrier is refused, not quietly granted.** A null hook
answers `NO_DEVICE`. A caller told "yes" would build on an ordering that is not
there, which for a journal is the difference between a recoverable medium and a
corrupt one.

The write proof asks for one before reading back, so the primitive is exercised
on every boot rather than merely existing, and the gate asserts both that it
was asked for at least once and that none failed.

## What is left in I4

Step 3, the across-reboot check — the only test that separates a write that
reached the medium from one that reached a cache. It needs a medium that
survives a boot, and QEMU's synthesised FAT is not one: the guest does not even
see the `docs` directory staged on the host. That makes it a job for the real
disk images and VirtualBox rather than for the boot gate.

Write-back in the block cache is also still off. The policy note in I2 said it
belongs here, and it does — but it belongs *after* step 3, because write-back is
precisely the thing that makes "the bytes are on the medium" stop following
from "the write returned". Turning it on while the only check is within a
single boot would remove the property the check is testing.
