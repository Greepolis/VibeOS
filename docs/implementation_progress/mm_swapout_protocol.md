# Swap-out against everything else (M-056)

Status, 2026-09-24: **fixed, and proved on the host only.** Host-tested and
sabotage-verified; the machine boots and soaks green with it - but **no run on a
machine has ever reclaimed an anonymous page**, before this change or after it.
See "How it is proved" and "What it does not cover".

## The report, and what reading the code found

An external review said, in one line, that `vibeos_vmspace_swap_out` "does not
yet have the final re-verification CAS". The line alone did not say what the
exchange should protect. Reading the code, the answer was larger than the
report:

Reclaim runs from the allocation path - `hw_alloc_page_admitted`, on any core,
from any allocation, possibly while the allocating process holds its own locks -
and it evicts a page from an address space that is *not its own*, holding none
of that process's locks. So three things could happen to the victim while its
page was being evicted, and nothing stopped any of them:

1. **It exits.** Teardown frees the page tables the eviction is walking, and the
   eviction's compare-exchange then writes into a page the allocator has handed
   to somebody else.
2. **It unmaps the page.** munmap saw an entry that already said "swapped", so it
   freed the slot - while the eviction was still writing into it, for the next
   eviction to be given.
3. **It touches the page.** The entry said "swapped" before the write, on the
   argument that a store would fault and be "delayed rather than lost". It was
   not delayed: the fault found a swapped entry and read the slot - before the
   write had put anything in it.

The anonymous tier's own state (`g_hand`, its counters) was also unlocked, with
more than one core able to reclaim at once.

None of this was reachable in the boot gate: its boot never comes under enough
pressure to reclaim (`anon_scanned=0`). The nightly soak was believed to drive
reclaim and does not either - see the last section - and no host test had ever
called `vibeos_anon_reclaim` at all.

## The protocol now

**Claims keep the victim's page tables alive** (`vibeos_rmap_claim_sole`,
`vibeos_rmap_unclaim`). Reclaim takes the frame's holder only if there is exactly
one, and records a claim on that holder's root under the rmap lock - the lock
teardown takes first. `vibeos_rmap_forget_root`, which teardown calls before it
frees anything, removes the root's holders (so no new claim can start) and then
waits for the claims already taken. The wait goes through a hook the arch
supplies, which bounds it with a named panic like every other wait here. A
deadlock was ruled out before this was written: teardown never runs on the
address space it destroys, so the evicting core's shootdown targets nobody the
waiting core is blocking.

**The page stays present while it is written.** A writable page loses its write
bit and gains `VIBEOS_PTE_SWAPOUT` (bit 52), and every core is told; a read-only
page is left alone. The page is written. Then one compare-exchange, from exactly
the marked entry to the swapped one, commits. Anything that touched the entry in
between makes it fail and the swap-out is abandoned with nothing lost:

| Meanwhile | What happens |
| --- | --- |
| a store | faults; the fault gives the write bit back (`swap_out_cancelled`); the commit fails |
| munmap | clears the entry; the commit fails; no entry ever named the slot |
| fork | converts the marked page to copy-on-write in both; the commit fails |
| mprotect | rebuilds the entry without the marker; the commit fails |
| compaction | moves the frame; the commit fails |

The marker is honoured in the three places copy-on-write taught this kernel
about: the fault handler (cancel, user or kernel mode), `hw_user_range_ok`
(writable - a syscall writing a buffer being evicted must not get EFAULT), and
every path that rebuilds an entry. Two of those rebuilds were subtle enough to
be wrong without a test: mprotect keeping the marker would give a write bit back
to a page made read-only (CLAUDE.md's bit-9 defect again), and fork sharing a
marked page "as it stands" would leave the child read-only without
copy-on-write, killed on its first store.

## How it is proved

- `tests/kernel/rmap_tests.c`, `test_claims`: only a sole holder is claimed; a
  teardown with no claim does not wait; one with two claims waits until both are
  released; a released root cannot be claimed; an unmatched unclaim is counted;
  a full table refuses.
- `tests/kernel/compact_tests.c`: six groups in which the in-memory swap disk
  runs "another core" in the middle of the page's write - a look, a store, an
  mprotect, an munmap, a fork - plus a read-only page; and the first host test of
  `anon.c`, which evicts through the claim on both the refused and the evicted
  path and then tears the owner down without waiting.
- Sabotage: `mm-swapout-protocol.txt` (6), `mm-rmap-claims.txt` (3),
  `mm-anon-claims.txt` (2) - all eleven red, each on its own named check.
- Two re-pointed cases in `mm-vmspace.txt` (protect drops the ownership mark;
  fork leaves the parent writable), whose anchors this change moved - red again
  on their own checks.
- The machine: `check.sh all` green (gcc and clang, warnings 0, boot), and the
  soak at 120 and 12,000 rounds green with nothing lost per round. That shows
  the change breaks nothing that runs. It does not show the protocol working on
  a machine, because of the next section.

## What it does not cover

**Anonymous reclaim has never run on a machine.** The soak was expected to be
the run that exercises it - the nightly job is called "Memory + Swap Soak" - and
it is not: at 12,000 rounds it reports `anon_scanned=0 freed_anon=0`, the same
as the boot gate. The machine never falls below its low watermark, so
`vibeos_reclaim_run` is never called, and the swap area's only traffic is the
eight-sector self-test round trip. Everything above about the protocol on a
machine - the fault handler's cancel in ring 0 and ring 3, the arch's bounded
wait, `hw_user_range_ok` - is therefore correct by review and by the host tests,
not by a run. A workload that drives the machine into reclaim is what would
change that, and it is the next thing this area needs.

Found by reading the soak's own output for the counters this change added,
which read zero; the soak's verdict line has no field for reclaim and was green
throughout.
