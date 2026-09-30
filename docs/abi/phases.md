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

- The corpus: BusyBox as the boot image stages it (Ubuntu's `busybox-static`,
  which is linked against glibc), the musl test programs, and - from their
  released sources, built against musl - `sqlite3`, `lua`, and LTP's
  `testcases/kernel/syscalls`.
- A checked-in needs list per workload, measured under strace on Linux, with the
  system files each opens.
- A report: for each workload, needed / done / partial / missing; across the
  corpus, the missing ones by phase and by who asks.

**Done when** the report exists for every corpus program and runs in the nightly.

**Status (2026-09-29): done.** `tests/corpus/` holds 25 workloads - sixteen of
BusyBox, the six musl test programs, two of `sqlite3` and one of `lua` - measured
by `scripts/dev/corpus-measure.sh` and reported by `scripts/dev/corpus-report.py`
in [corpus.md](corpus.md): 80 distinct syscalls asked for, 41 done, 16 partial,
23 missing, 13 workloads ready (Lua among them; `sqlite3` waits on `fcntl`
locks, `pread64`/`pwrite64` and `fsync`). The musl programs come out ready,
which is the report's own check. `scripts/dev/corpus-build.sh` downloads SQLite,
Lua and LTP from pins with their SHA-256 (`tests/corpus/sources.txt`) and builds
them static against musl. 1,530 of LTP's syscall tests build: 1,331 matched to
320 syscalls - an oracle for 73 of L1's 78 - and three directories do not build
(`fmtmsg`, `timer_create`, `utils`). The nightly job `corpus-needs` builds SQLite
and Lua, measures again and fails on drift.

The first LTP build produced 378 tests, not 1,530: its top-level make stops at
the first directory that fails, `-k` or not, and `fmtmsg` fails early in the
alphabet. The script builds each directory on its own now. LTP's tests are
measured as an oracle - which exist and build - not traced one by one: most
need root on the host, and what they exercise is by construction the syscall
they are named after.

On the way, `trace-linux-binary.sh` - the tool this whole method rests on -
turned out to report full coverage for every binary: it looked for handlers as
`case LSYS_` in `arch_hw.c`, gone since C4, and its name extraction no longer
matched strace, so both lists were empty. It reads the table now (M-077).

### A1. The registry

**Objective.** One source of truth for all 373 numbers (invariant 6), and two
counters where there is one (invariant 7).

- A registry file (number, name, state, phase, reason) replaces
  `scripts/dev/linux-syscall-numbers.txt`, which listed only the
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

**Status (2026-09-29): done.** The registry is `kernel/abi/linux_syscalls.def`,
an X-macro list - `SYSCALL(number, name, state, phase, errno, "why")` - that the
C code and the scripts both read: 373 Linux numbers seeded from Linux's
`asm/unistd_64.h` and the plan's table, plus VibeOS's two own. States are DONE,
PARTIAL, MISSING, DEFERRED and REFUSED; each of the 33 refusals carries its own
reason, seven of them answer EPERM rather than ENOSYS.

- The dispatcher answers a number with no row from its line: a refusal with its
  errno, counted as `refused`; a deferred call with ENOSYS, counted and reported
  by the gate by number (`abi_deferred_syscall_nr`); a missing one as before
  (`abi_unimplemented_syscall_nr`). `hello` asks for `iopl` on purpose and
  expects EPERM, so the refusal count is seen moving on every boot
  (`abi_refused_unproven` otherwise).
- The boot refuses to start when a row and its line disagree in either
  direction, and `check-syscall-checks.py` holds the same rule against the
  sources - reading the registry instead of `linux-syscall-numbers.txt`, which is
  gone. A registry line it cannot parse is a failure: the first version skipped
  one with a space before a comma, which the compiler accepted.
- [syscalls.md](syscalls.md) is written by `make-syscall-table.py` from the
  registry, and `check.sh` runs it with `--check`, so the published table fails
  the build when it drifts. Not generated in the book build, as planned: a stale
  page is caught before the push, which is earlier.
