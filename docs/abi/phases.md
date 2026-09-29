# Syscalls and the Linux ABI: Phases

Two parts. **A** rebuilds the layer the current 73 syscalls stand on, so that
the next three hundred can be added and tested one at a time. **L** adds them,
by capability, each phase closed by programs this project did not write.
Numbers per phase are from [syscalls.md](syscalls.md).

Every step, in both parts, is done the way this project does things: a gate or
host test that fails if the step regresses, a sabotage case that proves the test
can fail, a row in the tracker for what it found, and the docs updated in the
same change.

---

## Part A - the refactor of what exists

### A0. The corpus and the measurement

**Objective.** Know, per program, which syscalls it needs and which of them are
missing - before writing any.

- Build the corpus as static musl binaries on the host: BusyBox (with its test
  suite), `sqlite3`, `lua`, and LTP's `testcases/kernel/syscalls`. Staged on the
  boot image like the existing test programs.
- `trace-linux-binary.sh` over each, into a checked-in needs list per program.
- A report: for each program, needed / done / partial / missing.

**Done when** the report exists for every corpus program and runs in the nightly.

### A1. The registry

**Objective.** One source of truth for all 373 numbers (invariant 6), and two
counters where there is one (invariant 7).

- A registry file (number, name, state, phase, reason) replaces
  `scripts/dev/linux-syscall-numbers.txt`, which today lists only the
  implemented ones. It is generated once from the kernel's
  `arch/x86/entry/syscalls/syscall_64.tbl` and edited by hand after that.
- The dispatcher answers a refused number with its registry errno and counts
  `abi_refused`; a missing number counts `abi_missing`. The gate fails on
  `abi_missing` from a corpus program, as it already does for an unimplemented
  call today.
- [syscalls.md](syscalls.md) is generated from the registry by a script in the
  book build, instead of by hand.
- `check-syscall-checks.py` also checks each row against the registry.

**Done when** the three statements of a number (row, registry, table) cannot
disagree without `check.sh` failing, and a sabotage that marks a refused number
missing turns the gate red.

### A2. Handlers leave the architecture

**Objective.** Invariant 9: `kernel/abi/linux/` stops including
`arch_hw_internal.h`.

- A declared interface for what a handler needs: the current task and its
  identity, its descriptor table, its address space (map, protect, unmap, fault
  a user range in), the filesystem, the network stack, and wait/wake. Implemented
  by the architecture; a fake implementation for host tests.
- `hw_sys_*` renamed `linux_sys_*`; the helpers that only handlers use move with
  them, as in C4.
- Host tests per handler, beginning with the 16 partial rows, whose gaps are
  written as failing expectations that the owning L phase turns green.

**Measure.** The ~700 references to `hw_*`/`g_tasks` in `kernel/abi/linux/`
go to zero; blast radius for "add a syscall" goes to one file plus its row.

**Done when** the Linux ABI builds into the host test binary and the existing
boot gate is unchanged.

### A3. Descriptors as Linux has them

**Objective.** Fix the two descriptor defects under every I/O syscall.

- An **open file description**: a refcounted object holding the offset, the
  flags and the type-specific state, shared by `dup`, `dup2`, `dup3`, `fork` and
  `SCM_RIGHTS` later. The descriptor table holds references to it, not copies.
- A table that grows to `RLIMIT_NOFILE` (default 1024), with `FD_CLOEXEC`
  honoured at exec and `close_range`.
- One operations table per file type - regular file, directory, pipe, socket,
  terminal, and later eventfd/timerfd/signalfd/epoll - so `read`, `write`,
  `lseek`, `fstat` and `ioctl` dispatch by type instead of by the `if` chains they
  use today.

**Done when** a host test shows two `dup`ed descriptors sharing an offset, a
program opens 200 files, and descriptor sharing between threads (M-072) still
passes.

### A4. Paths

**Objective.** Relative paths, a working directory, and the `*at` family.

- A per-process working directory and root, inherited by fork and threads.
- One path walk through the mount table: `.`, `..`, mount crossings, symbolic
  links where the filesystem has them, `ENAMETOOLONG`/`ELOOP`/`ENOTDIR` as Linux
  reports them.
- `dirfd` honoured everywhere `AT_FDCWD` is today.
- The `/lib/ld-musl-x86_64.so.1` substitution in the loader is deleted, as
  CLAUDE.md says it should be the day the layout becomes real.

**Done when** `cd` in a shell works, relative `open` works, and the loader finds
its interpreter by path.

### A5. Layouts and errno from Linux's headers

**Objective.** Invariant 8 for structures: `stat`, `statx`, `dirent64`,
`timespec`, `sigaction`, `siginfo_t`, `sockaddr_*`, `rlimit`, `utsname`,
`termios`.

- A host test compiled against the host's `linux/*.h` uapi headers asserts every
  offset and size the kernel writes, and every errno value it returns.

**Done when** the test exists and a sabotage that moves one field turns it red.

---

## Part L - the syscalls, by capability

Each phase names its target programs. It is done when they run under the gate
and their LTP cases pass, not when its rows exist.

### L1. Files and paths (72 missing, plus 6 partial finished)

`stat`, `lstat`, `access`, `pread64`/`pwrite64`, `fcntl`, `flock`, `fsync`,
`truncate`, `rename`, `rmdir`, `link`/`symlink`/`readlink`, `chmod`/`chown`,
`umask`, `utimensat`, `statfs`, the whole `*at` family, `dup3`, `preadv`/`pwritev`,
`statx`, `copy_file_range`, `sendfile`, `close_range`, `openat2`, `sync`.
Extended attributes answer ENOTSUP from the filesystem.

A minimal terminal: the console becomes a TTY file type answering `TCGETS`,
`TCSETS`, `TIOCGWINSZ` and `TIOCGPGRP`/`TIOCSPGRP`, which every interactive shell
asks for first.

