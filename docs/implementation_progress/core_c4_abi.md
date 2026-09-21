# C4 - one operation vocabulary, and the Linux ABI leaves `arch_hw.c`

Status, 2026-09-21: **stages 1, 3, 2a and 2b done.** The plan
(`docs/core/phases.md`) is the authority on intent; this file records what was
built, what it measured, and what it found on the way.

## Stage 1: the vocabulary, its declarations, and their enforcement

- `include/vibeos/abi.h` declares every kernel *operation* once, with the checks
  that apply to it, in an X-macro list. The enum, the name table and the check
  table cannot drift, and a line without its checks does not compile. It is named
  `op`, not `syscall`: the dispatcher C3 deleted had left its own
  vocabulary behind (`syscall.h`, `syscall_abi.h`, `syscall_policy.h`, 1,023 lines
  nothing used), which was removed on 2026-09-20 - one vocabulary now.
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
**Sixteen were stale before C4 began - and are repaired**: `mm-vmspace` (seven), `threads` (two),
`tlb-shootdown`, `stress`, `journal`, `ahci`, `ring3-fault`, `services`,
`storage`. Each was re-run, which found three that mattered more than their anchors (a shootdown counter that stopped isolating fork, an untested stale-TLB guard, an AHCI sabotage OVMF had already defeated); see check-sabotage-anchors.py. Unresolved is now ratcheted at zero.

## Not done


- The linkage from `linux_internal.h` to `arch_hw_internal.h` is a relative
  include. The Linux layer still reaches x86-64 objects (a task, its address
  space, its descriptor table) directly; abstracting that is the day a second
  architecture exists, not before.

## Stage 2b: the pointer arguments are declared in the row (2026-09-21)

- A row's fourth column is `NOPTR` or `PTRS(...)`: which arguments are user pointers,
  how long, read or write, optional, and - for a request code that decides whether
  the argument is a pointer at all (ioctl, prctl, arch_prctl, netctl) - the value of
  the argument it depends on. `vibeos_ptr_t` (abi.h) holds it; the constructors
  (`OUT`, `IN`, `OUT_BUF`, `IN_VEC`, `*_OPT`, `*_IF`) are in `linux_internal.h`.
- The dispatcher's `check_pointers` validates them **before the handler runs** and
  answers -EFAULT. 22 rows carry descriptors and about 30 handler checks are gone.
  The registry refuses a descriptor that names an argument the call does not have.
- `hw_user_range_ok` has **one call site** (`linux_user_ok`, dispatch.c); it was 46.
  `check-chokepoints.py` reports 4 for it (definition, two declarations, that call)
  and watches `linux_user_ok` at 16, where the old "a syscall stopped checking"
  alarm lives now.
- **What stays in the handlers (13 sites), and why it cannot be a descriptor:**
  an iovec element's own base (read out of user memory a moment earlier), readlink's
  length (the link target's), clone's stack and the two tid words and wait's status
  word (stores that are skipped silently, not refused), futex (answers EINVAL, not
  EFAULT), the signal frame and sigreturn (a rsp, not an argument), and the kernel's
  own reads of user memory (a string, the crash dump).
- **Error precedence changed, deliberately, and this is the price.** A call that has a
  bad pointer *and* another error now reports EFAULT where the handler used to report
  the other one first: read on a bad fd with a bad buffer (was EBADF), getdents on a
  non-directory (was ENOTDIR), getcwd with size 1 (was ERANGE), pipe2 with a bad
  pointer and bad flags, sigaction with a bad signal number and a bad pointer, the
  ioctl cases for descriptors above 2. Linux itself mostly reports EBADF first; no
  program here depends on the order and the boot's ABI self-test passes, but it is a
  behaviour change and is recorded as one.
- **Decision (2026-09-21): EFAULT-first stays; Linux's EBADF-first is not reproduced.**
  Doing it would make the engine look up file descriptors - a filesystem dependency
  (the console's fd < 3, sockets) inside the one generic place - and would make every
  future row declare which check precedes its pointer. That is permanent surface for
  an order nothing here depends on. It is reversible: the choice lives only in
  `check_pointers` and in this note. Revisit if a real program relies on the order.
- **The engine is gated for the first time.** The ring-3 program writes from a kernel
  address and prints a line only if it was refused; nothing asserted that line until
  now, so an engine that validated nothing would have booted green. The gate has
  `kernel_pointer_not_rejected`, and `cases/core-ptr-engine.txt` shows both engine
  sabotages turn the boot red with that reason.
- Sabotage: `core-ptr-engine.txt` (2, boot), `core-ptr-descriptors.txt` (4),
  `core-syscall-hoist.txt` (3), `core-user-check.txt` (2, chokepoints).

## Stage 2a: rows registered by the file that holds the handler (2026-09-21)

- Each of fs/proc/mm/sig/misc/net.c ends with an X-macro list of rows (number, name,
  operation, call) and `LINUX_DEFINE_SYSCALLS`, which generates the adapters and the
  table. `dispatch.c` looks the number up in the registry; the central `switch` is gone.
- The registry (`abi_linux.c`) validates a whole table before taking it and refuses a
  duplicate number, a NULL handler and an operation NONE or out of range.
- **"Add a syscall" is 2 files** (abi.h and the file that holds the handler), from 6.
  The number is stated a second time in `scripts/dev/linux-syscall-numbers.txt` on
  purpose, and `check-syscall-checks.py` holds every row against it.
- Sabotage: `cases/core-syscall-rows.txt` (wrong number, duplicate number, deleted row,
  row that no longer validates memory) and four registry cases in `abi-abi_linux.txt`;
  all red. Full check green, 3/3 boots, clang and gcc warnings 0.
