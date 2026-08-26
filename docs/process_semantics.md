# Process semantics

How a process is created, replaced, signalled and reaped here, and the places
where matching Linux turned out to mean something other than what the code
first assumed.

## Two user windows

VibeOS's own programs link at `0x8000000000`. Linux programs link at
`0x400000`, which is inside the kernel's identity map - so the physical range
behind the low window is reserved out of the allocator, and nothing of the
kernel's can live where a process would shadow it. Which addresses a process
may use is decided in one place, `hw_user_addr_ok`.

This is not a stylistic split. An unmodified static Linux binary has its load
address baked in, and refusing it would mean refusing the whole point.

## Starting a program

`execve` builds a System V startup stack: `argc`, the `argv` pointers, the
`envp` pointers, then the auxiliary vector. A C runtime reads more of that
vector than its name suggests it must:

- `AT_PHDR`, `AT_PHNUM`, `AT_PHENT` - glibc walks its own program headers.
- `AT_ENTRY`, `AT_BASE`.
- `AT_RANDOM` - sixteen bytes used to seed the stack canary. When it was
  absent, the pointer was null and the process dereferenced it during startup,
  before `main`. `getrandom` is deliberately still `ENOSYS`; that is a separate
  promise and this one had to be kept first.

The entry `rsp` must be 16-byte aligned, which is why `_start` cannot be C.

`argv[0]` is not the path. BusyBox is twenty commands in one binary and decides
which it is by the name it was invoked under; FAT stores names upper case with
an extension. The shell supplies the name, the path only opens the file.

`/proc/self/exe` is stored absolute even when the caller used a relative path.
glibc does not merely prefer that - it asserts on it during startup and aborts.

## fork

Copy-on-write. The page tables are shared with the writable bit cleared and
`PTE_COW` (bit 9) set, and a frame refcount table says how many address spaces
hold each frame.

Three places have to agree, and they do not look related:

- The fault handler must accept **kernel-mode** writes. The kernel writes user
  buffers with `CR0.WP` set, so a `read()` into a COW page faults from ring 0.
- `hw_user_range_ok` must treat a COW page as writable. It is writable; it just
  needs a copy first. Rejecting it makes a valid buffer look invalid.
- A **second** fork must preserve the COW bit rather than treating the page as
  plain read-only, or the third process shares a page nobody will copy.

The refcount table is indexed from the region base. Indexing it from the end of
the table instead freed pages that were still in use - a bug that presents as
unrelated memory corruption much later.

## Signals

Delivered on the return path to user space, never mid-syscall. The frame is
built below the red zone, and return goes through the user library's own
`SA_RESTORER` trampoline rather than anything the kernel supplies.

**Linux `sigset_t` bits are numbered from zero: bit 0 is signal 1.** This
kernel numbers them by signal number. The conversion happens only at the
boundary, in `hw_sigset_from_user` and `hw_sigset_to_user`. Doing it anywhere
else means two conventions in the same code path and an off-by-one that only
shows up for whichever signal happens to be tested last.

## Exit and wait

**A wait status is not an exit code.** The exit code lives in the high byte and
the signal number in the low seven bits. `128 + sig` is what a shell prints,
not what the kernel stores - copying the shell's arithmetic into the kernel
produces a status that looks right in one test and is wrong everywhere else.

Descriptors are inherited across `fork` and released on exit, pipe ends
included. Exit that did not release pipe ends left readers waiting on a writer
that no longer existed, which is a hang rather than an error.

## Pipes

`pipe2`, `dup`, `dup2`, inheritance across `fork`, and `SIGPIPE` for a write
with no reader. The boot self-test runs `ls /EFI/BOOT | wc -l` in BusyBox's
shell and the gate fails if the pipeline does not complete - a pipeline that
deadlocks and a pipeline that returns the wrong number are both caught, because
the count is asserted and not merely printed.

## Scheduling and locks, where processes meet them

Spinlocks mask interrupts, so anything slow under one is slow with the timer
off - and under emulation that is indistinguishable from a hang. `execve` is
the long one: two megabytes loaded, hundreds of pages mapped, an address space
torn down. It therefore takes `g_exec_lock` *preemptibly*, with interrupts
left on, which is safe only because no interrupt handler ever takes that lock.

That choice has a consequence recorded in `CLAUDE.md` and not yet resolved: a
lock whose holder can be preempted must not be waited on by a core that has
interrupts masked, because such a core never reaches the scheduler and can
never hand the holder a CPU to finish on.

## What is verified

The boot gate asserts behaviour rather than log lines: an unmodified static
Linux binary ran, BusyBox dispatched an applet and did file work, the
interactive shell ran, signals were delivered, `fork` produced a child that was
reaped with the status the parent expected.

`Ctrl-C` becoming a real `SIGINT` is implemented but not gated - it is checked
by hand.
