# NTFS and exFAT, and the limit that made NTFS useless (I5)

Both drivers had been in the tree for months. Neither had parsed a byte. A boot
now carries six mounts and five filesystem implementations:

```
[IO] FSIMAGE name=ext2    sectors=0x1000 got=0x1000 result=OK
[IO] FSIMAGE name=iso9660 sectors=0x2f0  got=0x1000 result=OK
[IO] FSIMAGE name=ntfs    sectors=0x8000 got=0x1000 result=OK
[IO] FSIMAGE name=exfat   sectors=0x8000 got=0x0    result=OK (no marker)
[IO] MOUNTED at=/ type=fat   /vol1 fat   /ext2 ext2
[IO] MOUNTED at=/iso iso9660 /ntfs ntfs  /exfat exfat
```

## NTFS could not read the root of any real volume

`ntfs_index_scan` refused any directory whose entries lived in an
`$INDEX_ALLOCATION` rather than in the resident index root. That reads like a
limitation on large directories, and it is not: **mkntfs gives the root
directory one**, because a fresh volume already contains sixteen metadata
files. So the driver failed at the first lookup on every volume ever produced
by anything other than this project.

The host test could not see it. Its image was synthetic, with a directory small
enough to be resident — the one shape the driver could handle. Fifth time this
phase has found code that worked only in the arrangement it was written
against, with the arrangement doing the work.

The allocation is now walked: the run list is mapped VCN by VCN, each INDX
block is read, its fixups undone, and its entries scanned with the same walker
the resident root uses. Linearly, not as the B-tree it is — every entry appears
in exactly one node, so a scan is correct, and descending would need the
collation rules to make a bounded walk slightly faster.

Sabotage-confirmed both ways, and the pair is the interesting part:

- Skipping the allocation walk turns the **boot** red and leaves the **host
  tests green**. Only the mkntfs image can see it.
- Restoring the old offsets in the extracted walker turns `test_ntfs_list`
  red. That was a mistake actually made while pulling the walker out so the
  root and an INDX block could share it, and the synthetic fixture caught it on
  the first run — which is what that fixture is for.

## exFAT mounts, and does not read a file, and says so

`mkfs.exfat` produces the image; exfatprogs ships no tool that writes into one
without mounting it, and mounting needs root and FUSE — which neither a
developer machine nor a CI container reliably has. NTFS is luckier: `ntfscp`
writes into an image directly, so its marker is staged with no privileges and
no host driver touching the artefact.

So the exFAT row carries no marker and the boot reports `OK (no marker)`.
Mounting a real `mkfs.exfat` volume and reading its root is far more than that
driver had ever done; folding it into plain `OK` would claim a byte comparison
that did not happen, and a gap that is visible is worth more than one that is
not.

## Two smaller things this turned up

**A silent refusal that named the wrong cause.** `LOOP_MAX` was 2. The third
and fourth images were refused, and every refusal in `vibeos_x86_64_loop_attach`
was a bare `-1`, so the caller reported the likeliest one: *no image on this
medium*. For a boot that sentence was simply false — the images were on the
disk. Every refusal now says which it was, and the gate tolerates only a
genuine absence.

**A field written on one path and read on all of them.** The reported byte
count was a single global, and the exFAT row — which never reads a file —
printed the previous row's value. Cleared by the function that owns the
contract now, which is the rule this project already wrote down after
`interp_base`.

## The wedge measurement, and what it actually found

Adding these two images raised the wedge rate: 2 wedges in 12 boots with four
images against 0 in 12 with two, same binary, only the medium differing. The
obvious explanation — more synchronous work during bring-up — does not hold:
bring-up costs about 0.8 s more, and the wedges happen 45 s later.

Catching one said why it is not about filesystems at all:

```
[HW][TRAP] cpu=3 vector=0xd rip=0xdead0000dead0000 rsp=0x22d5d10 -> PANIC
[MM] POISON_BROKEN frame=0x22d5000 word=0x1a0 found=0x22d5d10 owners=0
```

A core executed the free-page poison pattern, and the frame the poison detector
names is `0x22d5000` — which is the **CR3 of another core in the same wedge
report**. A task running on an address space whose top-level page has been
freed. That is the silent-wedge family this project has documented, caught this
time by the poison and the guard rather than going quiet.

`freed_by` symbolises to `vibeos_isr_stub_table`, which is not a plausible
caller, so that field is not to be trusted as it stands — first question when a
detector fires is whether the detector is right, and part of this one is not.

These filesystems did not cause that. They changed the timing and the
allocation pattern enough to make it more likely, which is the only role they
play in it. It is recorded here rather than in a filesystem commit because it
belongs to the memory manager, and it is open.

## H-011: an attribute length that wrapped the record open (fixed 2026-09-11)

`ntfs_find_attr` bounded an attribute with `off + attr_len > size` and
`ntfs_data_attr` with `at + attr_len > size`, both in `uint32_t`. An attribute
length from the volume wraps that sum: `0xFFFFFFF0` at offset 64 adds up to 48
and passes. The resident-value checks after it are correct only if `attr_len`
is, so a value longer than the whole record was accepted.

What made that worse than a wrong read: the MFT record is a local array on the
kernel stack in lookup, in `read_at` and in one more function, and `read_at`
copies the resident value to its caller. The bytes after the array - whatever
the kernel stack held - went to user space.

Every bound in both functions is now `length > size - offset`, once `offset <=
size` is known. That is the rule for any length read from a volume: the sum
wraps, the difference cannot once the offset is inside.

`test_ntfs_attr_length_wrap` edits record 9 of the host test image - a resident
`$DATA` holding "inner" - to that length and a 4096-byte value, remounts, and
looks the file up. Refusing it is a correct answer; if it is found, neither its
size nor a read may exceed the record. Red on the old code, green on the new.
It does not depend on what the stack happens to contain.

The finding asked for the same sweep through every filesystem parser, and it
was done. Record walks with 8- or 16-bit lengths over small offsets - ext2
directory entries, ISO9660 records, NTFS index entries and run headers - cannot
wrap 32 bits. The four `read_at` paths (exFAT, ext2, ISO9660, NTFS) use
`offset + len > node->size` after establishing `offset < size`: that wraps
only with a size declared within 4 GiB of 2^64 and an offset in that range,
and then `len` is not trimmed. Much harder to reach, still wrong, and recorded
in the review tracker to be fixed with M-009 and M-010.

Host tests green. `check.sh all` was **red once**: one of its three boots failed, and
its log was overwritten by the next boot, because the check loop kept no
evidence - so that failure could not be read. The loop keeps failed logs now.
Twelve boots on the same kernel, every failure kept: 12 pass, 0 fail.
