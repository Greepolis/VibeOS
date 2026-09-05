# The execve failures that said nothing

**Status: closed. Every exec refusal is now named, counted and asserted.**

## How it surfaced

A six-boot run came back 4/6, with both failures reporting
`stress_seed_not_reported,stress_run_did_not_finish`. Every memory counter was
clean — no leaks, no double allocations, no poison hits, no rmap mismatch — and
`SVC_STRS.ELF` appeared in the log only as a line of `ls` output. The service
had never run.

Following it took three steps and none of them should have been necessary:

1. init printed `SVC_START svc-stress 10`, so the fork succeeded.
2. The scheduler printed `task pid=0xa exited code=0x7f` — 127.
3. Reading `user/prog/init.c` showed where 127 comes from: the child calls
   `execve` and, when it returns, exits 127. **execve had failed.**

And the kernel said nothing about it. The `[EXEC] … refused:` tally showed
`not-found=6`, the same six a healthy boot shows.

## What was wrong

`hw_exec_refuse` prints a line and increments a per-reason tally, and the boot
gate asserts the *set* of reasons seen contains only `not-found` — which is a
much stronger check than a count, and was already right.

Three paths never reached it. The copies from user memory — the path string,
the argv vector, the envp vector — returned a bare `-EFAULT`, `-E2BIG` or
`-EINVAL`. No line, no tally, no reason. An execve that failed on one of them
was indistinguishable, from outside, from an execve that was never called.

That is the same defect this subsystem already carried a comment about, in the
place where it had been fixed:

> Counted as well as printed. This one had a message of its own, which is
> exactly how it stayed outside the tally: a sentence in the log is not a reason
> anything can assert on.

The `not-found` path was fixed with that reasoning. The three beside it were
not, and nothing noticed for as long as they never fired.

## The fix

A new reason, `VIBEOS_EXEC_BAD_ARGS` — "the kernel could not read the path, the
argv vector or a string in it" — and the three paths routed through
`hw_exec_refuse`, with a detail saying which vector: a program with too many
arguments and a program handed a bad environment pointer are not the same bug.

Confirmed by breaking it. Setting `VIBEOS_HW_MAX_ARGV` to 1 makes init's
two-entry argv too large:

```
[EXEC] refused reason=bad-args path=EFI/BOOT/SELFTEST.ELF at=argv
[EXEC] refused reason=bad-args path=EFI/BOOT/SVC_OK.ELF at=argv
reason=missing:VIBEOS_SELFTEST_DONE verdict=guest_wedged
```

The boot goes red and says why. Before this change the identical sabotage
produced services that quietly did not start.

A note on that sabotage, because it nearly passed for a different reason: the
first attempt used `sed` against `32u` while the constant reads `16`, so the
edit never applied and the run came back green having proved nothing. It was
caught only because the script prints the constant before and after. CLAUDE.md's
rule about a sabotage run that proves nothing is worth applying to the edit
itself, not only to the harness.

## It named itself on the first occurrence

The eight-boot run after the fix hit it once, and the failure that used to read

```
reason=invariant_failed:stress_seed_not_reported,stress_run_did_not_finish
```

now reads

```
reason=invariant_failed:exec_unexpected_refusal=bad-args,stress_seed_not_reported,...
[EXEC] refused reason=bad-args path=EFI/BOOT/SVC_STRS.ELF at=argv
```

with the chain complete and legible in three lines:

```
SVC_START svc-stress 10
[EXEC] refused reason=bad-args path=EFI/BOOT/SVC_STRS.ELF at=argv
SVC_EXIT svc-stress 10 127
```

Two things narrow it a long way. The **path** copied fine - it is in the
message - so the failure is specific to the argv vector, not to reading user
memory in general. And `svc_argv` lives in init's `.bss`: the child of the fork
writes `argv[0]` and `argv[1]` into it, which is a **copy-on-write write**, and
execve then reads that same page back.

So the shape is a page the child has just faulted copy-on-write, read by the
kernel a moment later.

## What is still open

*Which* of the range check's reasons applies. "bad-args" says a vector could not
be read and not why, and the check has a `why` for exactly this - "not
accessible without a reason is a dead end" is already written down beside it.
The refusal now carries it: the detail reads `argv:pdpt_absent_or_not_user` or
`argv:leaf_not_writable` or whichever level of the walk failed, because an
absent PML4 entry and a leaf that is present but not user-accessible are
completely different bugs.

That distinction is what decides between a stale TLB entry after the
copy-on-write fault and a page-table level that was never installed. The next
occurrence answers it.