- Host test `test_linux_registry`; sabotage `abi-registry.txt` (iopl missing,
  iopl deferred, read without a row - each red for its own reason) and
  `abi-registry-dispatch.txt` (the refusal path removed, the count removed).
  `check-sabotage-anchors.py` did not know `.def` files and reported the first
  as anchored nowhere; it does now.

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

**Status (2026-09-29): done.** Nothing under `kernel/abi/` names the architecture
any more: no `hw_*`, no `g_tasks`, no architecture global, no inline assembly,
no `arch_hw_internal.h`. Every `hw_sys_*` is `linux_sys_*`.

- **The interface is not Linux's.** It is `include/vibeos/ksvc.h`, *kernel
  services*, with a `ks_` prefix: 92 functions over tasks, locks and waiting,
  user memory, the address space, processes, registers and devices. A second
  personality - Windows is planned - is another directory beside
  `kernel/abi/linux` that includes the same header. The architecture implements
  it in `kernel/arch/x86_64/ksvc.c`, mostly one line over a function that
  already existed. What is more than a line is what used to sit in the handlers
  and is really about the machine: the mm lock's interrupt window, a fresh
  page's leaf bits, reading an entry for `pageinfo`, a fork's or a thread's
  registers, an exec's entry, the signal frame, the TLS MSR.
- **Portable structures.** The process state, the image and the lock layout
  moved to `include/vibeos/procstate.h`, and the architecture keeps its names
  for them as typedefs. Linux errno and signal numbers moved to `abi_linux.h`.
  The layer's exports to the kernel - the syscall entry, signal delivery,
  descriptor copies, futex wake - are `include/vibeos/linux_exports.h`.
- **Reusable across personalities.** The pointer checks a row declares run in
  `kernel/abi/abi.c` (`vibeos_abi_check_pointers`, handed the personality's own
  range check and bad-address error), and the row macros are
  `include/vibeos/abi_rows.h`. The architecture's entry only reads registers;
  the dispatcher is `linux_syscall(frame, nr, a[6])`.
- **Moved, not rewritten.** The futex table and its wake left the
  architecture's task code for `kernel/abi/linux/futex.c`, with the wait. The
  descriptor claim and a pipe end's release left it for `fs.c`. mmap's two
  mapping loops, which differed only in the leaf, became one.
- **Host tests.** `tests/kernel/ksvc_fake.c` is the other implementation: a task
  table, user memory with a window that can be made to fault (a sibling's
  munmap between the check and the copy), a filesystem and a network stack in
  memory, and waits, exits and panics that return to the test instead of
  hanging it. `linux_abi_tests.c` runs the handlers through `linux_syscall`:
  pipes, dup2, the console, the pointer engine, sigset numbering, sigaction,
  kill across sessions, a forged signal frame, the registry's three answers,
  fork. **Every PARTIAL row has a gap expectation** written as what Linux does;
  it fails today for the reason the registry names, a gap that starts passing
  fails the test until the line says DONE, and a gap for a line that is not
  PARTIAL fails too. `rseq`, partial by decision (phase R), asserts the
  decision instead.
- **Checks.** `check-abi-layering.py` (in `check.sh`): no personality file names
  the architecture - the forbidden globals are read from `arch_hw_internal.h`,
  after the first version knew only `g_tasks` and let a sabotage reading
  `g_timer_ticks` through - and both implementations define every service.
  `check-syscall-checks.py` followed one-line definitions into the next
  function (its body ended at the next `\n}\n`), which made `close` "reach" the
  user-range check through `ks_lock`; it matches braces now, and its listing is
  identical to the one before the refactor for all 68 operations.
  `check-chokepoints.py` counts `include/` too and the `ks_` doors beside the
  `hw_` ones. 26 sabotage anchors were re-pointed, 12 by applying the code's own
  renames and 14 by hand.
- **Sabotage.** `abi-host-handlers.txt` (sigset numbering, a gap closed without
  the registry, a forged frame resumed) and `abi-layering.txt` - all red for
  their stated reason.

