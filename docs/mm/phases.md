# Memory Management: Phases P0-P7

*Part of the [memory management rewrite plan](README.md): [architecture](architecture.md) · [observability](observability.md) · [maintainability](maintainability.md) · [phases](phases.md) · [decisions and risks](decisions.md).*
Each phase below is specified to the level where implementing it is mechanical.
Where a choice remains, it is listed in §12 as a decision for you, not for me.

Rules that apply to every phase, without exception:

- The tree is green at every commit: `rc=0`, `warnings=0`, host tests, and both
  disk gates.
- No phase begins before the previous one's definition of done is met and
  demonstrated.
- No behaviour changes outside the phase's stated scope. If something else is
  found to be broken, it is recorded in `docs/`, not fixed in passing.
- Every guard added gets a sabotage case, and the case is run and seen to turn
  the boot red before the phase is called done.
- Documentation is updated in the same commit, per the standing rule.

---

### P0 — The contract, with nothing behind it

**Objective.** Define every interface and every counter, wire the counters to
the *existing* implementation, and change no behaviour.

**Files created**
- `include/vibeos/mm.h` — the L0 and L1 contracts and the invariants of §2 as
  comments next to the functions that must uphold them.
- `include/vibeos/mm_stats.h` — the counter structure of §6.
- `kernel/mm/stats.c` — the counter definitions and one accessor.
- `tests/kernel/mm_stats_tests.c` — host tests for the accessor.

**Files modified**
- `kernel/arch/x86_64/arch_hw.c` — existing counters (`g_cow_shared`,
  `g_cow_copied`, `g_tlb_*`, `g_untracked_frees`) move into the structure; call
  sites updated. Nothing else.
- `kernel/core/kmain.c` — a `meminfo` console command printing every counter.
- `scripts/qemu-cli-smoke-linux.py` — drive `meminfo`; assert the must-be-zero
  counters of §6.
- `CMakeLists.txt`, `cmake/core_sources.cmake` — the new sources.

**Interfaces added**

```c
typedef struct vibeos_mm_stats {
    uint64_t frames_total, frames_free, frames_allocated;
    uint64_t frames_leaked, frames_double_put, poison_hits;
    uint64_t maps, unmaps, cow_shared, cow_copied;
    uint64_t tlb_shootdowns, tlb_acks, tlb_timeouts;
    uint64_t faults_resolved, faults_fatal;
    uint64_t vmas_live;
    uint64_t cache_hits, cache_misses, cache_evictions, cache_writebacks;
    uint64_t swap_ins, swap_outs, swap_used;
    uint64_t reclaim_scans, reclaim_freed;
} vibeos_mm_stats_t;

const vibeos_mm_stats_t *vibeos_mm_stats(void);
void vibeos_mm_stats_reset(void);          /* host tests only */
```

**Steps, in order**
1. Add the two headers with the structure and the documented invariants.
2. Add `kernel/mm/stats.c` and the host test; `ctest` green.
3. Replace the existing globals with fields of the structure, one at a time,
   building after each.
4. Add `meminfo` to the console and to the gate's command sequence.
5. Add the gate assertions for `frames_leaked`, `frames_double_put`,
   `poison_hits`, `tlb_timeouts` — all must be zero.

**Out of scope.** Any change to allocation, mapping or teardown behaviour.

**Tests**
- Host: `mm_stats_tests.c` — accessor returns a stable pointer, reset zeroes.
- Gate: `meminfo_missing`, `mm_counter_nonzero:<name>` for the four above.
- Sabotage: `scripts/dev/cases/mm-stats.txt` — remove the `meminfo` command;
  make `frames_leaked` non-zero artificially. Both must turn the boot red.

**Rollback.** Revert the commit; nothing depends on it yet.

**Done when.** `meminfo` prints all counters with plausible values; the four
assertions exist and their sabotage cases have been seen red; the boot is
byte-for-byte equivalent in behaviour (same phases, same markers).

---

### P1 — The frame layer

**Objective.** One owner for physical memory. Nothing outside `kernel/mm/frame.c`
touches the free list or a reference count.

**Files created**
- `kernel/mm/frame.c` (~400 lines expected), `include/vibeos/frame.h`
- `tests/kernel/frame_tests.c`

**Files modified**
- `kernel/arch/x86_64/arch_hw.c` — `hw_alloc_page`, `hw_free_page`,
  `frame_ref_slot`, `hw_page_get/put/owners`, the poison and the
  free-while-mapped detector all move out. The names remain as thin wrappers
  during the phase and are deleted at the end of it.

