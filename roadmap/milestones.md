# Milestones

## Current Snapshot (2026-08-03)

| Milestone | Status | Evidence |
| --- | --- | --- |
| M0 project charter | Completed | Phase 1 documents |
| M1 toolchain and build | Completed | gcc/clang x Debug/Release matrix, green on every push |
| M2 boot to kernel banner | Completed | UEFI boot gate under OVMF, required in CI |
| M3 memory and interrupts | Completed | paging, traps, PIT and APIC timer, all boot-verified |
| M4 scheduler and multitasking | Completed | preemptive SMP across all cores, per-process address spaces |
| M5 first user-space service | Completed | ring-3 programs with fork/exec/wait |
| M6 storage and shell | Completed | virtio-blk, FAT read and write, serial shell |
| M7 networked native system | Completed | TCP/IP over virtio-net, TCP round trip to a host server in CI |
| M8 Linux CLI compatibility | **Completed** | unmodified static BusyBox runs echo, cat and ls; its shell parses scripts, searches PATH, forks and execs. All four gated in CI |
| M9 process semantics | **Completed** | signals delivered and verified by a program built against glibc; copy-on-write fork measured at 1221 pages shared to 24 copied |
| M10 a machine with a screen | **In progress** | PS/2 mouse on IRQ12, framebuffer desktop, on-screen terminal mirroring the console. Gated on state; the pixels are still checked by hand |
| M11 graphical user space | Not started | - |
| M12 usable without a serial cable | Not started | - |
| M13 dynamic executables | Done | ET_DYN loaded at a bias, PT_INTERP mapped alongside, and a dynamically linked musl binary runs in the boot gate |
| M14 threads | Partly done | clone(CLONE_THREAD), futex wait/wake, tgid/tid and thread exit are in; one thread can be created and joined, several at once still hangs |
| M15 Windows console compatibility | Not started | - |
| M16 durable storage | Partly done | journal, block cache and device barrier built and swept against power loss; no filesystem routes its metadata through it yet |
| M17 architecture review for expansion | Ongoing | design documents updated alongside the code |

Status here reflects what boots and passes a gate, not what has been designed.
A milestone moves to Completed when something fails if it regresses.

Two notes on how this list changed. The original M9 and M10 were Windows
compatibility and an architecture review; the work that actually happened -
process semantics, then drivers and a screen - had no milestone at all, which
made the roadmap describe a different project from the one in the repository.
Windows compatibility has moved down rather than away: it was never started,
and putting it after the things this system needs to be usable is a statement
about order, not about scope.

The April assessment (`docs/project_status_assessment_2026-04-03.md`) predates
most of this work and is kept as a historical record rather than a current
view.

## M0: project charter complete

- all Phase 1 documents created
- repository structure agreed
- architectural choice documented

## M1: toolchain and build skeleton

- cross compiler selection
- build scripts
- image packaging
- emulator launch flow
- first automated boot smoke test

## M2: boot to kernel banner

- bootloader loads kernel
- serial output works
- early memory map parsed

## M3: memory and interrupts online

- IDT or equivalent configured
- paging stable
- physical allocator functional

## M4: scheduler and multitasking

- threads run and preempt
- timer tick or event scheduling works
- SMP bootstrap on x86_64

## M5: first user-space service

- init process starts
- IPC path usable
- service restart path proven
- service manager and device manager boundaries validated

## M6: storage and shell

- filesystem mounted
- shell executes native commands
- file I/O stable

## M7: networked native system

- TCP/IP available
- remote diagnostics or package ingress possible

## M8: Linux CLI compatibility (completed)

Exit criterion was "shell utilities or a small Linux user program run under
compatibility mode". Delivered, and gated:

- a static musl binary and a static glibc BusyBox, neither built by this
  project, load at the address they were linked for and reach `main`
- BusyBox dispatches on its own name and runs `echo`, `cat` and `ls`, reaching
  the filesystem through `openat`, `fstat`, `read` and `getdents64`