**Measure.** References to `hw_*`/`g_tasks` in `kernel/abi/linux/`: ~700 to 0.
Blast radius for "add a syscall" stays 2 (abi.h and the file with the handler),
plus its registry line - not the one the plan hoped for: the second is the
operation's declaration in abi.h, which every personality shares and which is
kept on purpose.

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

**Status (2026-09-30): done.**

- **Open file descriptions** (`include/vibeos/file.h`, `kernel/fs/file.c`): a
  counted object holding the offset, the status flags and each type's state,
  in a pool of 256 with its own lock. The last reference runs the type's
  release - a pipe end given back, a socket closed, a written file committed -
  and a slot is not handed out again until that release has finished. A put
  with nothing to put is `file_put_underflow` in the must-be-zero registry.
- **The table** (`fdtable.h`) holds references and one flag per number,
  close-on-exec, and grows a page of 256 at a time up to 1024 (RLIMIT_NOFILE,
  which prlimit now reports). Descriptors 0-2 are ordinary entries: a spawned
  program gets one console description on all three, so closing 1 and opening
  a file gives the file 1, as every shell's redirection expects. fork copies
  references, exec copies and then drops the close-on-exec numbers, and the
  last thread to leave destroys the table.
- **One operations table per type**, in `kernel/abi/files/` beside the
  personalities - regular file, directory, pipe end, socket, console - written
  against `vibeos/ksvc.h`, so a Windows personality uses the same ones. `read`,
  `write`, `lseek`, `fstat`, `ioctl` and `getdents64` dispatch by type: a pipe
  is a FIFO and a socket a socket to fstat, both ESPIPE to lseek. The socket
  waits (connect, accept, recvfrom) moved with the type, leaving the Linux
  handlers the sockaddr translation only. Pipe ends are counted per
  description, so dup and fork no longer touch a pipe's counts; the five sites
  that had to remember an acquire became none.