**Interfaces added** — as in §5 L0, plus:

```c
int      vibeos_frame_init(uint64_t base, uint64_t len);
uint64_t vibeos_frame_alloc(vibeos_frame_state_t state);
void     vibeos_frame_get(uint64_t phys);
int      vibeos_frame_put(uint64_t phys);
uint32_t vibeos_frame_owners(uint64_t phys);
vibeos_frame_state_t vibeos_frame_state(uint64_t phys);
int      vibeos_frame_reserve(uint64_t base, uint64_t len);
```

**Steps, in order**
1. Write `frame.c` with the descriptor array of §5, the free list, poison, and
   the counters from P0. No callers yet. **Done** (`bec59e3`).

   One correction to the design landed here: the poison is written on *release*,
   never at init. Initialising over a region whose first megabytes are already
   live would destroy them. `VIBEOS_FRAME_WAS_FREED` marks the frames the poison
   check may judge, so a never-freed frame is not reported as corrupt - see
   `12a1e19`.
2. Host tests first: alloc/free round trip, get/put arithmetic, double-put
   refused, out-of-range refused, poison verified on reuse, reserve excludes a
   range, and a never-freed frame full of garbage that must *not* be flagged.
   Green before any kernel change. **Done**; each was confirmed to go red by
   breaking the code it protects.
3. **Blocked on D9** — the ownership contract of an allocation. The old
   allocator hands back a frame with zero owners and lets the first mapping make
   it one; the new layer hands back one owner. Wiring the wrappers without
   settling that leaks every user page or double-frees every page table, and the
   mismatch is silent either way. See `decisions.md` D9; the recommendation is
   option A. Nothing in step 3 starts before that is answered.

   Then: point `hw_alloc_page`/`hw_free_page` at the new layer as one-line
   wrappers. Boot; `meminfo` numbers must match P0's.

   Two things step 3 also has to get right, found while reading the allocator:
   - The layer comes up *after* the bump allocator has already handed out the
     early page tables and the descriptor table itself. Those frames must be
     reserved, not free - `vibeos_frame_reserve(base, pmm.offset_bytes)` covers
     exactly the prefix a bump allocator has consumed.
   - The old allocator refused any frame above `VIBEOS_HW_IDENTITY_LIMIT`, since
     it cannot address one. Reserve that tail rather than letting `alloc` return
     a frame nobody can write.
4. Replace call sites of the wrappers with the real names, file by file.
   **Deferred, deliberately.** Steps 3 and 4 could not stay separate: the two
   contracts for what an allocation returns are incompatible, so the call sites
   had to change meaning in the same commit that changed where the arithmetic
   happens. Renaming them as well would have put three kinds of change in one
   diff, in the subsystem whose whole history is exactly that. The wrappers keep
   their names and now carry a comment saying so; the rename lands with P2,
   which touches these call sites anyway.
5. Delete the wrappers. Add the layering check for `g_free_pages`. `g_free_pages`
   is gone; the wrappers stay until P2 for the reason above.

**Status: done** (`29a6e29`), with steps 4 and 5 folded into P2 as recorded.
`check.sh all` green and 8 boots out of 8.

**Out of scope.** Page tables, address spaces, the two windows. `PTE_OWNED` is
not introduced here.

**Added during the phase, and not in the original plan**
- `vibeos_frame_alloc_contig`, because the bump allocator had to stop serving
  the callers that needed contiguous memory rather than merely stop being
  preferred. See decisions.md D9.
- `vibeos_frame_survey`, one walk giving the state histogram and the longest
  free run, so `meminfo` has a single source and fragmentation is a number.
- The bump allocator is closed, not just unused. It was still serving the GUI
  back buffer after the frame layer had adopted the region.

**Tests**
- Host: the seven cases in step 2, each with a negative form.
- Gate: `frames_total` covers the whole allocator region; `frames_free` non-zero
  at `USERLAND_START` and again at `USERLAND_DONE`.
- Sabotage: `scripts/dev/cases/mm-frame.txt` — make `vibeos_frame_put` free at
  a non-zero count; make it accept an out-of-range frame; remove the poison.
  Three cases, each must turn the boot red.

**Rollback.** The wrappers are the rollback: revert steps 4-5 and the old
implementation is still reachable.

**Done when.** `check-mm-layering.sh` reports no free-list access outside
`frame.c`; host tests cover every branch of get/put; 24 boots with
`frames_leaked` and `frames_double_put` zero.

---

