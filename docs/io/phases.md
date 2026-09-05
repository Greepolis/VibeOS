# Phases

Each phase says what it builds, what proves it, and what turns it red. The
order is not negotiable in one place and says so.

Every phase ends the same way: host tests, a sabotage case file under
`scripts/dev/cases/`, and — where the phase reaches the running machine — a
boot-gate assertion. A phase with no sabotage case is not finished, because
this project has twice shipped a check that could not fail.

---

## I0 — the contract, with nothing behind it

**Objective.** The types exist and nothing implements them.

**Files.** `include/vibeos/blkdev.h`, `include/vibeos/io_stats.h`,
`tests/kernel/blkdev_tests.c`

**Steps.**
1. `vibeos_blk_request_t`, `vibeos_blk_result_t`, `vibeos_blk_driver_t`.
2. The statistics struct, complete — including counters for layers that do not
   exist yet. The memory manager did this and it was right: filling in a field
   is cheaper than migrating a structure and its assertions a second time.
3. A registry of devices: bind, look up, enumerate.

**Done when.** A fake driver can be registered, a request submitted to it and
the result read back, entirely in a host test.

**Sabotage.** `io-contract.txt` — accept a request with no buffer; report a
short transfer as ok; let two drivers claim the same device index.

---

## I1 — the block layer

**Objective.** One entry point, real errors, more than one device.

**Files.** `kernel/io/blkdev.c`, and the two drivers move to it.

**Steps.**
1. `vibeos_blk_submit(request)`, synchronous for now.
2. virtio-blk and AHCI register tables instead of being bound through three
   function pointers.
3. Every wait bounded, every bound a counter.
4. `read` and `write` become symmetric — the missing `write_many` is not an
   omission to fix later, it is evidence that nothing writes.

**Done when.** The boot mounts and runs exactly as before, through the new
layer, with `blk_reads`, `blk_writes`, `blk_sectors` and `blk_errors_by_reason`
non-zero and asserted.

**Sabotage.** `io-blkdev.txt` — lose the sector count on a multi-sector
transfer; report a timeout as success; ignore the device index.

---

## I2 — one cache, not three

**Objective.** Every sector any filesystem reads goes through one cache.

**This is the risky phase and it is deliberately early**, because the risk is
not in the change — it is in living with three caches for another six months.

**Steps.**
1. The block cache gets its own lock by registration and a full set of
   counters: hits, misses, evictions, and a *ratio* the gate asserts rather
   than a presence check. The mm page cache shipped at 36% while "non-zero"
   passed happily.
2. FAT stops reading sectors directly. `fat.c`'s static sector buffers go.
3. `kernel/fs/blockcache.c` becomes the only cache below B4.

**Done when.** `cache_hits` and `cache_misses` are both non-zero on a boot,
the ratio is asserted, and FAT's static buffers are gone from the source.

**Sabotage.** `io-cache.txt` — return the wrong sector for a key; never record
a hit; skip eviction; drop the lock.

**Risk, stated plainly.** FAT is how the machine boots. A defect here does not
degrade anything, it stops the machine — and the page cache's history says the
failure mode is a *hit that returns the wrong data*, which looks like a working
boot until a program is handed somebody else's bytes. The verification for this
phase is `repeat-boot.sh` at 24, not a single green boot.

---

## I3 — the read contract: a length is not a byte count

**Objective.** Asking how long a file is stops requiring a buffer that could
hold it.

**This is what the memory manager is waiting for.**

**Steps.**
1. `vibeos_fs_size()` beside `vibeos_fs_read_at()`.
2. `hw_read_file_cached` returns bytes delivered; the file's length is asked
   for separately.
3. `execve` reads a header window. Both staging buffers shrink from 4 MiB and
   2 MiB to that window, with a named refusal — not a truncation — for a file
   whose headers exceed it.

**Done when.** `meminfo` no longer shows the staging buffers, and mm's P4 step 3
is closed.

**Sabotage.** `io-length.txt` — return the staged count as the length; accept a
file whose headers exceed the window; report a short read as a complete file.

**The last one is not hypothetical.** It is exactly the FAT defect this project
already had: `fat_next_cluster` reported a failed table read as end-of-chain,
the reader returned the size the directory claimed, and `execve` parsed the
previous program's bytes. The case exists to make sure the new contract cannot
express that.

---

## I4 — writes that are proved

**Objective.** Something writes, on a real machine, and the bytes survive.

**Steps.**
1. `write_many`, and the write path through the cache with a write-back policy
   that is stated rather than implied.
1b. **A barrier**, `vibeos_blk_barrier(device)`: everything submitted before it
   reaches the medium before anything submitted after. Not a flush — a flush
   empties the cache, a barrier only orders it.

   This is in I4 rather than in the journal phase deliberately. The journal is
   a later module and could wait; the *ordering contract it needs* cannot,
   because a write-back policy designed without it has to be rewritten when the
   journal arrives. The partition-table writer in I4c needs the same primitive
   for the same reason, which is the evidence it belongs at this level rather
   than inside whoever asked first.