- **Every call holds a reference** to the description it works on for its
  whole length (Linux's fdget/fdput), so a sibling thread's close takes the
  number away and not the file. A socket call still re-checks the socket's own
  tenancy (M-020), because a process's exit releases the sockets it owns.
- **New and finished syscalls**: `dup3` (DONE), `fcntl` (PARTIAL: descriptor
  and status flags, F_DUPFD and F_DUPFD_CLOEXEC; record locks answer ENOLCK
  rather than pretend), `close_range` (PARTIAL: CLOSE_RANGE_UNSHARE refused),
  `pipe2` honours O_NONBLOCK and O_CLOEXEC, `open` honours O_CLOEXEC, `socket`
  honours SOCK_CLOEXEC. `open` stays PARTIAL for the working directory only.
- **Host tests**: dup shares an offset, 200 files at consecutive numbers, a
  closed stdout reused, a pipe is a FIFO with ESPIPE and non-blocking EAGAIN,
  close-on-exec through open, dup3, F_DUPFD_CLOEXEC and close_range, and a
  forked child reading on from its parent's offset; the table and description
  layers have their own. The `open` gap for four descriptors closed on its own
  when the table grew and failed the test until the registry said so, which is
  the gap mechanism doing its job; its expectation is the working directory now,
  and fcntl and close_range carry theirs.
- **Checks**: `check-net-stable.py` follows the waits into `socket.c` and
  matches braces - its one-line signature parser saw two of four waits and said
  so instead of passing. `check-task-identity.py`'s count of lines indexing the
  table's arrays went to zero. The host test runner is unbuffered: a sabotage
  of the table's page clearing was named by its test and the line was lost when
  a later group crashed on the same garbage.
- **Sabotage**: `fs-file.txt` (3), `fs-fdtable.txt` rewritten (4 - the C5 cases
  named code that no longer exists), `abi-files.txt` (3), ten anchors
  re-pointed; all red for their reason, the page-clearing case by its test
  and then a crash.
- **Not done**: the write-back still replaces a whole file with at most 512
  buffered bytes, so O_RDWR on an existing file opens for reading only - a gap
  of the filesystem layer, recorded in `regular.c`. And eventfd, timerfd,
  signalfd and epoll are future types (L4).

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

**Status (2026-09-30): done.**

- **One path walk** (`include/vibeos/path.h`, `kernel/fs/path.c`), shared by
  every personality. A path is made absolute and normalised lexically against a
  root and a working directory - `.`, `..` and repeated slashes, with `..`
  stopping at the root - and then looked up through the mount table, so a path
  under a mount point reaches that filesystem rather than the boot volume. Every
  component before the last must be a directory (ENOTDIR otherwise), a component
  over 255 bytes or a result over 256 is ENAMETOOLONG rather than cut short.
  `vibeos_path_parent` answers for a name about to be created: its parent must
  exist, the name need not.
- **A working directory and a root per process**, in the process state beside
  the descriptor table and under its lock, inherited by fork, threads and exec.
  An open file description remembers the mount it was opened on, so a read, a
  directory listing and a write-back reach the right filesystem.
- **`dirfd` is honoured** by every *at call (`linux_path_at`): AT_FDCWD is the
  working directory, a directory descriptor its own path, anything else EBADF
  or ENOTDIR. New and finished rows: `chdir`, `fchdir`, `getcwd` (ERANGE for a
  short buffer), `mkdirat`, `openat`, `open` all DONE; `unlinkat` PARTIAL, since
  AT_REMOVEDIR has no filesystem here that removes a directory. `readlinkat` and
  `newfstatat` resolve relative paths too.
- **exec resolves its path** against the working directory, and `argv[0]` stays
  the name the caller gave - BusyBox still dispatches on it.
- **The interpreter substitution is deleted.** The loader is staged at
  `/lib/ld-musl-x86_64.so.1` on the boot volume and the kernel opens whatever
  path the program's PT_INTERP names. `check-exec-layering.sh`, which existed
  to keep the substitution to one function, went with it.
- **FAT reads long names.** It had to: the loader's name is not 8.3. VFAT
  entries are collected in front of their short entry and used only when their
  checksum matches it, so an orphaned long name left by a tool that did not know
  VFAT is ignored rather than misattributed. The ESP writer emits them, and the
  gate mounts a second image written by mtools (`FATLONG.IMG`, at `/fatlong`)
  and reads a file through a long directory name - an artefact neither side of
  the test produced, after the ISO9660 lesson.
- **Gated**: the boot self-test runs `cd -P /DOCS && pwd -P && cat NOTES.TXT`
  in the shell; `shell_cd_did_not_work` fails the boot unless the program
  printed `/DOCS` and the relative `cat` succeeded. The musl interpreter is
  loaded by path on every boot that runs a dynamic binary.
- **Host tests**: `path_tests.c` (normalisation, the root floor, NAME_MAX, the
  walk across two mounts, ENOTDIR and ENOENT, the parent rule) and, in
  `linux_abi_tests.c`, the working directory, relative open, the at calls with
  a directory descriptor, the errors, and fork inheriting the directory.
- **Sabotage**: `fs-path.txt` (4, host), `abi-paths.txt` (2),
  `io-fat-longnames.txt` (2), and a case for the new image row in
  `io-filesystems-gate.txt`; every one red for its reason. One of them only on
  the host: a directory descriptor ignored in favour of the working directory
  boots green, run to confirm it, because nothing the self-test runs opens
  relative to one. Breaking the long-name reader fails the image row *and* the
  dynamic binary - the loader's name is a long one.
- **Not done**: symbolic links and ELOOP - no filesystem here has them, so
  there is nothing to follow; `chroot` stays refused, although the root is now
  a real field. Both belong to L1.

### A5. Layouts and errno from Linux's headers

**Objective.** Invariant 8 for structures: `stat`, `statx`, `dirent64`,
`timespec`, `sigaction`, `siginfo_t`, `sockaddr_*`, `rlimit`, `utsname`,
`termios`.

- A host test compiled against the host's `linux/*.h` uapi headers asserts every
  offset and size the kernel writes, and every errno value it returns.

**Done when** the test exists and a sabotage that moves one field turns it red.

**Status (2026-09-30): done.**

- **One declaration of Linux's layouts** (`include/vibeos/linux_layout.h`):
  `stat`, `__kernel_timespec`, `new_utsname`, `sysinfo`, `sockaddr_in`, `iovec`,
  `rlimit64`, the kernel's `sigaction`, and the getdents64 record, with Linux's
  field names and padding named too - plus every constant a handler compares a
  user's argument against: the *at flags, fcntl commands, close_range, clone,
  wait4, mmap, prctl, arch_prctl, futex, rlimit, sigprocmask, socket and
  dirent-type numbers. The handlers used to write byte offsets and bare numbers
  (`rec[18] = 4`, `STAT_OFF_UID 28u`, `domain != 2u`); TIOCGPGRP and AT_FDCWD
  were spelled twice in two files, and the registry spelled EPERM and ENOSYS a
  second time. Each handler now fills a structure and copies it out once.
- **The file layer's numbers are Linux's by decision**, and named so: the open
  flags and mode bits were already `VIBEOS_O_*` and `VIBEOS_S_IF*`; seek origins
  (`VIBEOS_SEEK_*`) and the console's ioctl requests (`VIBEOS_IOCTL_*`) joined
  them. getdents64 is the one file operation whose output is a personality's,
  and says so.
- **The test** (`tests/kernel/linux_layout_tests.c`) compiles against the host's
  uapi headers and makes 232 comparisons: every field's offset and size, every
  structure's size, every errno value the kernel returns, every signal number,
  flag and constant. What uapi does not carry - `struct dirent64`, `AF_INET`,
  `SOCK_*`, `DT_*`, and the `S_IF*` types that `linux/stat.h` withholds from a
  glibc build - comes from the C library's headers, in a second file
  (`linux_layout_libc.c`), because the two sets of headers cannot share one.
  On a non-Linux host it says it was skipped; on Linux the includes are
  unconditional, so a runner without the headers fails to build rather than
  passing having compared nothing.
- **The test cannot fall behind the header**: `check-linux-layout.py` (in
  `check.sh`) fails when a field, a structure's size or a Linux-valued constant
  is declared and not compared, and when a file under `kernel/abi/` defines a
  constant under Linux's own spelling. Its first run found `FUTEX_CMD_MASK`,
  which is Linux's name for a different value (`~(PRIVATE | CLOCK_REALTIME)`,
  not `0x7F`); it is `VIBEOS_FUTEX_OP_BITS` now.
