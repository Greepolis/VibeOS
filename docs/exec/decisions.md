# Program Loading: Decisions

Asked before the phase that needs them, not settled here.

## Open

| # | Question | Why it is not mine to settle | Needed before |
| --- | --- | --- | --- |
| X1 | Are ELF headers still read eagerly, or faulted in like everything else? | Eager is simpler and keeps one small read; faulting them in removes the last special case but makes the first fault of an exec recursive | X-P2 |
| X2 | What happens to a file-backed region when the file is written while mapped? | Refusing the write, invalidating the cache, or copy-on-write are three different products. Today nothing notices | X-P2 |
| X3 | Does `execve` keep a bounded eager read for small programs? | A page-fault per page costs more than one read for a 4 KiB service; a hybrid is faster and is two code paths | X-P2 |
| X4 | Does the interpreter substitution move to a real filesystem layout now, or stay a stand-in? | It is a filesystem question, not a loader one, and doing it here would spread it | X-P2 |
| X5 | Is the fault-injection service a boot service like `svc-stress`, or a console command? | A service exercises it every boot and costs boot time; a command is opt-in and therefore forgotten | X-P3 |

## Taken

### X0 — This plan follows the scheduler's, decided 2026-08-31

Program loading and task lifetime touch the same state - an exec destroys an
address space, rewrites a task's registers and replaces its regions - and the
defect that closed most recently (`execve` not asking whether siblings shared
the address space) sat exactly on that boundary.

Doing the loader first would mean building on a lifetime layer that does not
exist yet, and the ownership question would have to be answered twice. The
scheduler plan's S-P3 puts that question in one place; this plan then uses it
rather than reinventing it.

The cost is that memory-plan P4 step 3 waits for S-P3. That is stated plainly
in the memory plan rather than left as a surprise.