2. A boot-time exercise: write a file, read it back, compare byte for byte.
3. The same across a reboot, which is the only test that distinguishes a write
   that reached the medium from one that reached a cache.

**Done when.** The gate asserts the round trip and the across-reboot check.

**Sabotage.** `io-write.txt` — acknowledge a write that was never issued; write
to the wrong LBA; lose the last sector of a multi-sector write; report a
write-back failure as success.

**Why this is high on the list.** The write path today has the same standing as
the seven unmounted filesystems: it exists, it is host-tested, and no evidence
exists that it works. A filesystem that can only read is a demo.

---

## I4b — volumes: partitions, probing, and more than one mount

**Objective.** A disk is more than one filesystem, and the kernel can say what
is on it.

**Files.** `kernel/io/volume.c`, `kernel/io/parttab.c`, and `kernel/fs/vfs.c`
grows a mount table.

**Steps, in order.**
1. **Read both table formats.** GPT and MBR, with GPT tried first and the
   protective MBR recognised for what it is. A GPT disk parses as a valid MBR
   describing one huge partition, so a reader that tries MBR first and stops on
   success gets a plausible wrong answer rather than an error — which is the
   worst kind, because nothing reports it.
2. **A volume is a device.** `{device, first_sector, sectors}`, the same shape
   B1 takes and the same shape mm's swap area takes. Everything above B3 stops
   knowing whether it is looking at a whole disk or a slice of one.
3. **Probe.** Each `vibeos_fs_ops_t` gains a `probe` that reads its own
   superblock. The order is recorded with its reason: exFAT's boot sector *is*
   a FAT boot sector with different fields, so a FAT probe that checks only the
   jump and the signature claims an exFAT volume and mounts it wrong.
4. **A mount table.** There is one global mount today, which is the structural
   reason only one filesystem can run. A small fixed array, no allocation on
   these paths, and a resolver that turns a path into `(mount, tail)`.

**Done when.** The boot lists the volumes it found, with their type and size,
and mounts more than one.

**Sabotage.** `io-volume.txt` — accept the protective MBR as a real table;
report a partition that extends past the end of the disk; let two mounts claim
the same path; have a probe say yes to a filesystem that is not its own.

---

## I4c — writing a partition table

**Objective.** Create, delete and format a volume — and never the wrong one.

**Separated from I4b deliberately.** Reading a table wrongly gives a machine
that cannot boot, which is loud. Writing one wrongly destroys data that was
never this machine's to lose, and it does it silently and immediately. They do
not belong in the same change.

**Steps.**
1. Every table write takes the checksum of the table it is replacing, and is
   refused if the disk no longer holds it. A stale view must not be able to
   overwrite a newer one.
2. A refusal to touch a partition that is mounted, or that the swap area names.
3. Format: put a filesystem on a volume, and only through the filesystem's own
   `format` op — the volume layer must not know what a FAT boot sector looks
   like.
4. GPT's backup table at the end of the disk written *before* the primary, so
   an interrupted write leaves a disk with a recoverable table rather than two
   corrupt ones.

**Done when.** A test partitions a scratch device, formats it, mounts it,
writes a file, unmounts, re-reads the table and finds the same layout.

**Sabotage.** `io-parttab.txt` — write the primary before the backup; accept a
stale checksum; format a mounted volume; write a table whose entries overlap.

**Confirm before doing.** This is the one phase in this plan that can lose a
user's data, and the case file should say so at the top rather than at the
bottom.

---

## I5 — the filesystems that have never run

**Objective.** ext2, NTFS, ISO9660 and exFAT are mounted from real images and
read from, in CI.

**Steps, one filesystem at a time.** Each is its own change: build a small
image with the host's own tools (`mke2fs`, `mkisofs`, `mkfs.exfat`), attach it
as a second device, mount it, read a known file, compare bytes.

**Done when.** Each has a gate assertion naming it, and the boot says which
filesystems mounted.

**Why one at a time.** Four filesystems landing together means a failure is
attributed by guesswork. This project's own rule about counts applies to
changes as well: change one thing.

**And a decision this phase forces.** A filesystem that cannot be given a real
image in CI should be *deleted*, not kept. Two thousand lines that nothing
runs is not a feature, it is a liability with a plausible name — and this plan
would rather record that honestly than carry it.

---

## I5b — the kernel's own log, on disk

**Objective.** A machine that crashes leaves its log behind.

**Not the same thing as I5c.** `journal.c` is a write-ahead journal for
filesystem consistency. This is the operating system recording what happened,
so it can be read after the machine that recorded it has stopped. The two share
a word and nothing else, which has already caused one misunderstanding.

