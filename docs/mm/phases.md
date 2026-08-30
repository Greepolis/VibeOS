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
5. Delete the wrappers. Add the layering check for `g_free_pages`.

**Out of scope.** Page tables, address spaces, the two windows. `PTE_OWNED` is
not introduced here.

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

**Done when.** No page-table write outside `vmspace.c`; **48 boots** with no
`FREE_WHILE_MAPPED`, no stress failure and no unexpected ring-3 fault; the four
sabotage cases seen red.

---

### P3 — Regions

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

