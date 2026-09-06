# The kernel's own log, on a medium that outlives the machine (I5b)

Every hard defect in this project was diagnosed from a serial log — the silent
wedge, the interleaved console, the copy-on-write corruption, all of it. On a
developer's machine QEMU captures that. On an appliance, or on real hardware
with no serial cable, a wedge left nothing at all, and the evidence behind the
last several fixes would simply not have existed.

A boot now says:

```
[IO] LOGSINK result=OK capacity=0x1fff prev_seq=0x7 written=0x1 failed=0x0
     bad=0x0 previous=VIBEOS boot mark
```

`previous=` is the previous machine's last record, read back from the medium
after the machine that wrote it had stopped.

Not the same thing as `kernel/fs/journal.c`. That is a write-ahead journal for
filesystem consistency; this is the operating system recording what happened.
The two share a word and nothing else, which has already caused one
misunderstanding.

## What had to happen first

The kernel could not have a second disk. `vibeos_x86_64_blk_bind` held one set
of function pointers and refused the second driver — *"first one to come up
owns the disk"* — which was right while a machine had one disk, and is why I5's
filesystem images had to be reached through a loop device. A log on the boot
filesystem is unwritable exactly when it is most wanted, so that singleton was
a hard blocker, not an inconvenience. It is gone; see the commit that removed
it, which also found that AHCI only ever worked because UEFI had prepared its
port.

## The three decisions that are not obvious

**No lock on the write path.** The context that most needs to write is a panic
handler, where another core may hold any lock and the scheduler is parked — a
lock here is a way for the last line before a crash to be the one that never
arrives. The sequence number is a single atomic fetch-add and the slot is
derived from it, so two cores writing at the same instant get different
sequence numbers, therefore different sectors, and never touch each other's.
The only other shared state is the staging buffer, and there is one per core.

**No header update per record.** The obvious design keeps a head pointer in
sector 0 and writes it after every record — a second thing to lose at exactly
the wrong moment, and double the I/O on the path whose latency matters most.
Each record carries its own sequence number and the head is *recovered* by
scanning at attach. A boot pays one pass over the medium; a lost head pointer
costs the log.

**Write-through, never write-back, never through the block cache.** The last
few lines before a crash are the entire point. Same ordering contract as I4's
barrier, used for the opposite reason: the journal wants ordering so it can
batch, this wants it to refuse to.

## Two things that were got wrong first

**A claim in a comment that the code did not support.** The module said the
checksum covering the sequence number is what stops a stale slot from
impersonating a fresh one after a wrap. Sabotaging that rule was caught by
*nothing*, because the reader compares the sequence it got against the one it
asked for and catches staleness whatever the checksum covers. What the rule
actually guards is **attach**, which validates records with no sequence to
compare against: one flipped high bit would set the next sequence to something
enormous for the rest of the life of the medium. The comment was corrected and
the test that makes the rule mean something was written afterwards — the
sabotage came first and the test second, which is the order that produces a
test that can fail.

**A bring-up that read one sector at a time.** Recovering the head over a 4 MiB
medium meant 8191 synchronous single-sector transfers, and boots began wedging
about two in four. It did not look like a slow log; it looked like a stopped
machine, and the two failures it produced — a wedge in `busybox_cat` and the
known intermittent `bad-args` — point nowhere near a log. It was identified by
shrinking the medium to 512 sectors and watching four boots go green, and then
fixed properly by batching the scan rather than by keeping the medium small.
The scan is the only batched path; the write stays one sector, because batching
*there* would be the write-back this module exists to refuse.

## What is gated

`logsink_missing` if the sink stops being brought up; the verdict itself;
`logsink_wrote_nothing`; `logsink_write_failed`; and the one that is the
feature: **`logsink_lost_the_previous_boot`** — once the medium carries records
from an earlier machine, the previous boot's last line must come back.

That assertion is conditional on `prev_seq > 0` deliberately. A fresh medium
legitimately has nothing on it, so `previous=none` cannot simply be banned; the
gate keeps `qemu-cli-logdisk.img` between runs, so the first run in a working
directory exercises the empty branch and every run after it exercises
persistence.

The bring-up reads record 0 *before* it writes this boot's mark. Reading it
after would make the newest record this boot's own, and the check would prove
that a write followed by a read works — nothing about persistence at all. That
ordering is load-bearing and is written down in the case file as a case that
would pass.

Nine host tests, sabotage-verified: six turn the suite red on the property they
guard, one is recorded as *the case exists and this environment cannot decide
it* (two cores sharing a staging buffer cannot be constructed in a
single-threaded host test, and nothing yet logs from more than one core), and
one — a failed write also counted as written — was initially reported as
uncaught because the `sed` that was meant to break it never applied. Third time
that trap has been walked into here.

## The second half: the log itself, and the panic

Every `hw_log` event now goes to the medium — every one, including those below
the serial level, because the quiet lines nobody was printing are exactly what
is wanted after a machine has stopped. A boot raises a few dozen, so the cost
is a few dozen sector writes against the several thousand reads a boot already
performs.

The formatting is shared between the serial writer and the sink so the two
cannot drift, and it neither allocates nor locks, because it runs from a panic
handler. `hw_panic` writes its reason to the medium *first*, before anything
else it does: every line after that is one more chance to stop before reaching
the disk.

A per-core re-entrancy flag guards the one loop that would otherwise exist: the
sink writes through the block layer, and the block layer logs when a request is
refused. One flag per core rather than a global — two cores logging at once are
not recursion, and a global would silently drop the second one.

`logdisk` on the console prints the tail, newest first. Deliberately not named
`log`: that already prints the two in-memory rings, and two commands whose
names differ by nothing would be read as the same thing. This one answers a
different question — what survived the last machine.

```
[LOGDISK] seq=0x13ad [DEBUG] task exited (a0 = pid, a1 = code) ...
[LOGDISK] seq=0x13ab [DEBUG] futex wake code=0x14 a0=0x8000800690 ...
[LOGDISK] seq=0x13a9 [DEBUG] copy-on-write fault resolved ...
```

The gate drives the command and asserts *more than the boot mark* rather than
merely non-zero: a sink holding only its own bring-up would satisfy a non-zero
check, and that is precisely the state the first half of this phase left.

## One thing that came out of this and is not explained

Sabotaging the feed — removing the one call that hands a log event to the sink —
wedged the machine on three boots out of three, before the console was reached.
That is not the known intermittent, which is about one in six and does not
repeat like that. Removing a disk write from the kernel log path should not stop
the machine, so something on that path depends on the timing it provides.

It means the kernel-side sabotage could not reach the new assertions, so those
were confirmed by moving the gate thresholds instead — which proves the parse
and the branches, and is weaker than breaking the code and watching it go red.
Both facts are recorded as they are in `scripts/dev/cases/io-logsink.txt`.

Worth chasing before I6: asynchrony will fold this into the noise.
