# Program Loading Progress

Status: **Rewrite underway** ([docs/exec/](../exec/README.md)). X-P0 and X-P1 done.
Last review: 2026-09-01

## Why it is being rewritten

`execve` is the longest function in `arch_hw.c`, and everything it does lives
there: reading the file, parsing the headers, choosing an address window,
mapping segments, placing an interpreter, building the startup block. It holds
two static staging buffers totalling six megabytes, reads whole programs into
them with interrupts masked, and shares them between every core.

The plan and its phases are in [docs/exec/phases.md](../exec/phases.md). This
file records what has actually landed.

---

## X-P0 — Say what happens (done, 2026-09-01)

**The defect this closes is not a crash. It is that a failed exec could not be
diagnosed.** The loader had fourteen `return -1` sites and one message between
them, so every way of failing to start a program produced the same sentence.
"Cannot load" is not a diagnosis: a file that is absent, a file that is present
and short, and a file that is perfect with no memory left are three unrelated
investigations, and nothing in the log distinguished them.

That mattered more than it sounds. A failed FAT lookup once looked like the end
of a file, so a flaky sector produced a short image that claimed to be complete
and the loader parsed whatever the previous program had left in the shared
staging buffer. That defect is fixed - but nothing in the exec path would have
*said* so, because all of those outcomes printed the same line.

**What landed**

- `include/vibeos/exec_stats.h` and `kernel/exec/stats.c`: eleven refusal
  reasons, one counter each, portable so a host test can assert them as well as
  the boot gate. The names are stable text, because the gate matches on them -
  they are interface, not prose.
- `hw_exec_refuse()` in the architecture layer: counts, prints one line naming
  the reason, the path and the step, and returns -1. One `serial_lock` call for
  the whole line, because a message assembled from six `serial_puts` is six
  critical sections with three other cores writing into the gaps.
- Every refusal in `hw_proc_create` and the `not-found` refusal above it now
  goes through it. The two that already had ad-hoc messages of their own are
  included, since a sentence in the log is not something a gate can assert on -
  which is exactly how they had stayed outside the tally.
- `exec` on the kernel console prints the counters; `kernel/exec/view.c` holds
  the formatting, so the architecture layer knows nothing about it.

**The path is a parameter, not `argv[0]`.** `hw_proc_create` takes the path it
was given. Re-using `argv[0]` would have been free and would have been a lie:
BusyBox becomes twenty commands by looking up the name it was invoked under. The
embedded-image path has no filename at all and passes null, which prints as `-`.

**Gated.** The boot gate drives `exec` and asserts on *which reasons appear*,
not on a total:

    [EXEC] loaded=0x1c not-found=0x6

`not-found` is expected and frequent - a shell resolving a bare command name
tries it as a path first, and a C runtime asks for `/proc/self/exe`, which this
filesystem does not have. Every other reason is a failure. That is a much
stronger assertion than a count: one `no-memory` or one `short-read` hidden
among forty expected `not-found`s is exactly what this watches for, and a total
of forty-one would not blink.

The expected set was **measured from a green boot**, not reasoned about from the
loader - the whole reason the phase exists is that nobody could tell which
refusals were happening. Emptying the set turns the boot red with
`exec_unexpected_refusal=not-found`, which is how the assertion was confirmed.

---

## X-P1 — The parse comes out (done, 2026-09-01)

**A correction to the plan, made in the open.** X-P1 said to create
`kernel/exec/image.c` holding a pure segment-layout function. It already exists:
`kernel/core/elf.c` is portable, takes headers and a bias, and produces exactly
the list of (vaddr, file offset, filesz, memsz, flags) a loader must satisfy,
plus `vibeos_elf_page_flags` and `vibeos_elf_fill_page`. Writing a second one
would have produced two parsers that must agree about a security boundary, with
no build step checking that they do - worse than the long file the phase was
meant to shorten.

What was genuinely missing is that **none of it was reachable except through a
boot**. `tests/kernel/loader_tests.c` fixes that: twelve cases against a
hand-built ELF64, so each one names the field it exercises instead of hiding it
in an opaque blob.

The layout cases are the ones that have bitten a loader here or are one edit
from doing so: two segments with a hole between them (a loader that fills from
`min_vaddr` to `end_vaddr` hands the program writable pages it never asked
for); a `.bss` tail where `memsz` exceeds `filesz`; a page filled with a pattern
first, so a filler that copies and forgets to zero cannot pass against a buffer
that was already zero; two segments sharing one page, which must carry the
permissions of both; an image asking to be placed on top of the kernel; a
truncated file, refused rather than parsed as far as it goes; and an ET_DYN
image biased exactly once, with `image_span` proven not to move with the bias.

The startup block is **decoded back and compared**, not spot-checked. Every
field is one a C runtime reads before the program's first line, so a builder
that is plausible but wrong kills the program inside the runtime with no
reference to the loader that mis-built its stack. Two rules that were comments
are now assertions: `AT_ENTRY` stays the *program's* entry when an interpreter
starts the process - otherwise the interpreter re-enters itself and it looks
like a hang - and `AT_BASE` is *omitted* for a static program rather than
written as zero, because zero is a legal load address and a runtime believes it.

Confirmed by breaking it: changing one expected value turns `test_loader` red.

### One thing this phase cost, worth recording

The first version of the gate change had a syntax error - a new block inserted
between an `if` and its `else`. `check.sh` runs the gate as
`python3 ... > /dev/null 2>&1` and then reads `qemu-cli-summary.txt`, so the
crashed gate left the *previous* run's summary in place and `check.sh` reported
`reason=cli_and_network_verified`. Three runs passed having executed nothing.

It is the same trap as "a green build is not a build", wearing different
clothes, and the tell was the same: a file's timestamp that had not moved.
