# Pipes decide end-of-file outside their lock

**Status: OPEN.** Verified against the source, not yet fixed. Deferred on purpose
until C5 lands, so that a failure in either change can be attributed to one of
them rather than to both.

An external review reported the write half. It is real, and the read half -
which the review mentioned only as "ideally" - is the more serious of the two.

## The write half, as reported

`hw_pipe_write` tests `pp->readers == 0` **before** taking `g_pipe_lock`, while
`hw_pipe_release` decrements `readers` under that lock. So a writer can read
`readers = 1`, the last reader closes on another core, and the writer then takes
the lock and enqueues anyway.

The effect is a semantic violation rather than corruption. If everything fits in
that one locked pass, `write` returns the full length with no `SIGPIPE` and no
`EPIPE`, for bytes nobody can ever read. The buffer is not recycled under the
writer's feet - `hw_pipe_release` frees a pipe only when readers *and* writers
are both zero, and the writer still holds its end - so the bytes simply sit there
until the writer closes.

## The read half, which is worse

`hw_pipe_read` takes the lock, finds `count == 0`, releases it, and only then
tests `pp->writers == 0` - unlocked - to decide end-of-file:

```
reader                              writer
lock; count == 0; unlock
                                    lock; enqueue; unlock
                                    close -> writers = 0
writers == 0 -> return 0 (EOF)
```

The reader reports end-of-file **with data still in the buffer**. Not a late
signal: bytes written before the close, lost without a trace. The reader still
holds its end, so the pipe is not recycled - but a program that reads EOF closes,
and that close is what finally discards them.

That is the shape of an ordinary pipeline, where the writer writes and exits, and
the boot script itself runs one: `ls /EFI/BOOT | wc -l`. A rare short count there
would present as an intermittent failure somewhere in the boot, not as a pipe.

## Also present, and harmless

Between the reader's unlock and its `sti; hlt`, a writer can enqueue and call
`hw_keyboard_wake` before the reader sleeps. That is a missed wakeup, but `hlt`
also returns on the next timer tick, so it costs up to one tick of latency rather
than a hang.

## The fix, when it is taken

Both decisions move inside `g_pipe_lock` and are made together with the data they
depend on: the writer tests `readers` in the same critical section that enqueues,
and the reader tests `writers` in the same one that found the buffer empty.
Neither needs a new lock.

A test for the read half has to produce the interleaving on demand, and a loop
that races a short write against a close will only do that some of the time. The
case file should say which of those it managed, rather than counting a green run
as coverage.