### P2 — Address spaces, and the ownership bit

**This is the phase that repairs the defect.** Everything before it is
preparation and everything after it is new capability.

**Files created**
- `kernel/mm/vmspace.c` (~700 lines expected), `include/vibeos/vmspace.h`
- `tests/kernel/vmspace_tests.c` — with a fake page-table backend, so the
  walking logic is host-tested even though the hardware format is real.

**Files modified**
- `kernel/arch/x86_64/arch_hw.c` — `hw_map_page`, `hw_map_low_user_page`,
  `hw_aspace_create/destroy`, `hw_aspace_copy_user`, `hw_aspace_copy_low_user`,
  `hw_share_user_leaf`, `hw_handle_cow_fault`, `hw_sys_munmap`,
  `hw_sys_mprotect` are replaced by calls into the new layer.

**Interfaces added** — as in §5 L1.

**The ownership bit.** `PTE_OWNED = 1ull << 9` (bit 9, software-available on
x86-64). Set only by `vibeos_vmspace_map`. Cleared only by `unmap`/`destroy`,
which release the reference. The identity-split entries never carry it; a
`PROT_NONE` guard page always does.

**Steps, in order**
1. `vmspace.c` with `create`, `destroy`, `map`, `unmap`, and the two-window
   logic folded into `map`. Host tests against the fake backend.
2. Introduce `PTE_OWNED`; `map` sets it, `unmap` and `destroy` release exactly
   the entries carrying it. Boot. `FREE_WHILE_MAPPED` must be silent.
3. Move `protect` in. Verify the `PROT_NONE` thread-stack path still works
   (`THREADS.ELF`, `TFORK.ELF`).
4. Move `clone_cow` in — this is fork. Verify `cow_shared`/`cow_copied`.
5. Move `fault` in — this is the copy-on-write resolution and the stale-TLB
   case. Verify with the stress service.
6. Move the TLB shootdown decision inside `map`/`unmap`/`protect`.
7. Delete the old functions. Add the layering check for page-table writes.

**Out of scope.** Regions, backing stores, reclaim. `munmap` still walks in this
phase — it is rewritten in P3 — but it walks *through* `vmspace_unmap`.

**Tests**
- Host: map/unmap round trip, ownership bit set and cleared, identity entries
  never released, `clone_cow` reference arithmetic, double-unmap refused.
- Gate: `FREE_WHILE_MAPPED` absent; `maps` and `unmaps` converge at
  `USERLAND_DONE`; `cow_shared` and `cow_copied` both non-zero.
- Sabotage: `scripts/dev/cases/mm-vmspace.txt` — clear `PTE_OWNED` in `map`;
  release entries without it in `destroy`; skip the reference in `clone_cow`;
  drop the shootdown in `protect`. Four cases, each red.

**Rollback.** Steps 2-6 are individually revertible; each is a separate commit.

**Done when** (revised 2026-08-31, and the revision matters more than the
criterion). No page-table write outside `kernel/mm/`; across **48 boots**, no
`FREE_WHILE_MAPPED`, no stress-service defect, and `frames_leaked`,
`frames_double_put` and `poison_hits` all zero with the frame states summing to
the total; the sabotage cases seen red.

The original wording was "48 boots with no failure", full stop. That was wrong,
and not by a little: the machine has a pre-existing intermittent boot failure of
roughly 8% - see
[boot_repeatability.md](../implementation_progress/boot_repeatability.md) -
which makes 48 consecutive clean boots about a 0.02% event. No revision of this
project has ever met it, before or after this work, and the nightly gate that
applies the same rule to five boots has failed 27 times out of 31 since it was
created.

So the criterion was measuring the background rather than the phase. It is now
written in terms of what P2 is actually responsible for: the memory manager's
own invariants, asserted over a sample large enough to catch a rare violation.
Whether the machine boots cleanly every time is a real question and it now has
its own file, because it is a different defect with a different history.

**The rule this cost two days to learn: check a criterion against the baseline
before using it to judge a change.** A single 24-boot run of the parent commit
would have settled it, and was not taken until it was asked for.

**Status: done**, against the criterion above. Three concurrency defects found
and fixed, all the same shape - two cores disagreeing about a page-table entry -
and every one of them settled by experiment, because reading the code reached
the wrong answer six times.

The boot rate went from 27/48 while those defects were live to the ~90% that
`f45ab1d` (the commit before P2), `a714dbe` (step 2) and the finished phase all
measure equally. That equality is the evidence that what remains is not P2's.

### The three defects

