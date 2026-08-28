# Linux Compatibility Progress

Status: In Progress (static, position-independent and dynamically linked Linux binaries all run end to end)
Last review: 2026-08-27

## Read this first: there are two things called "Linux compatibility"

They are unrelated, and confusing them is how this area went untracked.

1. **The kernel's Linux syscall layer** (`kernel/arch/x86_64/arch_hw.c`, the
   `LSYS_*` dispatch). This is the real one. It is what runs a static musl
   program and a static BusyBox that this project did not build, and it is
   gated on every boot.
2. **The portable compatibility scaffold** (`user/compat/`). A runtime object
   with per-target enable flags, translated/denied counters, and one
   translation function per foreign OS. The Linux translator maps two syscall
   numbers. It is a shape, not a subsystem.

Everything that works today is (1). The kernel does not call (2) to run a Linux
binary; it enables the Linux target in the scaffold at boot and reports its
counters, which is why the serial log shows small numbers next to a working
BusyBox.

## Implemented

- 67 Linux syscalls in the `LSYS_*` dispatch, covering process lifecycle
  (`fork`, `vfork`, `execve`, `wait4`, `exit_group`), files and descriptors,
  pipes, signals (`rt_sigaction`, `rt_sigprocmask`, `rt_sigreturn`, `kill`,
  `tgkill`, `tkill`), memory (`mmap`, `brk`), sockets, and the startup
  interrogation a C runtime performs before `main`.
- A System V startup stack with a populated auxiliary vector, including
  `AT_RANDOM` - absent, it is a null pointer dereferenced while seeding the
  stack canary, before `main` runs.
- Copy-on-write `fork`, signal delivery through the libc's own `SA_RESTORER`
  trampoline, and pipelines in BusyBox's shell.
- An unimplemented syscall is reported by number on the serial line rather than
  silently returning zero, so a program failing for that reason says so.
- **Dynamically linked programs run.** A position-independent executable is
  placed at a bias chosen by the loader, and one naming a `PT_INTERP` has that
  interpreter mapped into the same address space and entered instead of the
  program. The interpreter relocates itself from `AT_BASE` and jumps to
  `AT_ENTRY`, which stays the program's own entry. Gated: `DYN_OK` and
  `PIE_OK` both fail the boot if they stop appearing.
- The interpreter path is translated. A musl binary asks for
  `/lib/ld-musl-x86_64.so.1`, and the boot volume is FAT, which has neither
  that directory nor a name that long; the loader sits beside the other
  programs and the one path is substituted in the kernel. That is a stand-in
  for a filesystem layout, not a design.

## Known sharp edges

Recorded because each cost hours and none is visible from the code:

- Arguments typed `int` arrive **zero-extended**: `mov $-100, %edi` delivers
  `0x00000000ffffff9c`, so comparing all 64 bits against `AT_FDCWD` never
  matches. Read them through `VIBEOS_ARG_INT()`.
- Linux `sigset_t` bits are numbered **from zero** - bit 0 is signal 1. This
  kernel numbers them by signal, and converts only at the boundary.
- A wait status is not an exit code: exit code in the high byte, signal in the
  low seven bits.
- `/proc/self/exe` must be absolute. glibc does not merely prefer it; it
  asserts and aborts during startup.

## Pending

- Dynamic executables were the largest gap here and are now closed; what is
  written under Implemented above covers them. What remains unproven is
  breadth: one program and one interpreter have been run, not a library with
  dependencies of its own.
- **Threads work, and are gated.** `clone(CLONE_VM|CLONE_THREAD)` creates a
  task sharing the caller's address space with its own kernel stack and TLS
  base; `futex` waits and wakes for real; exit clears the word a joiner sleeps
  on - before any context switch, because the word lives in the dying task's
  address space - and only tears that space down when the last thread of the
  group leaves. Four threads share a counter under a contended mutex, land on
  exactly 8000, and each sees its own thread-local. Sixteen boots out of
  sixteen with that running.

- `getrandom` is deliberately `ENOSYS`. `AT_RANDOM` is supplied; a real entropy
  source is a separate promise and has not been made.
- Process groups and a controlling terminal, so `ioctl(TCGETS)` can be
  something other than `ENOTTY` and `Ctrl-C` can target a job.
- The scaffold in `user/compat/linux/` translates two syscall numbers and is
  not on the path any real binary takes. Either it grows into the portable
  front end for the kernel layer, or it should be deleted; leaving it looking
  like the implementation is the current cost.

## Next checkpoint

- Decide the scaffold's fate, then start M13: `PT_INTERP` and a loader, which
  is what turns "static binaries" into "binaries".