- **Sabotage**: `abi-layout.txt` (5: two fields swapped at equal size, a field
  widened, the dirent64 order, `mem_unit` widened where the structure's size
  cannot tell, AT_FDCWD off by one), `abi-errno.txt` (2), `dev-linux-layout.txt`
  (3, against the check); all red for their reason. AT_FDCWD is named by the
  layout test and by the handler tests, which run first.
- **The sabotage tool had a hole, and two cases had never run.** `sabotage.py`
  drops lines starting with `#` as comments, so an anchor that is a `#define`
  vanished, the case ran with an empty anchor, changed nothing and scored NOT
  RED - which is how the errno cases first came back. `check-sabotage-anchors.py`
  skipped an empty anchor too. Both refuse one now, and `\#` at the start of a
  line is a literal `#`. That found `core-dispatcher.txt`'s fence case and
  `services.txt`'s "only one service is started", which had never tested
  anything; both are escaped and red now - the fence case by
  `check-reachable.py`, the manifest case at boot as `missing:[CRASH] end`: with
  one service started, svc-crash never runs and the crash record the gate waits
  for never comes.
- **Not declared, because nothing writes them yet**: `statx` (L1), `siginfo_t`
  and a Linux `ucontext` (the signal frame is this kernel's own layout, and
  SA_SIGINFO handlers get no siginfo - L2), `termios` (the console answers
  ENOTTY). Each arrives with the phase that writes it, and the check makes it
  arrive with its comparison.
- **Found on the way**: getdents64's gap reason said "FAT names", which has been
  wrong since A4 gave FAT long names; names are cut at 15 bytes by the listing
  buffer, which is what the registry says now. Still L1's.

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