**Programs:** BusyBox's file applets and its test suite (`ls -l`, `cp -a`, `mv`,
`find`, `tar`, `sort`, `grep -r`, `sed -i`), `sqlite3` on a file.

### L2. Processes, credentials and time (47)

Sleeping and timers (`nanosleep`, `clock_nanosleep`, `alarm`, `setitimer`,
POSIX timers), resource limits and usage, `waitid`, process groups and
sessions completed, the rest of signals (`sigaltstack`, `rt_sigsuspend`,
`rt_sigtimedwait`, `rt_sigqueueinfo`, `SA_SIGINFO` with a real `siginfo_t`),
`execveat`, `clone3`, pidfds.

The credential model arrives here: real, effective, saved and filesystem ids,
supplementary groups, and permission checks in L1's file operations. Setting the
clock stays EPERM until there is a privilege to allow it.

A minimal `/proc` (`/proc/self/exe`, `/proc/self/fd`, `/proc/self/maps`,
`/proc/meminfo`, `/proc/cpuinfo`) belongs here as well: it is a filesystem, not a
syscall, but `ps`, `top` and most C libraries read it.

**Programs:** BusyBox `sh` scripts with `trap`, `timeout`, `sleep`, `ps`, `time`;
LTP's signal and process cases.

### L3. Memory (10, plus `mmap` finished)

File-backed mappings - private and shared - through the page cache, `MAP_FIXED`,
`mremap`, `madvise`, `msync`, `mincore`, the `mlock` family, `memfd_create`.
This is the phase that makes **glibc** possible: its dynamic loader maps
libraries with `MAP_FIXED`.

**Programs:** the corpus rebuilt against glibc, dynamically linked; `sqlite3`
with memory-mapped I/O; `lua`.

### L4. Event loops (21)

`poll`, `select`, `pselect6`, `ppoll`, `epoll`, `eventfd`, `timerfd`,
`signalfd`, `inotify` - each a file type from A3 with a wait queue, not a special
case in the scheduler.

**Programs:** BusyBox `httpd` serving from the guest, `nc`, `tail -f`.

### L5. Sockets (11, plus `readv`/`writev` on sockets)

The rest of the BSD API (`sendmsg`/`recvmsg`, `shutdown`, `getsockname`,
`getpeername`, `socketpair`, socket options, `accept4`, `sendmmsg`/`recvmmsg`)
and **AF_UNIX** stream and datagram sockets, with `SCM_RIGHTS` on A3's open file
descriptions.

**Programs:** BusyBox `wget` against the host, a DNS lookup through the C
library, a client and server over a Unix socket.

### L6. Threads and scheduling (20, plus `futex` and `clone` finished)

The rest of `futex` (requeue, wake-op, the `futex2` calls; priority inheritance
if a program needs it), robust lists, CPU affinity, the `sched_*` parameters
mapped onto the scheduler's classes, `membarrier`, `getcpu`.

**Programs:** glibc's pthread test programs, a thread-pool workload across four
cores; LTP's futex and sched cases.

### L7. IPC (21)

System V shared memory, semaphores and message queues; POSIX message queues;
`splice`, `tee`, `vmsplice`.

**Programs:** LTP's ipc cases; a producer and consumer over SysV shared memory.

### L8. System administration (11, plus `uname` finished)

`mount`/`umount2` over the mount table, `chroot`, `pivot_root`, `reboot`,
`sethostname`, `swapon`/`swapoff` over the existing swap area, `syslog` over the
kernel log.

**Programs:** an init that mounts its filesystems and switches root; BusyBox
`mount`, `dmesg`, `reboot`.

### L9. Security (6)

Capabilities (`capget`/`capset`) on top of L2's credentials, `seccomp` in filter
mode, Landlock. This is where [../vision.md](../vision.md)'s security goals -
isolation, privilege domains, sandboxing - become syscalls rather than host
models.

**Programs:** a seccomp-sandboxed program killed for a forbidden call; LTP's
capability and seccomp cases.

---

## Deferred and refused

**Deferred (50):** NUMA, legacy AIO, `io_uring`, namespaces, the new mount API,
fanotify, file handles, quotas, `ptrace`, key management, memory protection keys
and the rest listed in [syscalls.md](syscalls.md). ENOSYS, counted as deferred.
Each moves into a phase the day a corpus program asks for it.

**Refused by decision (33):** syscalls Linux itself removed or never implemented
(`uselib`, `create_module`, `_sysctl`, `tuxcall`, ...), loading code into the
kernel (`init_module`, `kexec_load`, `bpf`), raw hardware access (`iopl`,
`ioperm`, `modify_ldt`), and `rseq`, whose ENOSYS the C library already handles.
Each has its errno and its reason in the registry.

## Order and dependencies

A0 and A1 first: they make every later step measurable. A2 before A3, because
A3's open file description is the first thing the host tests will exercise. A4
depends on A3 (a directory descriptor is a file type). A5 can run beside any of
them.

L1 needs A3 and A4. L3 is what unlocks glibc, so it comes before L6, whose
target programs are glibc's. L4 needs A3's file types. L5's `SCM_RIGHTS` needs
A3's descriptions. L9 needs L2's credentials. L7 and L8 are the least entangled
and can move if a program asks earlier.

## Where the refactor started from

The layer was built one syscall at a time as each program needed one, which is
how 73 rows got written and is not how 373 will be. C4 (2026-09-21) gave it one
table, one validation engine and one operation vocabulary; A1-A5 give the rows
beneath that table a registry, a portable home, descriptors, paths and layouts
that match Linux. The second syscall dispatcher `docs/roadmap.md` listed as an
open decision was deleted in C3 and is not part of this plan.
