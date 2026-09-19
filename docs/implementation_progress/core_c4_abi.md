# C4 - one operation vocabulary, and the Linux ABI leaves `arch_hw.c`

Status, 2026-09-19: **stages 1 and 3 done, stage 2 next.** The plan
(`docs/core/phases.md`) is the authority on intent; this file records what was
built, what it measured, and what it found on the way.

## Stage 1: the vocabulary, its declarations, and their enforcement

- `include/vibeos/abi.h` declares every kernel *operation* once, with the checks
  that apply to it, in an X-macro list. The enum, the name table and the check
  table cannot drift, and a line without its checks does not compile. It is named
  `op`, not `syscall`: `include/vibeos/syscall.h` still holds the
  `vibeos_syscall_id` of the dispatcher C3 deleted, and reusing that word would
  have made two vocabularies with one name.
- `kernel/abi/abi_linux.c` is the Linux ABI as a translator: a constant
  `number -> operation` table plus `clone()`'s decision by flags (which is why
  `classify` takes the first argument). The ABI is bound to a task in
  `hw_task_alloc`, the one place a slot is claimed, and read from the task.
- `scripts/dev/check-syscall-checks.py` holds each declaration against the
  handlers' call graph, in both directions. It is **reachability, not proof**: it
  catches a check that disappears, not a check applied to the wrong pointer. Its
  first run corrected three of my own declarations (`exit`, `exit_group`, `prctl`
  touch user memory).
- Found and removed on the way: `vibeos_linux_translate_syscall` was called on
  every syscall and its result stored in a variable nothing read, and a
  `[COMPAT] translated=` counter printed a number that meant nothing.

## Stage 3: 4,000 lines leave `arch_hw.c`

`arch_hw.c` went from 12,174 to about 7,800 lines. Its Linux-facing handlers and
the helpers only they use now live in `kernel/abi/linux/`:

| file | what |
|---|---|
| `dispatch.c` | classify, then switch on the operation |
| `fs.c` | open, read, write, pipes, stat, getdents, ioctl, the console |
| `proc.c` | fork, clone, execve, wait, exit_group, prctl, futex, prlimit |
| `mm.c` | brk, mmap, mprotect, munmap, pageinfo |
| `sig.c`, `signal.c` | kill/tkill/tgkill/sigaction/sigprocmask; frame building and sigreturn |
| `net.c` | the socket calls (was `arch/x86_64/linux_socket.c`) |
| `misc.c` | uname, clock_gettime, time |
| `common.c`, `linux_internal.h` | what several of the above share |

**How it was cut.** Not by line range: the stretch was interleaved with
architecture that has nothing to do with Linux - the copy-on-write fault handler,
the TLB shootdown, the crash dumper, task lookup. A throwaway tool parsed the file
into top-level items, built the reference graph, and moved a handler together with
every helper *only handlers reference* (an item moves iff everything that
references it moves). Everything else stayed and became a named dependency in
`arch_hw_internal.h` - 67 declarations and 29 macros and types, which is the honest
size of the seam. Being driven by the compiler, the first pass turned up four
mistakes in the tool itself (types named by shared declarations, `static inline`
helpers, prototypes of moved symbols, and section banners that travelled with the
first item after them); all four are fixed and none is in the product.

**Two metrics moved, both honestly.**

- *input device* went 4 to 5 the moment the handlers moved, because `fs.c` named the
  keyboard and framebuffer directly - a dependency that had been invisible inside
  one file. It is 4 again: the console is now two functions the architecture
  supplies (`hw_console_getc`, `hw_console_echo`).
- *syscall* is **6** (1 before C4, 4 after stage 1). The case and the handler are
  now two files and the handler needs a declaration between them. That is the
  price of the cut and it is meant to be temporary; stage 2 exists to remove it.

## The check this phase added, and what it found

`scripts/dev/check-sabotage-anchors.py` verifies that every sabotage case still has
an anchor to break. Moving 4,000 lines makes stale anchors certain, and
`sabotage.py` only reports one when somebody runs it. First run: **20 cases with
nothing left to break.** Four were this phase's (two anchors that moved, two written
against names renamed in the same change and never run - now run and red).
**Sixteen were stale before C4 began**: `mm-vmspace` (seven), `threads` (two),
`tlb-shootdown`, `stress`, `journal`, `ahci`, `ring3-fault`, `services`,
`storage`. They are recorded as a ratcheted baseline and handed to a follow-up
task; each needs its sabotage re-run to confirm it still goes red.

## Not done

- **Stage 2:** the dispatcher does not yet call the checks - `hw_user_range_ok` is
  still called from 46 sites inside handlers - and the handlers are still reached
  through a central `switch`, not registered by the file that holds them. Both go
  together: a row per syscall in its own file, carrying number, operation, handler
  and its pointer-argument descriptors.
- The linkage from `linux_internal.h` to `arch_hw_internal.h` is a relative
  include. The Linux layer still reaches x86-64 objects (a task, its address
  space, its descriptor table) directly; abstracting that is the day a second
  architecture exists, not before.
