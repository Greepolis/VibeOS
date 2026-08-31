# Program Loading: Rewrite Plan

The subsystem that turns a path into a running process. Written to the same
rules as the memory manager's plan, and second in order because **it is what
blocks the memory plan's P4 step 3**: `execve` cannot fault pages in while it
is a single function that reads a whole program into a staging buffer.

Companions: [architecture.md](architecture.md), [phases.md](phases.md),
[decisions.md](decisions.md).

## 1. Why this is a rewrite and not a fix

`execve` today is one long function inside an 8312-line file. In sequence it:
reads a whole ELF off the disk into a 4 MiB staging buffer under a lock that
masks interrupts; parses it; maybe reads a second file, the interpreter, into a
second 2 MiB buffer; creates an address space; maps every page of both images by
copying out of those buffers; builds the startup block on the stack; destroys
the outgoing address space; and rewrites the task's registers.

Every one of those steps has produced a defect, and the reasons rhyme:

- The 2 MiB read under `g_exec_lock` was indistinguishable from a hang until the
  block driver learned multi-sector transfers. Interrupts are off for the
  duration.
- A failed FAT lookup returned the end-of-chain marker, so a flaky sector
  produced a short file that claimed to be complete - and `execve` parsed
  whatever the previous program had left in the shared staging buffer.
- `execve` destroyed the outgoing address space without asking whether sibling
  threads still ran on it. `hw_task_exit` had asked that for months; the two
  live two thousand lines apart.
- The interpreter path is translated by string comparison against one hard-coded
  name, because FAT has neither the directory nor a name that long.

The staging buffers are the centre of it. They exist because loading is a copy
rather than a mapping, and they are shared global state that a partly-failed
load leaves dirty for the next caller.

## 2. The goal: production ready

| Property | What it means here | How it is proved |
| --- | --- | --- |
| **No shared staging state** | two execs at once cannot see each other's bytes | the buffers are gone; a concurrent-exec test |
| **Loading is mapping, not copying** | a page of a program is faulted in from the page cache | `cache_hits` dominate on a re-exec; the 4 MiB buffer no longer exists |
| **Bounded latency** | nothing reads a disk with interrupts masked | no I/O under `g_exec_lock`; the lock's scope asserted |
| **All-or-nothing** | a failed exec leaves the caller exactly as it was | fault injection at each step; the process still runs its old image |
| **Truthful about short reads** | a file that cannot be fully read is refused, not parsed | the byte count is checked against the header, not against the directory |
| **Isolating** | a program never sees a previous program's bytes | frames zeroed or fully written; the tail page of a file zero-filled |
| **Observable** | a failed exec says which step failed and why | one log line per refusal, with the path and the reason |
| **Host-testable** | ELF parsing, segment layout and the startup block are portable | `tests/kernel/loader_tests.c`; today only `vibeos_elf_*` is tested |

### Invariants

1. No global buffer holds program bytes between two calls.
2. A page of a program image is produced by the backing layer, not copied out of
   a buffer.
3. An `execve` that fails at any step leaves the calling process running its
   previous image, with its address space and regions unchanged.
4. A short read is a refusal. The size a directory claims is never trusted over
   the bytes actually delivered.
5. The outgoing address space is released by whoever holds the last reference,
   asked through the one shared test.
6. Nothing reads a disk while holding a lock that masks interrupts.
7. Every refusal is logged once, with the path and the step.

## 3. Design goals

1. **Load by mapping.** A segment becomes a region backed by the file, and its
   pages arrive through the page cache. This is memory-plan P4 step 3, and it is
   the reason this plan exists.
2. **The parse is portable and pure.** Header, program headers, segment layout
   and the startup block are data transformations; they belong where they can be
   tested without a machine.
3. **One place decides the layout.** Which window, what protection, where the
   interpreter goes, and what the stack looks like.
4. **Failure unwinds.** The new address space is built beside the old one and
   installed only when it is complete - which is already how it works, and the
   plan is to keep it structurally rather than by care.
5. **The interpreter substitution is named as a stand-in.** It exists because
   FAT cannot hold `/lib/ld-musl-x86_64.so.1`; the plan does not generalise it,
   it isolates it so that a real filesystem layout can delete it.

## 4. Non-goals

- A real filesystem layout. The interpreter path substitution stays until there
  is one.
- Shared libraries beyond the single interpreter this kernel already loads.
- `MAP_SHARED` file mappings, which belong to the memory plan.
- Changing the two user windows, or where Linux binaries link.

## 5. Tracking

As the project rule requires: a row in `docs/implementation_progress.md`, a
detail file, and the book summary regenerated.
