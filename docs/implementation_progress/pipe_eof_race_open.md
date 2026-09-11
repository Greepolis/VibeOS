# Pipes decided end-of-file outside their lock

**Status: FIXED (2026-09-11).** An external review reported the write half; the
read half, which it mentioned only as "ideally", was the more serious.

## The write half

`hw_pipe_write` tested `pp->readers == 0` before taking `g_pipe_lock`, while
`hw_pipe_release` decrements `readers` under it. A writer could read one reader,
the last reader close on another core, and the writer then enqueue anyway:
`write` returned the full length with no `SIGPIPE` and no `EPIPE`, for bytes
nobody could read.

## The read half, which was worse

`hw_pipe_read` found the buffer empty under the lock, released it, and only then
tested `writers == 0` - unlocked - to decide end of file. A writer could enqueue
and close in between, and the reader returned end of file with those bytes still
in the buffer: lost, silently, in the shape of an ordinary pipeline such as the
boot script's `ls /EFI/BOOT | wc -l`.

## The fix

Each decision is made in the critical section that holds the data it depends on,
with no new lock. The reader computes end of file - nothing copied, nothing
faulted, no writers - before it unlocks. The writer tests `readers` right after
taking the lock and before enqueuing, raising `SIGPIPE` after unlocking.

A missed wakeup between the reader's unlock and its `sti; hlt` is still possible
and still harmless: `hlt` returns on the next timer tick, so it costs at most one
tick of latency.

## What was not done

No red test. Both windows are a few instructions between two cores, and a loop
racing a short write against a close would pass nearly every time - a test that
cannot fail proves nothing, and the case file would have to say so. The fix is
structural and is argued from the code.

`check.sh all` green with the boot's pipelines (`BUSYBOX_SH_OK`, `PIPE_OK`).
Twelve boots: 11 pass, 1 fail - the THREADS four-worker crash family
(`.boot-evidence/fail-20260911-162643-boot12.log`, rip 0x405b72, panic), not a pipe.