1. **Publication order** (`e02d9dd`). `map_raw` stored the entry and then took
   the reference, so a mapping existed that no count knew about. The big one:
   no `STRESS_FAIL` has appeared in the ~150 boots since.
2. **Release order**, in the fault and in teardown: releasing a frame while the
   entry still pointed at it.
3. **Double resolution** (`6510827`). Two threads faulting on one page both
   copied and both released the shared frame. Every store to an entry is a
   compare-exchange now, so the core that loses discovers it instead of
   overwriting the winner.

### What was learned about the detectors, which is worth as much

The free-while-mapped detector lived in the architecture's free path, and after
this rewrite almost nothing frees a frame through there - the address-space
layer releases directly. It was watching a door nobody walks through, and its
silence was being read as evidence. Moved to the frame layer's last release, it
fired immediately.

It then needed three predicates before it was worth trusting: a prediction
(`owners == 1`, taken while other cores map and unmap) reported frames that
were never freed; "is it still free" dropped the real cases where another core
won the race; **more mappings than references** is the invariant itself and is
indifferent to both. That last one produced `mappers=2 owners=1`, which is the
fact that named the third defect.

### Where to pick up the rest

Three defects were found and fixed, all of them the same shape - two cores
disagreeing about a page-table entry - and all three confirmed by experiment
rather than by reading:

1. **Publication order** (`e02d9dd`). `map_raw` stored the entry and then took
   the reference, so a mapping existed that no count knew about. This was the
   big one: no `STRESS_FAIL` has appeared in the ~100 boots since.
2. **Release order** in the fault and in teardown: releasing a frame while the
   entry still pointed at it.
3. **Double resolution** (`6510827`). Two threads faulting on one page both
   copied and both released the shared frame. Every store to an entry is a
   compare-exchange now, so the loser discovers it.

**What is still open.** About one boot in twelve, and it no longer looks like
the original defect:

- `svc-flap` takes a not-present *instruction fetch* with `cr2` equal to `rip`,
  inside the same page it was already executing from - a page that vanished
  under a running process. No detector has caught the moment yet.
- A wedge, roughly one boot in twenty-four, at varying phases.

Both may predate P2: CLAUDE.md already records an intermittent wedge and a
`ping` hang from before this work. **That is the first thing to establish** -
run `scripts/dev/bisect-boot.sh a714dbe 24` (P2 step 2, which measured 16/16)
and see whether these two signatures appear there too. If they do, they are not
P2's to fix and the phase can close on its own terms.

**Tools that now exist for it**, all of which earned their place today:
`repeat-boot.sh` keeps the log of every failed boot; the free-while-mapped
detector sits on the frame layer's last release and compares mappings against
references; `vibeos_vmspace_current_op()` names the operation in the report;
`bisect-boot.sh` builds a revision and always puts the tree back.

The code is written and the six commits are in (`87134d7` through `b2d6d7f`).
Fifteen sabotage cases, all red. No page-table write and no frame reference
taken outside `kernel/mm/`, checked on every build. The host tests, the torture
run at 150 seeds, and single boots are all green.

None of that is the phase's own criterion, and the criterion says no. This
section stays open until it passes.

**What failed, from the three verdicts the run preserved**

- `phase=userland_running`, wedged, quiet for 45s (boot 44).
- `deliberate_ring3_faults=3_expected=1, unexpected_cpu_fault,
  stress_run_found_a_defect` (boot 47). The stress service found something,
  which is the detector built for exactly this doing its job.

Twenty-one boots failed and only the last boot's log survived - and it had
passed. `repeat-boot.sh` did not keep the logs of failed boots; it does now.
The next run starts with evidence instead of a count.

**What this is not.** It is not the old premature-free family showing through:
`FREE_WHILE_MAPPED` is silent, the three MUSTBEZERO counters are zero on every
boot that reached the console, and the torture run compares against an
independent model at 150 seeds without a disagreement. Whatever this is, it was
introduced between P2 step 2 - which was 16 boots out of 16 - and step 7. That
range is five commits, each individually revertible, which is the reason D4
chose six commits over one.

**What the phase cost that the plan did not predict**

- **The ownership bit had to move.** The plan said bit 9; `PTE_COW` is 0x200,
  which is bit 9. See decisions.md D3. A `_Static_assert` now answers it.
- **`vibeos_vmspace_map_raw`.** Fork installs mappings carrying the
  copy-on-write mark, and the portable protection type has no word for it.
  Widening `vibeos_prot_t` with an x86-64 detail was the wrong trade; the
  architecture passes the leaf it wants and the layer still adds the ownership
  bit and takes the reference. It can go when P5 moves copy-on-write in.
