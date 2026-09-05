# P7's latency property: the bounds were there, the counters were not

**Status: closed.** Every wait a syscall can reach has a bound, each bound now
increments a counter, the counters are reported in one bracketed line, and the
boot gate asserts they are zero — and a missing line is a failure rather than a
pass.

## What was already right

The bounds all existed and each had been argued for in its own place:

| wait | bound | reachable from |
| --- | --- | --- |
| virtio-blk completion poll | 100,000,000 | any read or write |
| AHCI port-busy check | 1,000,000 | any read or write |
| AHCI completion poll | 20,000,000 | any read or write |
| virtio-net transmit | 2,000,000 | a socket write |
| TLB shootdown acknowledgement | bounded, `tlb_timeouts` | fork, COW fault |

The shootdown was the one done properly, and it is the model: bounded,
counted, and asserted by the gate — because "a shootdown that silently never
fires leaves the bug exactly as it was, with every boot green".

## What was wrong

**`VIBEOS_BLK_TIMEOUT` was produced by nobody.** It is defined in
`blkdev.h`, named by `blk_result_name`, printed by `kmain` on the
`[IO] MUSTBEZERO` line, and asserted by the boot gate. And no driver had ever
returned it: both disk drivers hit their bound and returned `-1`, which is the
same value the device's own error bit produces. "The disk took too long" and
"the disk said no" arrived at the block layer as one thing.

So the gate carried an assertion that was green and *could not go red*. Same
family as this project's "a line in the serial log is not a check": the
mechanism was in place, the reporting was in place, and the two were not
connected.

**`g_tx_timeouts` was incremented and read by nobody.** virtio-net's transmit
counted its timeouts into a static that no accessor exposed and no line
printed. That is the defect this project produces most often — a number written
and never read — and the rule for it is already written down: when adding one,
write the assertion that would fail if the caller disappeared.

## What was done

Each driver counts its own bound firing and exposes the count.
`vibeos_x86_64_blk_bind` takes that accessor, and the adapter samples it around
the driver call: if the count moved, the request gets `VIBEOS_BLK_TIMEOUT`
rather than a generic failure.

Sampled around the call rather than returned through it, deliberately. The
driver entry points are `int`-returning and threading a reason through three of
them in two drivers is a wider change than the fact deserves. The counter only
grows, so a move across the call means a bound fired. Under concurrency another
core's timeout could be attributed to this request — accepted, and worth
stating: the counter the gate asserts is the driver's own and is exact, and
mislabelling *which* request timed out matters far less than the alternative,
which was not knowing anything had timed out at all.

`kmain` prints one bracketed line — `[IO] WAITS blk_timeouts=… net_tx_timeouts=…`
— because a line assembled from several `serial_puts` calls is several critical
sections, and this project has had a gate report failures that were a marker cut
in half.

## Confirmed by breaking it

**Set virtio-blk's bound to 1.** `[IO] MUSTBEZERO … timeout=0x1` and
`[IO] WAITS blk_timeouts=0x1`. That is the whole point of the change: before
it, the same sabotage produced a boot that failed with a generic error and a
`timeout` count still reading zero. The boot fails for other reasons too — a
disk that times out on every read cannot boot — so the case is about the
failure being *named as a timeout*, not about which verdict comes out.

**Remove the WAITS line.** `invariant_failed:io_wait_counters_missing`. A
counter that stops being printed must not read as a counter that stayed at
zero; that distinction is why the check treats a missing line as a failure.
