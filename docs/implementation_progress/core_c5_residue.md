# C5 residue: mprotect against fork, and one descriptor table per process

C5 closed with two items written down as out of its scope
([phases.md](../core/phases.md), "Still open"). Both are closed here
(2026-09-27), together with a third defect found while proving the second.

## mprotect held the address-space lock for half the call (M-071)

`hw_sys_mprotect` took `hw_mm_lock` around the region-list update and released
it before narrowing the page tables. The reason was real when it was written:
narrowing does a TLB shootdown, and a sibling spinning in `hw_mm_lock` with
interrupts off could not acknowledge the IPI, so the shootdown timed out and
panicked. So a fork on another thread - which walks the page tables under the
same lock - could copy an entry mprotect was halfway through changing.

The reason has gone. The lock's spin opens an interrupt window every turn, and
since M-065 a waiting core answers flush requests itself. mprotect now holds
the lock from the range check to the last entry, with a single exit.

Reading the function for that found the other half. The range check and the
page-table pass both asked `hw_pte_lookup`, which answers only for a *present*
entry - so mprotect on a page in swap was refused with EFAULT, although a
swapped entry keeps its permissions (M-063) and `vibeos_vmspace_protect`
already knew how to change them. The check now accepts present or swapped, and
the pass hands every page to `vibeos_vmspace_protect`.

**Proof.** `svc-reclaim` now makes every block read-only before reading it
back; by then most of the oldest are on disk. With the check put back to
"present only" (`scripts/dev/cases/mm-mprotect.txt`) the boot goes red with
`reclaim_load_failed` - `RECLAIM_FAIL: mprotect refused block 0` - for the
right reason, and green without the sabotage. The lock itself has no case:
a fork has to read an entry in the few instructions between the list update and
the narrowing, which one boot does not arrange. Recorded as untested.

The ABI self-test's case that mprotect refuses an unmapped page
(`linux-abi.txt`) had its anchor re-pointed at the new check, found by
`check-sabotage-anchors.py` on the first `check.sh all`.

## Descriptors were copied per thread (M-072)

`clone(CLONE_VM|CLONE_THREAD)` gave the new thread a *copy* of its creator's
table, and the source said so in a comment. So a descriptor one thread opened
did not exist in the others, and a close in one left the rest holding it - the
last of the five process-state copies `core_c5_process_state.md` lists.

The table moved into `hw_procstate_t`, beside the dispositions and the region
list, and a thread shares it by holding the same reference. fork and exec take
a copy (`hw_fds_copy`, every pipe end in it gaining an owner); a thread takes
nothing.

Sharing the table made two things true that were not:

- **Two threads can open at once.** `open` looked for a free slot, then read the
  disk, then marked the slot used - a sibling opening meanwhile found the same
  slot free. Claiming now goes through `files_lock` and happens *before* the
  lookup; a failed lookup gives the slot back. `pipe` claims its two ends the
  same way instead of scanning by hand. `close` takes the entry out of the table
  in one step under the lock and finishes with its own copy, so a second close
  of the same descriptor from another thread gets EBADF instead of releasing a
  pipe end twice. `dup2` reads, replaces and re-references in one critical
  section. The lock is not held across I/O: two threads reading one descriptor
  at the same moment share its position without ordering, a difference from
  Linux recorded in the structure's comment.
- **Who closes the table.** Every exit closed its own copy. Now the table goes
  with the last thread to leave it, counted by `files_users` - apart from the
  process reference, because exit gives that back only after it has switched
  away, and closing a pipe has to happen before. The same change fixed a defect
  nobody had reported: each thread's exit released *all* of the process's
  sockets by thread-group id, so a worker that finished closed the connections
  its process was still using. They now go with the last thread too.

**Proof.** `THREADS_C5_FILES` in `tests/linux/musl_threads.c`: a thread makes a
pipe and main reads from it; then main closes the write end while another
thread waits to write through it, and that write must fail. The gate asserts it
(`thread_descriptors_not_shared`, `threads_c5_files_did_not_report`). Run
against the kernel *before* this change - the test first, the fix stashed - it
printed `THREADS_C5_FILES_FAIL: a pipe a thread opened is not open in main`.
Sabotage (`scripts/dev/cases/fd-shared.txt`): a clone that does not count
itself as a user of the table, and an exec that does not copy it. The lock has
no case, for the same reason as mprotect's.

The first of those went NOT RED at first, and the reason is worth more than the
case. With the increment gone, the first thread the threads test ever creates
takes the count to zero on exit - closing a table that held nothing of interest
yet - and every exit after that wrapped it below zero, so by the time
`THREADS_C5_FILES` ran nothing closed anything and the test passed. The symptom
appears once, wherever the first exit happens to fall; the underflow happens on
every exit after it. So the leave refuses to go below zero and counts the
attempt as `files_double_leave`, on the `[TASKS] MUSTBEZERO` line the gate
already asserts - the mechanism gated, not one lucky symptom.

`check-task-identity.py` asserted the table was embedded in `hw_task_t`; it now
asserts the opposite and that `hw_procstate_t` holds it, and its ratchet on
lines that index the table directly went from 17 to 12.

## rmap_missing_remove was an eviction finishing inside a teardown (M-073)

Running the new threads test against the old kernel was supposed to show one
failure. It showed three, and one of them was `mm_rmap_missing_remove=1` - the
rare counter that had fired once in about fifty boots and had been instrumented
since. The instrumentation named it at once: `RMAP_MISSING` with a caller in
`vibeos_vmspace_swap_out`, and a virtual address inside `svc-reclaim`'s blocks.

That boot's `svc-reclaim` had exited early - the old mprotect refused its first
block - with some 1,400 blocks still mapped, so its teardown ran while the clock
was evicting from it. `vibeos_vmspace_destroy` calls `vibeos_rmap_forget_root`,
which removes every holder recorded for the root *first* and only then waits
for reclaim's claims on it (M-056). A reclaimer that claimed a page before the
teardown finishes its swap-out afterwards: the entry is still there, the commit
succeeds, and its `vibeos_rmap_remove` finds nothing, because the teardown took
it. Nothing is wrong - the frame is released once, by the eviction, and the
teardown frees the slot the entry now names - but the must-be-zero counter
said otherwise.

`forget_root` now marks the claims it waits on, and a remove that finds nothing
for a root with a marked claim outstanding is counted as
`removed_after_forget`, which is printed and is not a must-be-zero. A remove
that finds nothing anywhere else is still `missing_remove`. The host test
`test_eviction_during_teardown` plays the reclaimer from the wait's relax hook,
and the sabotage that stops marking (`mm-rmap-claims.txt`) turns it red. Clearing
the mark when the last claim goes was tried as a case too and went NOT RED: the
mark is read only while a claim is outstanding and a new claim clears it, so the
clearing was redundant and was removed rather than tuned.

Lesson worth keeping from the order of events: the rare defect was not found by
hunting for it. It appeared when a load that normally runs to completion
stopped early and left a large teardown to race the clock - an arrangement no
test had set up on purpose.
