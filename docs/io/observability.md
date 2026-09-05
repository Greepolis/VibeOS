# Observability

The storage path currently carries **zero counters**. Not few — none. The
memory manager carries about twenty, most of them asserted by the boot gate,
and the difference is why "the disk is slow", "the disk is retrying" and "the
disk is fine" are the same silence today.

This document lists what each layer counts, including layers that do not exist
yet. That is deliberate and it is what the memory-manager plan did: filling in
a field later is cheap, migrating a structure and every assertion that reads it
is not.

## The rule this project arrived at the hard way

**A number that is written and never read is not observability.** Three
subsystems have shipped one: the scheduler computed a time slice nothing
consulted, the memory watermarks were set at boot and never checked from the
allocation path, and reclaim grew a tier nothing called. Each was correct in
isolation and wired to nothing.

So every counter below is in one of two states, and the state is written next
to it:

- **asserted** — the boot gate fails on it, and a sabotage case proves the
  assertion can go red;
- **diagnostic** — printed for a human, and explicitly not a gate.

A counter in neither state should not be added.

## B0/B1 — devices

| counter | state | why |
| --- | --- | --- |
| `reads`, `writes`, `sectors_read`, `sectors_written` | asserted non-zero | a boot that reads nothing has not booted |
| `errors[reason]` | asserted zero for medium/short | a disk returning errors is not a healthy boot |
| `errors[absent]` | diagnostic | a machine with no disk is a configuration |
| `timeouts` | **asserted zero** | the bound exists so this can be zero; virtio-net's transmit bound is the precedent |
| `max_wait_iterations` | diagnostic | how close the bound came to firing, which is the number that says a bound is too tight *before* it fires |
| `requests_in_flight_peak` | diagnostic | sized the queue when I6 lands |

**`max_wait_iterations` is the one worth arguing for.** A timeout counter tells
you the bound fired; it cannot tell you the bound is one retry away from
firing on a slower machine. This project has already tuned a bound twice by
watching failures instead of margins — virtio-net went 50M, 2M (which broke the
network), then 20M.

## B2 — the block cache

| counter | state | why |
| --- | --- | --- |
| `hits`, `misses` | **asserted as a ratio** | the mm page cache shipped at 36% while "non-zero" passed happily; a floor near what the thing actually does is the only useful kind |
| `evictions`, `resident` | diagnostic | sizing |
| `wrong_key_returned` | **asserted zero** | the page cache once returned another file's frames; this is that defect's counter |
| `write_back_pending`, `write_back_failed` | asserted zero for failed | a write-back that fails silently is a lost file |

## B3 — volumes

| counter | state | why |
| --- | --- | --- |
| `volumes_found`, `mounts` | asserted non-zero | a boot with no mounted volume has not booted |
| `probe_rejected[fs]` | diagnostic | which filesystem said no to what, which is the only way to debug a disk that mounts as the wrong type |
| `table_writes_refused[reason]` | asserted zero unless a test caused it | a refused table write is either a bug or a test |

## B4 — files

| counter | state | why |
| --- | --- | --- |
| `opens`, `reads`, `bytes_read` | asserted non-zero | |
| `short_reads` | **asserted zero** | the defect this project already had: a short read reported as a complete file, and `execve` parsing the previous program's bytes |
| `writes`, `bytes_written` | asserted non-zero once I4 lands | until then, evidence that nothing writes |

## Events, not just counters

Counters say how often. Some things need to say *which*, and this project has
learned that the hard way twice — a fault that named an address but not the
program, and a free that named a frame but not the caller.

- **`[BLK] error`** — device, LBA, sector count, direction, reason. One line,
  written under the console lock in a single call, because a diagnostic split
  across several calls comes back interleaved from different cores and reads as
  a contradiction.
- **`[VOL] found`** — device, first sector, length, table type, probed
  filesystem. Printed for every volume at boot, so "the machine mounted the
  wrong thing" is answerable from the log rather than by adding printfs.
- **`[FS] mounted`** — path, type, volume.

## What the boot must be able to say without any new code

Three questions, and if a future defect makes any of them unanswerable from an
ordinary boot log, that is a regression:

1. Which devices exist, and how big are they?
2. Which volumes are on them, and what is on each volume?
3. What did the machine read and write, and did anything fail?
