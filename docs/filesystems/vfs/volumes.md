# I4b steps 1 and 2: the disk says what is on it

**Status: the scan runs and reports. Steps 3 and 4 — probing and a mount table
— are not done, and the boot line says why.**

## What was already there

Everything. `vibeos_partition_parse_mbr`, `vibeos_partition_parse_gpt` with
both CRCs checked, the protective-MBR recognition, `vibeos_storage_scan` — all
written, host-tested, sabotage-verified, and **called by nobody**. The same
state the swap stack was in yesterday and the block cache was in the day
before, and for the same reason: nothing noticed.

So the work here was not writing a partition reader. It was making the one that
exists run.

## What the machine now says

```
[IO] VOLUMES table=mbr partitions=0x1 volumes=0x1 mounted=0x0 disk_sectors=0xfc000
[IO] VOLUME idx=0x0 first_lba=0x3f sectors=0xfbfc1 kind=fat fs=none
```

An MBR-partitioned disk of 504 MB, one partition starting at LBA 63, recognised
as FAT. And `fs=none`.

**That last field is the finding.** The volume is FAT and no driver claimed it,
because the portable driver set is exfat, ext2, ntfs and iso9660 — and **none
of them is the filesystem this machine actually boots from**. The FAT driver
lives in `kernel/arch/x86_64/fat.c`, outside the `vibeos_fs_ops_t` world the
scan mounts through.

So the machine can now read its own partition table and cannot mount what it
finds there, which is a much more precise statement of the gap than "only FAT
is mounted" was.

## It reads through the one cache

Not a second one. `vibeos_x86_64_fat_cache()` hands out the instance the
filesystem already uses, because two caches over one device is exactly the
arrangement I2 spent a phase removing — and standing a second one up here to
avoid a two-line accessor would have put it straight back.

## Three dead counters came alive

`volumes_found`, `mounts` and `probe_rejected` were on
`check-counters-produced.py`'s exemption list as "I4b, volumes and probing".
They are produced now, and the check **failed on the stale exemptions** —
which is the half of that check that is easy to overlook: an entry left there
after it stops being true is a permission slip lying around for the next
counter that goes quiet.

## What is not asserted, and why

`mounted` is not required to be non-zero. Demanding a mount here would be
demanding a driver that does not exist, and a gate that fails for a reason
nobody can act on is one people route around. The volume count and the disk
size are asserted: a scan that found nothing, or one that saw a disk of unknown
size, is broken rather than merely unlucky — a size of zero makes the scan
decline to parse a GPT at all, since every one of its checks is against a size.

## The case this environment cannot run

Accepting the protective MBR as a real table is the most dangerous defect this
layer can have, and this boot cannot reach it: the medium is MBR-partitioned.
A GPT disk parses as a perfectly valid MBR describing one partition covering
the whole disk, so a reader that tries MBR first and stops on success gets a
*plausible wrong answer* rather than an error — and nothing reports it. The
host tests cover it; the case file records that end-to-end reproduction needs a
GPT image, in the same spirit as the AHCI cases QEMU cannot exercise.
