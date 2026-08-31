# Program Loading: Phases

Same rules: host tests and sabotage cases land with each phase, criteria are
stated in terms the phase controls, and any boot count is a ratio against the
same measurement on the parent commit - see
[boot_repeatability.md](../implementation_progress/boot_repeatability.md) for
why a bare count cannot be a verdict here.

---

## X-P0 — Say what happens

**Objective.** A failed exec explains itself before anything is restructured.

**Steps**
1. One counter per refusal reason: not found, short read, bad header, no
   interpreter, out of memory, image too large.
2. One log line per refusal, with the path and the step. Today several refusals
   return `-1` silently and the caller reports "cannot load".
3. Gate assertion: the deliberate refusals a boot performs are the only ones.

**Done when.** Every `return -1` in the exec path is preceded by exactly one
line naming the path and the reason.

---

## X-P1 — The parse comes out

**Objective.** Everything that is a data transformation is portable and tested.

**Files created** — `kernel/exec/image.c`, `include/vibeos/image.h`,
`tests/kernel/loader_tests.c`

**Steps, in order**
1. Segment layout: given headers and a bias, produce the list of (vaddr, len,
   prot, file offset) a loader must satisfy. Pure function, host-tested,
   including the cases that have bitten: holes between segments, a segment whose
   memory size exceeds its file size, a segment ending mid-page.
2. The startup block builder: argv, envp and the auxiliary vector, with
   `AT_ENTRY` staying the program's own entry when an interpreter starts - the
   rule a dynamic program depends on and which is currently a comment.
3. Point the existing loader at both; delete the inline versions.

**Out of scope.** Reading the file, mapping anything.

**Tests**
- Host: nine layout cases, plus a startup block decoded back and compared.
- Sabotage: `scripts/dev/cases/exec-image.txt` - lose the BSS tail; overlap two
  segments; put the interpreter's entry in `AT_ENTRY`.

---

## X-P2 — Load by mapping, and the staging buffers go

**This is the phase that repairs the defect, and it is memory-plan P4 step 3.**

**Files created** — `kernel/exec/load.c`

**Steps, in order**
1. A segment becomes a region backed by the file: `vibeos_vma_insert` with
   `VIBEOS_BACKING_FILE`, the file's id, and the segment's offset. The region
   layer already carries all three and advances the offset correctly on a
   split - that was tested at memory P3 precisely for this.
2. The fault handler resolves a file-backed region through
   `vibeos_cache_get`, which already exists and is already asserted by the boot
   gate at a hit ratio.
3. `execve` stops reading whole files. The ELF *headers* are still read eagerly
   - they are needed to decide anything - but the segments are not.
4. Delete `g_exec_elf` and `g_interp_elf`, and the 6 MiB they hold.

**Out of scope.** Writable file mappings and write-back, which stay with the
memory plan.

**Tests**
- Host: a fault against a file-backed region returns the right page; a region
  split mid-file still faults to the right offset.
- Gate: the staging buffers are gone from `meminfo`; a re-exec of the same
  program is served from the cache; no disk read happens under `g_exec_lock`.
- Sabotage: `scripts/dev/cases/exec-load.txt` - fault in from the wrong offset;
  keep the staging buffer alive; read a segment eagerly.

**Done when.** `g_exec_elf` and `g_interp_elf` do not exist; the boot's exec
path performs no read with interrupts masked.

---

## X-P3 — All-or-nothing

**Objective.** A failure at any step leaves the caller running its old image.

**Steps, in order**
1. Fault injection at each allocation and read point, driven by a boot-time
   seed like the memory stress service.
2. The unwinding made structural: the new address space and region list are
   built beside the old ones and swapped in one step.
3. The outgoing address space released through the single shared ownership test.

**Done when.** The injection service completes every seed with the parent still
running its original program; counters return to their starting values.

---

## X-P4 — Acceptance

- **Concurrent exec.** Two processes exec different programs at once, repeatedly.
  Today they share two global buffers.
- **A truncated program is refused**, not parsed - the failure that once made a
  short file look complete.
- **Re-exec is cheap**: the second exec of a program reads nothing from disk.
- **The interpreter substitution is one function**, marked as a stand-in, with a
  test that fails if a second hard-coded path appears.