- its shell parses a script, runs builtins, searches `PATH`, forks and execs
- the boot fails on `busybox_did_not_run`, `busybox_applet_dispatch_failed`,
  `busybox_file_operations_failed` and `interactive_shell_did_not_run`

## M9: process semantics (completed)

Not in the original plan. It became a milestone because two things a real
program assumes were missing, and both are invisible until something depends
on them:

- signals are raised, masked, queued and delivered on the way back to user
  space, with the frame built on the process's own stack below the red zone.
  Verified by a program written against Linux, so the return trampoline comes
  from a real C library rather than from a test that agrees with us
- `fork` shares pages copy-on-write instead of copying them. Measured: 1221
  pages shared, 24 later duplicated. A shell forks for every command and the
  `exec` after it discards the copy, so eager copying was work guaranteed to
  be wasted

## M10: a machine with a screen (in progress)

- **done**: PS/2 mouse driver on IRQ12 with packet resynchronisation, a
  framebuffer desktop with a pointer, and a window that mirrors the console so
  the machine shows what it has been saying on the serial line
- **done**: the boot gate fails if the on-screen terminal stayed empty
- **remaining**: typed characters still reach only the shell; `Ctrl-C` becomes
  `SIGINT` but that path is not yet exercised by a test
- **remaining**: the pixels are checked by `scripts/dev/screenshot.py`, run by
  hand. A gate that a human has to remember to run is not a gate

## M11: graphical user space

The GUI is currently kernel code, which makes it a drawing rather than a window
system. Exit criterion: a ring-3 program draws its own window and receives its
own input.

- syscalls for querying the screen, presenting a buffer, and reading input
- input routed to a focused window rather than to whatever reads the console
- the desktop itself becomes one of those programs

## M12: usable without a serial cable

The point at which someone can sit in front of the machine and use it. Exit
criterion: boot to a shell in a window, type, and get output, with no serial
console attached.

- keyboard into the terminal window, including editing
- `dup`, `dup2`, `pipe2` and `poll`, without which a shell cannot do `ls | wc`
- the boot self-test driven through that path, so it is gated rather than
  demonstrated

## M13: dynamic executables

Today `ET_DYN` and `PT_INTERP` are refused, so "runs Linux programs" means
static ones. Exit criterion: a dynamically linked binary runs.

- load the interpreter, hand it the auxiliary vector, let it relocate
- `mmap` with file backing, which needs a page cache
- shared library search and `openat` on a real directory tree

## M14: threads

`clone` with `CLONE_VM` returns `ENOSYS` and futexes are answered honestly
rather than implemented. Exit criterion: a threaded program runs correctly
under contention.

- threads sharing an address space, with per-thread TLS and kernel stacks
- real futex wait and wake
- `set_robust_list` starts meaning something

## M15: Windows console compatibility

Unchanged in scope, moved in order. A PE loader and enough of the NT
user-space surface for a console executable, in a subsystem outside the kernel.

## M16: durable storage

FAT has no journal, so a write interrupted by a reset can leave the volume
inconsistent. Exit criterion: power loss during a write does not corrupt the
filesystem.

- a block cache with write-back and barriers - **done**, and the barrier now
  reaches the device, because a drive acknowledges a write when it reaches its
  own volatile cache and not before
- a test that cuts power mid-write and checks the result - **done**, as a sweep
  rather than a single case: a fake drive stops accepting writes after exactly
  N of them, for every N a transaction performs and under several flush
  orderings, and the volume must come back all-old or all-new
- a filesystem with ordered metadata updates - **not done**. The journal
  exists, is tested and recovers at mount, but no driver stages its metadata
  through it yet. Until that connection is made, this milestone describes a
  mechanism and not a volume, and FAT writes remain as crash-unsafe as before.

## M17: architecture review for expansion

- reassess kernel and service boundaries now that there is a user space
- validate the security posture: there is one identity, and it is root
- review the risk register and compatibility scope
- decide ARM64