- **The memory lock moved into L0.** Step 5 wedged the boot: the layer
  allocates a frame, and until then every allocation had gone through an arch
  wrapper that took the lock. A layer several cores can drive has to defend
  itself - "remember to hold the lock" held for exactly one phase.
- **Two behaviours were fixed rather than moved**, both in the same family and
  both reachable from an ordinary program:
  - `mprotect` used to grant write access to a copy-on-write page, letting a
    forked process write into a page its parent was still reading.
  - `fork` selected low-window pages by `PTE_USER`, so a child of a threaded
    process inherited an address space with holes where its `PROT_NONE` thread
    guards belonged. `cow_shared` rose from 0xbd2 to 0xc09 per boot when that
    was fixed - fifty-five pages that had never been cloned.
- **Steps 4 and 5 of P1 folded in here**, as recorded at the time. The wrapper
  names remain; they now wrap one call each.

---

### P3 — Regions

**Budget (decision D5).** This phase may cost up to 10% of boot time and no
more, and the boot gate measures it rather than trusting a reading of the code.
Whole-system optimisation is a much later milestone; this number exists only to
stop a phase making things quietly worse in the meantime.


**Objective.** `mmap`, `munmap`, `mprotect` and `brk` operate on descriptors of
what was asked for.

**Files created**
- `kernel/mm/vma.c`, `include/vibeos/vma.h`, `tests/kernel/vma_tests.c`

**Interfaces added** — as in §5 L2, plus `insert`, `remove`, `find`, `split`,
`merge`, `clone`.

**Steps, in order**
1. `vma.c` as a pure data structure with host tests: insert, find, split at an
   address, merge adjacent compatible regions, reject overlaps, clone a list.
2. Attach a region list to `hw_proc_t`; populate it from `mmap` and `brk`
   without yet using it for anything.
3. Rewrite `munmap` to consult the list rather than walk.
4. Rewrite `mprotect` likewise; guard pages become regions with no backing.
5. `clone` the list in fork; free it on exit. `vmas_live` must return to zero.

**Out of scope.** File backing. Regions carry a backing *kind* in this phase,
and only `ANON` is implemented.

**Tests**
- Host: nine cases covering split, merge, overlap, clone, exhaustion.
- Gate: `vmas_live` zero at `USERLAND_DONE`; ABI self-test unchanged.
- Sabotage: `scripts/dev/cases/mm-vma.txt` — skip the split on partial unmap;
  leak the list on exit; allow an overlapping insert. Three cases, each red.

**Done when.** `munmap` contains no page-table walk; `vmas_live` returns to
zero; host tests cover the nine cases; 24 boots green.

---

### P4 — Page cache

**Objective.** A fault can be resolved from a file, and the same file read twice
is read from disk once.

**Files created**
- `kernel/mm/backing_anon.c`, `kernel/mm/backing_file.c`,
  `include/vibeos/backing.h`, `tests/kernel/backing_tests.c`

**Steps, in order**
1. The `vibeos_backing_ops_t` interface and the anonymous implementation, which
   is what P3's regions already do implicitly.
2. The file implementation with a hash keyed on (file id, offset).
3. `execve` and the ELF loader fault pages in instead of copying through the
   4 MiB staging buffer.
4. Merge `kernel/fs/`'s block cache into this one rather than running both.

**Out of scope.** Writable file mappings and write-back. `MAP_SHARED` is
refused with `ENOSYS` in this phase and implemented in a later one.

**Tests**
- Host: hit, miss, eviction, key collision.
- Gate: `cache_hits` and `cache_misses` both non-zero; running BusyBox twice
  shows the second exec hitting; the staging buffers are gone from `meminfo`.
- Sabotage: `scripts/dev/cases/mm-cache.txt` — never record a hit; return the
  wrong page for a key; skip eviction. Three cases, each red.

**Done when.** The staging buffers no longer exist; the hit ratio on a normal
boot is asserted, not merely non-zero; 24 boots green.

---

### P5 — Swap

**Objective.** A workload larger than physical memory completes.

**Files created**
- `kernel/mm/backing_swap.c`, `kernel/mm/swapmap.c`,
  `tests/kernel/swap_tests.c`

**Steps, in order**
1. A swap map over a block device: allocate, free, read, write a slot.
2. Page-out: pick a frame (P6 chooses well; here it is FIFO), write it, record
   the slot in the region, unmap it.
