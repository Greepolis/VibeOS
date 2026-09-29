# Syscalls and the Linux ABI: Plan

Every Linux x86-64 syscall, and the refactor of the layer that answers them.
Written to the rules the memory, scheduling and program-loading plans used: the
goal stated as properties that can be checked, invariants before code, and each
phase closed by a program somebody else wrote rather than by a test this project
wrote for itself.

Companion documents: [phases.md](phases.md) (the order and what closes each
step) and [syscalls.md](syscalls.md) (all 373 numbers, each with its state and
its phase).

## 1. The objective

**An unmodified Linux x86-64 program runs, and every syscall it can make has an
answer that was decided rather than defaulted.** Three answers exist and every
number has exactly one:

- **implemented** - with Linux's semantics, its errno values and its structure
  layouts, checked against Linux's own headers and tests;
- **refused by decision** - a fixed errno, a sentence saying why, and a counter
  that says *refused* (loading a kernel module, raw port access, a syscall Linux
  itself removed);
- **deferred** - no program in the corpus asks for it; answered with ENOSYS and
  counted separately, so the day one does, the gate says which.

What is not an answer: ENOSYS because nobody wrote the row. Today that is 302
of the 373.

## 2. Where it stands (2026-09-29)

| | |
| --- | --- |
| Rows in the kernel | 73: 71 Linux numbers and 2 of VibeOS's own (`netctl`, `pageinfo`) |
| Done / partial / missing | 55 / 16 / 302 of 373 |
| What runs | static, PIE and dynamic musl binaries: BusyBox, a shell, threads, signals, TCP |
| Dispatcher | one table-driven engine (C4): each row declares its pointer arguments and the engine validates them before the handler runs; `check-syscall-checks.py` holds the declarations against the call graph |

The table works and is worth keeping. What limits the next three hundred rows is
underneath it:

1. **The handlers are not portable.** `kernel/abi/linux/` includes
   `arch_hw_internal.h` and reaches `g_tasks`, `hw_*` and the page tables directly
   - about 700 references across 4,866 lines. A handler cannot be host-tested,
   so every syscall so far was proved by booting.
2. **Four descriptors.** The table holds descriptors 3 to 6, and an entry is a
   value copied on `dup` and `fork` - so two descriptors that Linux says share an
   offset do not. Most real programs open more than four files; all of them
   expect `dup` to share.
3. **One directory.** There is no working directory: `getcwd` answers `/`, and
   `openat` accepts only `AT_FDCWD`.
4. **Anonymous memory only.** `mmap` refuses file-backed mappings and
   `MAP_FIXED`, which glibc's dynamic loader needs.
5. **No terminal.** `ioctl` answers ENOTTY to everything, so a shell cannot ask
   for its window size or raw mode.
6. **ENOSYS means two things.** A deliberately refused number and a missing one
   look the same to the counter and to the gate.

None of these is a missing syscall. They are why adding syscalls one at a time,
the way the current 73 arrived, would not get far.

## 3. Invariants

The first five were written down in [../roadmap.md](../roadmap.md) before this
plan; they stand.

1. Every syscall argument that names user memory is validated in one place, and
   a refusal says why (`hw_user_range_why`).
2. An `int` argument arrives zero-extended and is compared through
   `VIBEOS_ARG_INT`, never on all 64 bits.
3. A `sigset_t` is converted at the boundary and nowhere else.
4. An unimplemented syscall is refused and counted, never silently succeeded.
5. A wait status carries the signal in the low seven bits and the exit code in
   the high byte, and every producer and consumer agrees.
6. **Every number has one registry entry** - implemented, partial with its gap
   named, refused with a reason, or deferred - and the dispatcher, the number
   file and [syscalls.md](syscalls.md) are generated from or checked against it.
   Two statements of one number disagreeing is a build failure, as it already is
   for the rows.
7. **Missing and refused are different counters.** A corpus program reaching a
   missing number fails the gate; reaching a refused one is expected and counted.
8. **Conformance is measured against somebody else's artefact.** Errno values and
   structure layouts are checked against Linux's uapi headers; behaviour against
   the Linux Test Project and real programs. CLAUDE.md records why: a fixture
   this project writes can only prove the handler matches the fixture.
9. **A handler is portable and host-testable.** It reaches a task, its
   descriptors, its address space and the filesystem through a declared
   interface the architecture implements - not through `arch_hw_internal.h`.
10. **A partial row names its gap in the registry**, and the gap is the same
    sentence the refusal logs, so "partial" cannot quietly become "done".

## 4. Method

The syscalls a program needs are a property of its C library and of what it
does, not of the kernel, so they are measured rather than reasoned about:
`scripts/dev/trace-linux-binary.sh` runs a binary on the host under `strace` and
lists what VibeOS does not serve. Every phase starts from that list for its
target programs, and ends when those programs run under the gate.

The corpus grows by phase (see [phases.md](phases.md) and
[corpus.md](corpus.md)): BusyBox as the boot image stages it (a static glibc
build), the musl test programs, `sqlite3`, `lua`, a small HTTP server, and the
Linux Test Project's syscall tests built against musl. LTP is
the conformance oracle - thousands of cases written by the people who define the
behaviour, and exactly the kind of artefact neither side of this project
controls.

## 5. Boundaries

The dispatcher, the registry, argument validation and the handlers are portable.
The register ABI, the `syscall` trampoline and the frame that `fork` and signal
delivery need are the architecture's. The Linux ABI is a translator onto the
kernel's own operations (`include/vibeos/abi.h`, C4); a Windows or macOS ABI
later is a second translator onto the same operations, which is why A2 declares
the operations and not Linux's names for them.

## 6. What this plan does not do

It does not design a native VibeOS syscall ABI - [../syscalls.md](../syscalls.md)
records that intent, and C4's operation vocabulary is where it would start. It
does not promise every *deferred* number: those wait for a program that asks. And
it does not treat a number count as progress - a phase is done when its programs
run, not when its rows exist.