**Why it matters more here than it would elsewhere.** Every hard defect in this
project was diagnosed from a serial log — the silent wedge, the interleaved
console, the copy-on-write corruption, all of it. On a developer's machine QEMU
captures that. On an appliance, or on real hardware with no serial cable, a
wedge leaves nothing at all, and the only evidence of the last several failures
would not have existed.

**Steps.**
1. A reserved area, not a file. It has to be writable when the filesystem is
   broken — which is exactly when it is most wanted — so it is a volume from
   B3 with a fixed header, addressed by sector.
2. A circular buffer with a monotonic sequence number, so the reader can order
   records across a wrap and across reboots, and can tell "the machine stopped"
   from "the log wrapped".
3. **Write-through, and never write-back.** The last few lines before a crash
   are the entire point, and a cache holding them when the power goes is the
   one failure this feature cannot have. This is the same ordering contract the
   barrier in I4 provides, used for the opposite reason: the journal wants
   ordering to be *able* to batch, this wants it to refuse to.
4. It must not allocate, and it must work from a panic handler — which means
   from a context where another core may hold the console lock and where the
   scheduler is parked.
5. A reader: `log` on the kernel console shows the previous boot's tail.

**Done when.** A boot writes records, the machine is reset, and the next boot
prints what the previous one recorded — including the last line before the
reset.

**Sabotage.** `io-logsink.txt` — lose the last record before a reset; wrap
without advancing the sequence; allocate on the write path; write through the
cache instead of past it; let a panic-time write take a lock it might not get.

**The one that matters most is the first.** A log that loses its last line
loses the only line anybody wanted.

**Deliberately after I4b**, because it needs a volume to live in, and
deliberately before I6, because asynchrony is precisely when a machine most
needs a log that is already on the medium.

---

## I5c — the journal, as a module of its own

**Objective.** "These writes happen together or not at all", available to
anything that needs it.

**Files.** `kernel/txn/journal.c` — moved out of `kernel/fs/`, which is where
it is today and which says it is a filesystem concern. It is not: ext2 wants
one, FAT has none, and I4c's partition-table writer wants exactly the same
guarantee when it puts the backup table down before the primary. Two customers
in two layers is what makes it a module rather than a feature.

**Steps.**
1. Move it, unchanged, and give it the barrier from I4 instead of whatever
   ordering it assumes today.
2. A transaction API that is not filesystem-shaped: begin, add a write, commit,
   and a replay run at mount.
3. ext2 uses it. Then I4c uses it, which is the change that proves it is not an
   ext2 feature wearing a general name.

**Done when.** A transaction interrupted at any point leaves the medium either
fully updated or fully unchanged, demonstrated rather than argued.

**Sabotage.** `io-journal.txt` — commit before the data is on the medium;
replay a transaction that never committed; skip replay at mount; reorder across
a barrier.

**The verification is the interesting part.** Every other phase here is checked
by a test that reads something back. This one needs the write interrupted at
each step, which means a driver that can be told "fail after N sectors" and a
mount that then has to recover. That harness is worth building once and is
reusable for I7's failing-disk property — which is why I5c comes before I7 and
not after it.

**And what the row in the progress table currently claims.** It says
"journalled writes verified against power loss", which is true of a host test
and not of any machine that has ever run this kernel. This phase is what would
make the sentence mean what it appears to mean.

---

## I6 — asynchronous I/O

**Objective.** A request can be submitted and completed later.

**Deliberately last, and it cannot move earlier.** Asynchrony makes every
existing defect harder to see: a wrong sector returned synchronously is a bug
you can print, and the same bug behind a completion queue is a bug you
reproduce one boot in thirty. Everything above must be correct and counted
first.

**Steps.** A completion callback; a queue per device; interrupt-driven
completion for virtio-blk and AHCI; the synchronous entry point kept as a
wrapper that waits, so nothing above has to change at once.

**Sabotage.** `io-async.txt` — complete a request twice; complete the wrong
request; lose a completion; run the callback with the queue lock held.

---

## I7 — acceptance

Each is a test that does not exist.

- **A disk that fails.** A driver that returns errors on demand, swept across
  which request fails. Every failure must leave no request in flight, no
  sector wrongly cached, and a reason the caller can print.
- **A disk that is absent.** The machine boots, says so, and refuses cleanly.
- **A disk that is slow.** Every bound is asserted and every timeout counter is
  zero on a healthy boot.
- **Isolation.** A sector freed from the cache and re-read is never served from
  the previous tenant's data.
- **Soak.** Sustained read and write, with the cache's frame count returning to
  where it started — the same property the memory manager now asserts for
  userland.

**Done when.** All five run in CI, each has a sabotage case that turns it red,
and every claim in `README.md` §2 maps to the test that proves it.