3. Page-in on fault.
4. A stress operation that allocates beyond memory and verifies every byte,
   with the seed printed as usual.

**Out of scope.** Choosing *which* page to evict beyond FIFO — that is P6.

**Tests**
- Host: swap map allocation, exhaustion, round-trip fidelity.
- Gate: `swap_ins` and `swap_outs` non-zero; the new stress operation green.
- Sabotage: `scripts/dev/cases/mm-swap.txt` — corrupt one byte on write-back;
  lose the slot on page-out; double-free a slot. Three cases, each red.

**Done when.** A program needing more than free memory completes; data through
swap is byte-identical; 24 boots green.

---

### P6 — Reclaim policy

**Objective.** Pressure is survivable and the eviction order is deliberate.

**Files created** — `kernel/mm/reclaim.c`, `tests/kernel/reclaim_tests.c`

**Steps, in order**
1. LRU lists using the fields reserved in the frame descriptor at P1.
2. Watermarks: a low mark that starts reclaim, a minimum that blocks allocation.
3. Eviction order: clean cache, then dirty cache written back, then anonymous to
   swap. Pinned frames never.
4. Pinning for page tables, DMA buffers and kernel allocations.
5. **Compaction.** Reclaiming free memory is not the same problem as making it
   *usable*: `vibeos_frame_alloc_contig` is first fit, so after a few hours of
   allocation and release there can be plenty free and no run of it long enough
   for a 4 MiB staging buffer or a DMA descriptor. Free frames are recovered by
   P6 steps 1-3; contiguous *space* is only recovered by moving frames that are
   still in use and repointing whatever maps them.

   Deliberately last, and it cannot move earlier. Moving a frame means finding
   every page-table entry that points at it, and that is only answerable once
   ownership is recorded rather than inferred - which is what L1 and L2 build at
   P2 and P3, and precisely what today's kernel cannot do. Attempting it before
   then would mean scanning every address space on every move and trusting the
   result, which is the same "reconstruct the truth from the hardware bits"
   mistake this rewrite exists to end.

   The order is: a reverse map from frame to the mappings that hold it (the
   `backing` field in the descriptor is reserved for its handle), then a move
   that copies the frame, updates every mapping, shoots down the TLB, and only
   then releases the old frame. Pinned frames - page tables, DMA, anything a
   device has an address for - are never moved, which is the same list as step 4.

   Measured, not asserted by eye: `frag_largest_run` and `frag_free_frames` in
   the stats, so "there is memory but not in one piece" is a number somebody can
   read in `meminfo` before it becomes an allocation failure nobody can explain.

**Tests**
- Host: list ordering, watermark transitions, pinned frames skipped; a
  compaction that moves a frame, with every mapping following it, and a pinned
  frame refusing to move.
- Gate: `reclaim_scans`/`reclaim_freed` non-zero under the pressure test; the
  order asserted by which counter moves first.
- Sabotage: `scripts/dev/cases/mm-reclaim.txt` — evict a pinned frame; evict
  dirty before clean; never write back; compact a frame and skip one of its
  mappings; compact a pinned frame. Five cases, each red.

**Done when.** The exhaustion workload from P7 completes through reclaim rather
than failing; the order is asserted; and a run that fragments memory
deliberately can still satisfy a 4 MiB contiguous request afterwards, which is
the only honest test of compaction.

---

### P7 — Acceptance: the properties of §2, demonstrated

The phases above build the thing; this one proves it is ready, and it is not
optional. Each item is a test that did not exist before.

- **Exhaustion.** A service that allocates until `vibeos_frame_alloc` fails,
  then exits cleanly. The machine must survive, the counters must return to
  their starting values, and nothing may panic.
- **Fault injection.** A build-time switch that fails the *n*th allocation, run
  across a sweep of *n*. Every failure must leave counts unchanged and no
  mapping behind - this is how invariant 5 is proved rather than asserted.
- **Soak.** A long run - the stress service with its round count raised by two
  orders of magnitude - after which `frames_free` matches its value at
  `USERLAND_START`. A leak of one frame per fork is invisible in 120 rounds and
  obvious in 12000.
- **Isolation.** A test that allocates, writes a pattern, frees, reallocates,
  and asserts it never reads the previous tenant's data or the kernel's.
- **Latency.** No path reachable from a syscall waits for another core without
  a bound; the bound is asserted and its timeout counter must be zero.

*Done when:* all five run in CI, each has a sabotage case that turns it red, and
the invariants of §2 are each mapped to the test that proves it.

