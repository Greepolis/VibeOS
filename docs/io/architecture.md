# Architecture — the layers and where the boundaries go

Five layers. The test of a boundary is whether a layer can be host-tested
without the one below it, and the memory-manager rewrite is the evidence that
this is worth insisting on: everything in that subsystem which could not be
host-tested is where the defects were.

```
  B4  files            vibeos_fs_ops_t: FAT, ext2, NTFS, ISO9660, exFAT
  B3  volumes          partitions, mounting, which filesystem is on what
  B2  block cache      one cache, shared, the only place a sector is held
  B1  block devices    a request, a completion, and a reason when it fails
  B0  drivers          virtio-blk, AHCI, and whatever comes next
```

Today B4 is split across two directories, B2 is bypassed by the only
filesystem that runs, and B1 is four function pointers with three shapes.

## B0 — drivers

A driver registers a table and nothing else:

```c
typedef struct vibeos_blk_driver {
    const char *name;
    uint32_t    sector_bytes;     /* 512 today; not assumed to be */
    uint64_t    sectors;          /* how big the device is */
    int  (*submit)(void *ctx, vibeos_blk_request_t *req);
    void *ctx;
} vibeos_blk_driver_t;
```

One entry point rather than three. `read`, `read_many` and `write` today are
the same operation with different shapes, which is why `write` has no
`write_many` — nobody noticed the asymmetry because nothing writes.

**Registration, not weak symbols.** The PE/COFF lesson is written down in
CLAUDE.md: a weak definition in a different object from its caller links on
Linux and fails on Windows. Every extension point here is a table.

**A driver never blocks forever.** Every wait is bounded and the bound is a
counter, because virtio-net taught this project that a lock around something
that already waited turns one stuck transmit into every core parked behind it.

## B1 — block devices

The request is a value, not a set of arguments:

```c
typedef struct vibeos_blk_request {
    uint32_t device;
    uint64_t lba;
    uint32_t sectors;
    void    *buf;
    int      write;
    vibeos_blk_result_t result;   /* filled by the driver */
} vibeos_blk_request_t;
```

Two things this buys, and both are answers to defects this project has had.

**A reason, not a zero.** `vibeos_blk_result_t` distinguishes *not issued*,
*device absent*, *timed out*, *medium error*, *short transfer* and *ok*. Today
a caller cannot tell a disk that is missing from a disk that is broken, and the
FAT chain reader once returned an end-of-chain marker for a failed table read —
the same value a healthy last cluster returns.

**Multiple devices.** There is one global device today. A machine with a disk
and a CD, or a disk and a swap partition on another spindle, cannot be
described. `device` is an index into a small table.

## B2 — the block cache

**One cache.** Every sector read by any filesystem passes through it, including
FAT. That is the single largest structural change in this plan and it is early,
because "two structures that hold the same fact" is the shape of four separate
defects in this codebase — most recently a page cache that returned the frames
of a different file because two cores placed entries at once.

It has its own lock, supplied by registration, for the reason CLAUDE.md gives
after the fourth time: a layer with mutable statics and more than one possible
caller locks itself.

## B3 — volumes

Three things that are usually confused with each other, and this plan keeps
them apart because each fails differently.

### Reading a partition table

MBR and GPT. Exists as `kernel/fs/partition.c`, host-tested, never run. A
partition is a device to everything above: `{device, first_sector, sectors}`,
which is the same shape B1 already takes and the same shape mm's
`vibeos_swap_area_t` already takes. That is not a coincidence worth losing — a
swap partition is a volume, and B3 is what fills that descriptor in.

**GPT before MBR**, and both. A machine that only understands MBR cannot read a
disk any modern tool produces, and one that only understands GPT cannot read
the boot media this project ships. The protective MBR on a GPT disk is exactly
the trap: it *parses* as a valid MBR describing one huge partition, so a reader
that tries MBR first and stops on success gets a plausible wrong answer rather
than an error.

### Probing what is on a volume

Given a volume, which filesystem is it? Each `vibeos_fs_ops_t` gains a `probe`
that reads its own superblock and says yes or no, and the volume layer asks
each in turn.

**Probe order is a correctness question, not a preference.** exFAT's boot
sector is a FAT boot sector with different fields; a FAT probe that only checks
the jump instruction and the signature says yes to an exFAT volume and then
mounts it wrong. Each probe must identify its own filesystem *positively* and
the order must be recorded with the reason.

### Mounting more than one thing

There is one mount today, `g_rootfs`, a global. That is why the plan can say
"only FAT runs": there is nowhere to put a second filesystem even if it worked.

A mount table with paths — a small fixed array, since this kernel does not
allocate on these paths — and a lookup that resolves a path to `(mount, tail)`.
Everything above stops taking a `vibeos_fsmount_t *` and starts taking a path.

### Writing a partition table

Creating, deleting and formatting. This is the only part of B3 that can destroy
data that was not this machine's to lose, and it is treated accordingly: every
write to a table is refused unless the caller passes the current table's
checksum, so a stale view cannot overwrite a newer one. See the phase.

## B4 — files

`vibeos_fs_ops_t`, unchanged in shape. What changes is that every filesystem
reaches the disk the same way, and that the read contract stops conflating two
numbers:

```c
/* Bytes delivered into buf. Never more than len. */
long vibeos_fs_read_at(mnt, node, offset, buf, len);

/* How long the file is. A different question, and asking it must not
 * require a buffer that could hold the answer. */
int  vibeos_fs_size(mnt, node, uint64_t *out_size);
```

That separation is what unblocks the memory manager: `execve` can then read a
header window while knowing the file's real length, and the 6 MiB of staging
buffers become a header window each.

## Where the journal is *not*

It is not one of the five layers, and putting it in `kernel/fs/` — where it is
today — says it is a filesystem concern. It is not.

A journal answers one question: **do these writes happen together or not at
all?** That is a transaction, and transactions are orthogonal to both files and
blocks. ext2 wants one; FAT does not have one; and the partition-table writer
in I4c wants exactly the same thing when it puts the backup table down before
the primary. Two customers in two different layers is the test of whether
something belongs in a module of its own, and this passes it.

Its failure mode is different from everything else here too. The rest of this
stack fails by returning the wrong data, which a test can see immediately. A
journal fails by **not being replayed**, which is invisible until the power
goes out — so its verification is "interrupt the write at every point and check
what comes back", a different kind of harness from anything else in this plan.

So: `kernel/txn/journal.c`, above B2, used by whoever needs it.

### The one thing that cannot wait

A journal controls the *order* in which writes reach the medium, and a cache
that reorders around a barrier makes it worthless. That contract is between the
journal and B2, and it has to exist **before** write-back is designed — not
after.

```c
/* Everything submitted before this reaches the medium before anything
 * submitted after it. Not a flush: a flush empties the cache, a barrier
 * only orders it, and a journal that used a flush would write the whole
 * cache out on every transaction. */
int vibeos_blk_barrier(uint32_t device);
```

If I4 lands a write-back policy with no barrier in it, adding the journal later
means rewriting the write path a second time. That is the whole reason this
section is here rather than in the phase that builds the journal.

## Where the mm page cache fits

It stays where it is, above B4, keyed by file identity rather than by sector.
Decision D6 of the mm plan already settled that the two caches are not merged,
and this plan does not reopen it — but it does make the reason cleaner: B2
caches *sectors of a device*, the mm cache holds *pages of a file*, and after
this refactor the second sits on top of the first instead of beside it.
