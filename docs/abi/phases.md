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
  `fs-fat-longnames.txt` (2), and a case for the new image row in
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

**Where it starts from (2026-09-30).** The syscalls are the small part. Only FAT
is writable, and only a whole file at a time: a descriptor buffers 512 bytes and
the file is rewritten from them when the last reference goes, so a program that
writes more gets short writes. The filesystem interface (`vibeos/vfs.h`) has
lookup, read, whole-file write, list, unlink and mkdir - no write at an offset,
truncate, rename, rmdir, links, attributes, statfs or sync. ext2, exFAT, NTFS
and ISO9660 are read-only. There is no wall clock (time is uptime) and no
credential model (L2). `stat`, `rename`, `chmod` and `symlink` need something
true to report before they are worth a row, so L1 builds that first:

1. **The filesystem interface grows the operations** - write and truncate on a
   node, create, rmdir, rename, link, symlink and readlink, attributes (mode,
   owner, times, link count), statfs and sync - optional per driver, with the
   wrapper answering as Linux does when one is missing: EROFS from a read-only
   filesystem, EPERM from one that cannot represent the thing. The path walk
   follows symbolic links (ELOOP after 40) and answers lstat's question too.
2. **tmpfs** (`kernel/fs/tmpfs.c`): the filesystem with all of it - modes,
   owners, hard and symbolic links, timestamps, rename over an existing name,
   sparse files - mounted at `/tmp`. Portable, host-tested, and a nightly
   torture against a model, per the rule that every module gets one.
3. **Writing through a descriptor**: regular files write at their offset through
   the node, with O_CREAT/O_EXCL/O_TRUNC/O_APPEND as Linux means them, and
   `pread64`, `pwrite64`, `preadv(2)`, `pwritev(2)`, `truncate`, `ftruncate`,
   `fsync`, `fdatasync`, `sync`, `syncfs`, `fallocate`, `fadvise64`,
   `readahead`, `copy_file_range`, and `sendfile` served rather than refused.
   A filesystem without in-place writes keeps the whole-file path, without the
   512-byte ceiling.
4. **FAT writes in place**: write at an offset, truncate, rename and rmdir in
   the FAT driver, so the boot volume is a real filesystem too.
5. **Names and metadata**: `stat`, `lstat`, `statx`, `access` and the
   `faccessat`s, `rename` and `renameat(2)`, `rmdir` and AT_REMOVEDIR, `link`,
   `symlink`, `readlink` against real links, `creat`, `mknod`, `openat2`, the
   `chmod`, `chown` and `utime` families, `umask`, `statfs`/`fstatfs`.
6. **The rest of the file calls**: `flock` and fcntl record locks, `getdents`,
   getdents64 without its 15-byte names, the xattr calls (EOPNOTSUPP from every
   filesystem here), `sync_file_range`, close_range's UNSHARE.
7. **A minimal terminal**: the console answers TCGETS, TCSETS, TIOCGWINSZ and
   the process-group requests, with ECHO and ICANON honoured.
8. **The programs**: the BusyBox file workloads of the corpus run in `/tmp`
   under the gate, `sqlite3` on a file, and the LTP cases for these syscalls
   staged and run.

**Step 1 (2026-09-30): done.**

- `vibeos_fs_node_t` carries mode, link count, owner and three times. The
  lookup wrapper zeroes the node before asking the driver and completes what
  the driver left out (a type from `is_dir`, 0755 or 0644, one link), so five
  drivers written before the fields existed hand no stack garbage to stat.
- Eleven operations beside the original six - `write_at`, `truncate`,
  `create`, `rmdir`, `rename` (with NOREPLACE), `link`, `symlink`, `readlink`,
  `setattr`, `statfs`, `sync` - each optional, returning 0 or a negated errno.
  A missing one is EROFS from a filesystem that writes nothing, EPERM from one
  that writes but cannot do this, EOPNOTSUPP for `write_at`/`truncate`/`create`
  where the caller keeps a whole-file fallback. `vibeos_fs_set_clock` is the
  registration timestamps read from. The five drivers' tables are designated
  initialisers now; positional ones were one field away from a hole.
- `vibeos_path_walk` replaces `vibeos_path_lookup` and `vibeos_path_parent`: it
  takes a root, a base directory and the path as written, follows links one
  component at a time (a relative target from the link's directory, an
  absolute one from the root, ELOOP after 40), applies ".." to the path
  resolved so far - so `link/..` is the parent of where the link points - and
  with CREATE answers for a missing last component where it would be made.
  NOFOLLOW and a trailing slash behave as Linux's do.
- The Linux handlers walk through `linux_walk_at`; `newfstatat` honours
  AT_SYMLINK_NOFOLLOW and reports owner, link count and times, `unlinkat` and
  `mkdirat` do not follow the last component, exec runs the program a link
  points at, and `readlinkat` reads real links (the `/proc/self/exe` answer is
  kept, recognised before a walk would call `/proc` missing).
- Errno values for what comes next (EACCES, EBUSY, EXDEV, EFBIG, ENOSPC, EROFS,
  EMLINK, ENOTEMPTY, ELOOP, EOPNOTSUPP), and S_IFMT, S_IFBLK, S_IFLNK - all
  compared with Linux's headers, which the layout check made non-optional.
- Host tests: 16 walk cases (links relative, absolute, chained, across a
  mount, in the middle of a path, cyclic, dangling, a target shorter than the
  link with path after it) and 12 for the wrappers. Sabotage: `fs-path.txt`
  gained 5 cases and `fs-vfs.txt` has 4; all red. Removing the link bound is
  red with ENAMETOOLONG rather than a hang: every splice leaves a separator
  behind, so the pending buffer refuses a cycle too - a second defence, and
  not the right errno.

**Step 2 (2026-09-30): done.**

- `kernel/fs/tmpfs.c`: 1024 inodes; a file's data in pages through sixteen
  direct pointers, one indirect and one double-indirect page (up to about a
  gigabyte), a page never written is a hole; a directory is a file of
  fixed-size records holding whole names. Modes, owners, three times, hard and
  symbolic links, rename over an existing name (NOREPLACE checked first, as
  Linux does; a directory cannot move inside itself), rmdir of empty
  directories, statfs with TMPFS_MAGIC. A freed inode's generation moves on, so
  a description left holding it gets ENOENT rather than the next file's bytes -
  the known gap is that the last unlink frees at once, before the last close.
- **It never allocates under its own lock.** The allocator can reclaim, and the
  lock masks interrupts; so an operation takes up to three spare pages first,
  the locked code takes from them, and the rest go back after. Writes go a
  page at a time for the same reason. The host test's allocator fails the test
  if it is called with the lock held.
- Mounted at `/tmp` at boot, 32 MiB at most, its pages through the admitted
  door for user memory; timestamps read uptime until L2 has a wall clock. The
  gate asserts `tmpfs_not_mounted` from its own line, and the shell writes a
  file there and reads it back (`tmpfs_round_trip_failed` - the contents are
  the shell's arithmetic, so the echoed command cannot stand in for them).
- A must-be-zero, `tmpfs_bad_record`: a directory record naming a free inode,
  or one without its page, is refused as EIO and counted.
- Host tests (`tmpfs_tests.c`, 70 checks besides the helper) and a torture against a model that
  shares no code with it (`tmpfs_torture.c`: eleven paths, twelve operations,
  every return value, every byte read, every path's type, size and link
  count, and the pages and inodes held, after every round). It is the nightly
  `tmpfs-torture` job, 300 seeds sanitized, which covers `kernel/fs` - the
  nightly-coverage baseline goes from 8 to 7.
- Sabotage: `fs-tmpfs.txt`, 11 cases, all red under the host tests and 10 of
  them under the torture as well. Two lessons on the way. "The source's record
  is not found again" went NOT RED twice: with the target last nothing moves,
  and with the target just before the source the stale slot is exactly the new
  end, so the wrong removal removes the right record - only a record between
  them exposes it. And the torture could not see a stale generation until it
  learned to keep a node across rounds, as an open description does.

**Step 3 (2026-10-01): done.**

- **A regular file writes at its offset.** On a filesystem with `write_at` a
  description is "direct": `write` goes to the node through a kernel page,
  `read` comes from it, and the 512-byte buffer that used to be the whole file
  is not involved. A filesystem that only stores whole files - FAT, until step
  4 - keeps the old path: a file opened O_WRONLY or O_CREAT is replaced on
  release from what was written.
- **open's flags mean what Linux means.** O_CREAT makes the file at open, with
  the mode given less the process's umask; O_EXCL refuses an existing name and
  does not follow a link there; O_TRUNC empties at open; O_APPEND finds the
  end at every write, so two appenders interleave; O_RDWR reads and writes;
  O_DIRECTORY and O_NOFOLLOW are honoured; an access mode of 3 is EINVAL.
  O_WRONLY without O_CREAT on a missing file is ENOENT - it used to make one.
- **File types gained four operations** - `pread`, `pwrite`, `truncate`, `sync`
  - and their tables became designated initialisers. A type without positions
  is ESPIPE to the positional calls, as on Linux.
- **Twenty rows**: `pread64`, `preadv`, `sendfile`, `fsync`, `fdatasync`,
  `creat`, `umask`, `sync`, `syncfs`, `sync_file_range`, `readahead`,
  `fadvise64` and `copy_file_range` DONE; `pwrite64`, `pwritev`, `truncate` and
  `ftruncate` PARTIAL until FAT writes in place (step 4); `preadv2`/`pwritev2` PARTIAL
  (RWF_ flags are refused, not ignored); `fallocate` PARTIAL (it guarantees
  the size, not the space, and only mode 0 and KEEP_SIZE). The registry stands
  at 75 done, 22 partial, 193 missing.
- **sendfile and copy_file_range copy in the kernel**, through one page whose
  address goes where a user address usually goes: every file type copies
  through `vibeos_uaccess_copy`, which does not care whose memory it is. At
  most a megabyte a call, because a syscall runs with interrupts masked.
- **The umask** is per process, beside the working directory, inherited by
  fork and exec.
- **Host tests** run on a tmpfs the fake kernel mounts at `/tmp`, as the real
  one does: five groups, 70 checks - the flags, a 5000-byte write, the
  positional calls and their vector forms, truncation, durability, the advice
  calls, both kernel copies, and the whole-file filesystem still working.
  **At boot** the shell writes 820 bytes in twenty writes, appends five with
  `>>` and counts them with `wc -c`; the gate wants 825
  (`tmp_write_path_failed`).
- **Sabotage**: `abi-write-path.txt` (7) and `abi-file-calls.txt` (6), all red.
  Three went NOT RED first, and each was read before anything was changed:
  - *ftruncate on a read-only descriptor* was refused for the wrong reason. A
    read-only description was not "direct", so it fell into the whole-file
    branch - and its `fstat` and SEEK_END reported the size from when it was
    opened while another descriptor grew the file. A defect, fixed: "direct" is
    the filesystem's property, not the access mode's.
  - *the whole-file writer taken for one that writes in place*: the test only
    created new files, which take the create path; replacing an existing one
    is tested now.
  - *umask keeping bits above 0777*: the test never set one.
- **A check had stopped watching.** `check-user-access.py` looked in
  `kernel/abi/linux/` only; A3 moved read, write, getdents64, the console and
  the sockets to `kernel/abi/files/`, so the sites it was written for (M-050 to
  M-052) had been unwatched since. The case that hands read()'s user buffer to
  the filesystem went NOT RED, which is how it was found. It scans all of
  `kernel/abi/` now: 53 casts, none raw.

**Step 4 (2026-10-01): done.**

- **The driver left the architecture first** (`kernel/fs/fat.c`,
  `include/vibeos/fat.h`): it reaches the disk, its lock and the mount hook
  through registrations, so the host tests link the same file the kernel runs.
- **A file is its directory entry.** The node id is where the 32-byte entry
  sits (sector and slot), which is the one thing FAT has that names a file
  without naming its path; every operation on a node reloads the entry from
  there and refuses a slot that no longer holds a file (ENOENT). The known gap
  is the one that follows from it: a description held across an unlink or a
  rename loses its file, and after the slot is reused it names another. FAT has
  no inode to keep; an in-memory one is a later step's work if a program needs
  it.
- **Writes in place**: `write_at` grows the chain as needed and zeroes the gap
  when the offset is past the end - FAT has no holes and a cluster comes back
  from the allocator with whatever its last file left; `truncate` cuts the
  chain or grows it with zeros; a write that runs out of space keeps what
  fitted (the clusters that were left) and reports the short count.
- **Names**: an entry is created with its long name whenever the name is not
  8.3 upper case - the pieces, the checksum, a `~n` alias that no other entry
  in the directory has - and an 8.3 name in lower case is stored with the two
  NT flags rather than a long name, as Windows does. A directory grows a
  cluster when it fills (the fixed FAT16 root cannot, and says ENOSPC), and a
  name that used up the end-of-directory marker writes a new one.
- **rename, rmdir, setattr, statfs, sync**: rename writes the new name before
  removing the old, replaces an existing file (NOREPLACE honoured), refuses a
  directory moved into itself, and updates a moved directory's ".."; rmdir
  wants it empty; setattr keeps the read-only bit and the modification time,
  and refuses an owner other than root (EPERM) - FAT has nowhere to put one.
- **The whole-file path is gone.** With no filesystem left that stores whole
  files, `kernel/abi/files/regular.c` lost the 512-byte buffer, the write-back
  on release and the "direct" flag; `pwrite64`, `pwritev`, `truncate` and
  `ftruncate` are DONE. The registry stands at 79 done, 18 partial, 193
  missing.
- **Host tests** (`tests/kernel/fat_tests.c`): a volume the driver formats in
  memory, filled with 0xE7 first so nothing is zero by luck - names of every
  kind, in-place writes, gaps, truncation, a directory grown to forty long
  names, rename in each of its cases, times, statfs, a volume filled to its
  last cluster, three hundred create/unlink cycles in the root.
- **Somebody else's reader.** Those tests can only say the writer and the
  reader agree. `scripts/dev/verify-fat-mtools.sh` (in `check.sh`) has mformat
  make the volume and mcopy put a file on it, runs the driver over it
  (`tests/kernel/fat_exercise.c`), and compares what mdir and mcopy find with
  what the driver believes: names, contents, free space. mtools follows
  whatever the table says, so `scripts/dev/fat-fsck.py` checks what it cannot -
  lost and cross-linked clusters, chains against sizes, "..", orphaned long
  names - and is run on mformat's own volume first.
- **At boot** the shell writes 866 bytes on the boot volume in twenty-two
  writes and counts them, writes and reads back a file under a long name
  (`fat_write_path_failed`), and cuts a two-cluster file with O_TRUNC. When the
  machine has stopped the gate runs fat-fsck on the image it left
  (`boot_volume_inconsistent:<what>`) and has mtools read the long-named file
  (`mtools_cannot_read_what_the_guest_wrote`).
- **Sabotage**: `fs-fat.txt` (15, all red under the host tests or mtools, each
  case says which), `fs-fat-boot.txt` (2, red), `abi-write-path.txt` re-pointed
  (6, red). What went NOT RED, and why:
  - *Under mtools, three cases at once*, because mformat's volume was zeros: an
    unzeroed gap, an unzeroed directory cluster and a missing end marker all
    read as correct on a disk that is zero wherever nobody wrote. The image is
    filled with 'A' before mformat now, and fat-fsck has a `--dirty` mode that
    overwrites everything past each directory's marker.
  - *An unzeroed directory cluster*, still: with room left in the old cluster
    the marker stays there and the new cluster is never read. The exercise
    fills a directory exactly to the end of its cluster. And then the missing
    marker - written down at first as unverifiable here - turned out to be
    what had been hiding it.
  - *The short write*: the test's volume happened to have exactly the clusters
    the write needed. It checks its own arrangement now (`0 < free < 4`).
  - *A sector that reaches the cache and not the disk*, at boot: green, the
    volume consistent and the file readable. Recorded in `fs-fat-boot.txt` as
    something this environment cannot tell, with the likely reason.
- **Still open**: timestamps are uptime, so every file is dated 1980-01-01
  until there is a wall clock (L2); the exec page cache is not told about a
  write to a file it holds.

**Step 5 (2026-10-01): done.**

- **Thirty-two rows**, twenty-nine in a new file, `kernel/abi/linux/names.c`,
  and three in `fs.c`: `stat`, `lstat`, `statx`; `access`, `faccessat`, `faccessat2`;
  `rename`, `renameat`, `renameat2`; `rmdir` and unlinkat's AT_REMOVEDIR;
  `link`, `linkat`, `symlink`, `symlinkat`, `readlink`; `chmod`, `fchmod`,
  `fchmodat`, `fchmodat2`; `chown`, `fchown`, `lchown`, `fchownat`; `utime`,
  `utimes`, `futimesat`, `utimensat`; `statfs`, `fstatfs`; `mknod`, `mknodat`;
  `openat2`. The registry stands at 108 done, 21 partial, 161 missing.
- **What Linux decides before a filesystem is asked is decided in the
  handler**, so every filesystem answers alike: a rename or a link across two
  mounts is EXDEV, a mount's root is EBUSY to rename and rmdir, a directory
  goes only over a directory and never into itself, `rmdir` of "." is EINVAL
  and of ".." ENOTEMPTY (read off the string the program wrote - the walk has
  already resolved both), a hard link to a directory is EPERM, an empty
  symlink target ENOENT. What a filesystem cannot do it says itself through
  the wrappers - EROFS, or EPERM for a symbolic link or an owner on FAT - and
  nothing in the handlers asks which filesystem it is.
- **One user, root, until L2.** No call refuses for want of permission, and
  `access` answers as it does for root on Linux: W_OK is EROFS on a filesystem
  that writes nothing, X_OK is EACCES on a regular file nobody may run.
- **`st_dev` is the mount.** Every filesystem reported device 0, so a file in
  `/tmp` and one on the boot volume with the same inode number were one file
  to `cp` and `mv`, which compare the pair to refuse copying a file onto
  itself. A mount's place in the table, from 1; 0 is a pipe or a socket.
- **A descriptor's file is walked again from its path** (`linux_walk_fd`):
  fchmod, fchown, futimens, fstatfs and every AT_EMPTY_PATH. The gap is the one
  a path for an identity always has - after a rename the description's path
  names nothing, or something else - and it is the same one step 4 recorded
  for FAT's nodes.
- **chown clears set-user-id** on a regular file, and set-group-id when the
  file is group-executable, as Linux does even for root. `mkdir` passes its
  mode, less the umask, where it used to drop it. `utimensat` takes UTIME_NOW
  and UTIME_OMIT and, with no path, is futimens.
- **statx** fills the basic set whatever was asked and says so in `stx_mask`;
  Linux has kept adding fields to the end of the structure, so only the part
  filled is declared field by field and the rest is "up to the size", compared
  as that (`TAIL` in the layout test). Six structures and twenty-four constants
  joined `linux_layout.h`; the layout test makes 366 comparisons.
- **Partial, and why**: `mknod`/`mknodat` make regular files only - a FIFO or a
  device node is EPERM, because no filesystem here has anywhere to keep one and
  a name that looked like a FIFO and opened as an empty file would be worse;
  `renameat2` refuses EXCHANGE and WHITEOUT (EINVAL, Linux's answer from a
  filesystem with neither); `openat2` honours RESOLVE_NO_MAGICLINKS only - the
  others are ENOSYS, which a caller already handles by falling back to openat,
  where EINVAL would say its arguments were wrong.
- **Host tests**: four groups in `linux_abi_tests.c` on tmpfs, on the fake
  kernel's root (which can neither rename nor link) and on a filesystem mounted
  for the purpose that writes nothing. **At boot** the shell runs mv, ln,
  ln -s, chmod, mkdir, rmdir and stat in `/tmp` and the gate reads stat's own
  line, `META_600_2_2` (`names_and_metadata_failed`); then mv, mkdir and rmdir
  on the FAT volume (`fat_rename_failed`), which the gate's consistency check
  judges after the machine stops.
- **Sabotage**: `abi-names.txt` (31), `abi-names-fs.txt` (6),
  `abi-names-boot.txt` (2) and two more in `fs-fat-boot.txt`, all red. Two went
  NOT RED first: RENAME_NOREPLACE left to the filesystem, and an empty symlink
  target accepted. tmpfs refuses both itself, so on tmpfs the handler's check
  could not be seen. What the check is for is a filesystem that cannot rename
  or link at all giving Linux's answer (EEXIST, ENOENT) and not its own
  (EPERM) - so both are asked of the fake's root now. A FAT rename that leaves
  the old name behind is invisible from inside the guest and red as
  `boot_volume_inconsistent:cross_linked`.
- **Still open**: an open description does not follow its file across a
  rename; `fchmod` and `fchown` on a pipe or a socket are EINVAL where Linux
  changes an inode nobody can see; the remaining handler checks that tmpfs
  repeats (a file over a directory, a directory into itself) have no case of
  their own for the reason above.

**Step 6 (2026-10-01): done.**

- **Locks** (`kernel/fs/filelock.c`, `include/vibeos/filelock.h`): one table,
  personality-neutral, of two kinds kept apart as Linux keeps them. *Record
  locks* are byte ranges held by an owner - a process for `fcntl`'s F_SETLK,
  F_SETLKW and F_GETLK, an open file description for the F_OFD_ forms, both in
  one space so each sees the other's. An owner asking again replaces what it
  held: the table splits a lock whose middle is unlocked and joins two when
  the gap between them is filled, so an owner's locks are always disjoint and
  never two where one would do. *Whole-file locks* are `flock`'s, held by the
  description. The table never waits and never allocates; it refuses with the
  owner in the way, and checks it has room for the worst case before it
  changes anything.
- **The handlers do the waiting**, the way a pipe's reader waits, and stop for
  a signal. Before a process waits for a record lock the table is asked
  whether the holder is, through however many others, waiting for it: EDEADLK
  instead of two waits for ever. Linux looks for no deadlock among
  description-owned locks, and neither does this.
- **Lifetimes, which are the hard part of POSIX's locks**: closing *any*
  descriptor a process has for a file gives back every record lock the process
  holds on it; a process's locks end at its exit and are not inherited by
  fork; a description's locks - flock, OFD - go when the description does, so
  a dup and a fork share them. A lock at SEEK_END is where the file ends when
  it is taken; a length of 0 runs to the end wherever that goes; a negative
  one is the bytes before the start.
- **Directories**: the file layer hands out entries by position
  (`vibeos_dirent_t`, the `readdir` operation that replaced `getdents`), and
  the record is the personality's - so `getdents64` and the older `getdents`
  are two formats over one reader, and a Windows directory query will be a
  third. Names are whole (they were cut at fifteen bytes), "." and ".." are in
  every directory once, each entry carries the inode number stat reports and
  its real type (a symbolic link was listed as a file, and a program that walks
  a tree trusts that), and the position is the description's own, so
  `lseek(fd, 0)` rewinds and a `d_off` can be returned to.
- **Extended attributes**: the twelve calls say what Linux says of a
  filesystem that stores none - EOPNOTSUPP to setting, getting and removing,
  an empty list - after refusing what Linux refuses first, in its order: a
  flag, a name (ERANGE), a size (E2BIG), and the file itself, so a mistyped
  path is ENOENT and not "not supported".
- **close_range's UNSHARE** asks for nothing in a process of one thread and is
  honoured there. With threads it is still refused: a thread cannot hold a
  table apart from its process until L6, which owns that row now.
- **Sixteen rows closed**: `flock`, `getdents`, the twelve xattr calls, and
  `fcntl` and `getdents64` from partial to done. The registry stands at 124
  done, 19 partial, 147 missing.
- **`struct linux_dirent`**, the old call's record, is declared by nobody -
  Linux keeps it to itself and no C library has used the call in years. A
  fixture this project wrote would only agree with the declaration this
  project wrote, so the layout test asks the *host's kernel* to list "/" with
  that call and reads the answer through our structure: the records have to
  chain, and "." and ".." have to be there with an inode and a directory's
  type in the last byte.
- **Host tests**: the table on its own (`filelock_tests.c`), four groups of
  handlers in `linux_abi_tests.c`, and a torture in the nightly
  (`filelock_torture.c`, 300 seeds sanitized, three seeds in `check.sh` and in
  every host sabotage run). Its model has no ranges: a file is seventeen
  cells, an owner paints them, and every round compares the answer, a probe of
  every cell for every owner, and the number of locks held against the number
  of runs of equal cells - one more is a join that was missed.
- **At boot** the ring-3 self-test takes a record lock and a flock, forks, and
  the child finds both in its way (`file_locks_selftest_failed`) - nothing else
  the boot runs takes a lock, and the BusyBox on the image has no flock applet;
  the shell lists a directory holding a long name and a dangling link, and the
  gate reads ls's own line and `DIRS_4_1` (`directory_listing_failed`).
- **Sabotage**: `fs-filelock.txt` (15), `abi-locks.txt` (20),
  `abi-readdir.txt` (5), `abi-xattr.txt` (6) and one more in `fs-file.txt` on
  the host; `abi-locks-boot.txt` (2) and `abi-readdir-boot.txt` (2) at boot. All
  red, each for the expectation it names. One went NOT RED first: a pipe that
  cannot be told from another pipe. The test locked one pipe, and one of
  anything has an identity whatever it is computed from; it locks two now, and
  the other end of the first.
- **A new must-be-zero**, `filelock_unlocked`: the table used with no lock
  registered, as the mount table counts it.
- **Still open**: an exec keeps every record lock the process holds, those on
  its close-on-exec descriptors included, where POSIX gives those back as the
  descriptors close; a lock is found by the filesystem's identity for the
  file, so on FAT it follows the directory entry (step 4's gap); mandatory
  locking does not exist, as on current Linux.

**Step 7 (2026-10-01): done.**

- **The console is a terminal** (`include/vibeos/tty.h`,
  `kernel/abi/files/console.c`). It was a character device that answered ENOTTY
  to every terminal question but the foreground process group - by design, and
  truthfully - read a line at a time, echoed always, and handed back whatever
  had been typed when the keyboard went quiet, half a line included. It has
  modes now, one set for the one terminal, in Linux's numbering as the open
  flags are; another personality translates its console modes into them.
- **What is honoured**: ICANON - a read returns one finished line, erasable
  until Enter, and waits for it; without it a read returns as soon as MIN bytes
  are typed, with MIN 0 at once. ECHO, ECHOE, ECHONL. The erase, kill and
  end-of-file characters. ICRNL on input; OPOST with ONLCR on output. Typed-
  ahead input is taken from the keyboard only as far as a read needs it, so
  what comes next is read in whatever mode the terminal is in by then.
  Everything else is stored and given back as set - which is what lets a
  program save the modes and restore them - and no more: the interrupt
  character interrupts with ISIG clear, and TIME is not a timer.
- **The requests** are the personality's, as their structures are: TCGETS,
  TCSETS, TCSETSW, TCSETSF, TIOCGWINSZ and TIOCSWINSZ on the console, ENOTTY
  on anything else; and four any descriptor answers - FIOCLEX, FIONCLEX,
  FIONBIO, FIONREAD (a file, a pipe, the terminal). `ioctl` stays PARTIAL for
  what is left: ISIG, TIME, TCFLSH, TIOCSCTTY and the rest.
- **A terminal changes what every program does**, and the boot found out at
  once. A C library buffers a terminal by lines and writes a line as two pieces
  of one `writev`, which this kernel wrote as two writes: `THREADS_C5_EXEC_OK`
  arrived as `THREADS_C5_EXEC` and `_OK` with a log prefix each, and five gate
  assertions failed on output that was correct. A `writev` that fits a page is
  gathered and written once, as Linux writes it. `ls` lays its names out in
  columns for the width TIOCGWINSZ reports.
- **And a shell on a terminal is interactive.** BusyBox's ash prints a prompt,
  puts the terminal in raw mode and edits its own command line, asking
  `poll()` before every key - a syscall this kernel did not have, so the first
  boot printed "poll: Function not implemented" once per character of the
  self-test. `+i` does not turn that off in BusyBox. So **`poll` is here ahead
  of L4**, as much of it as the terminal needs: each file type says what it
  can do without waiting (`ready`: the console, a pipe; a file is always
  ready), and the wait is the pipes' wait. PARTIAL, owned by L4: a socket
  always reports ready. The self-test's shell session is a real interactive
  one now, every command typed a key at a time through raw mode.
- **A defect three steps old, found by reading the log this step changed.**
  The native shell's `write` opened its file with `O_CREAT` alone, which is
  read-only. That worked while a file was replaced on release from whatever
  had been written; from step 4, when a write went into the file at once, the
  write was refused, nothing looked at its result, the shell printed
  "written", and the file was empty. Every `cat DOCS/NOTES.TXT` in the boot -
  five of them, four programs - printed nothing for three steps, and the gate
  stayed green: the line "persistent hello" moved the gate's reported *phase*
  and was asserted by nothing. It is asserted now (`notes_file_not_read_back`,
  five times), the shell opens to write and checks what write returns, and the
  sabotage case is the defect as it shipped.
- **Host tests**: the fake kernel has a keyboard that can be typed at
  (`kf_type`); two groups cover the modes and the requests, poll and the
  gathered writev. **At boot**: the ring-3 self-test asks TCGETS and expects a
  canonical, echoing terminal (it used to expect ENOTTY); the shell's prompt
  (`shell_not_interactive`); and the word "echo" arriving as four writes of
  one character - the shell showing each key itself, the kernel's echo off
  (`terminal_raw_mode_failed`).
- **Sabotage**: `abi-tty.txt` (15), `abi-tty-fs.txt` (13), `abi-tty-pipe.txt`
  (2) on the host; `abi-tty-boot.txt` (2), `abi-tty-console-boot.txt` (1) and
  `user-sh-boot.txt` (1) at boot. Three host cases went NOT RED first, and
  each was the test's:
  - *poll waits with a timeout of 0.* The fake kernel abandons a call that
    waits, and an abandoned call leaves 0 behind - which is what poll returns
    when nothing is ready. The test compared the number. It asks now whether
    the call returned at all.
  - *poll reports reading to whoever asked about writing.* Nothing asked about
    writing on a descriptor that could be read.
  - *a read returns every finished line at once.* The terminal takes input
    only until one line is finished, so two never waited together - except
    after raw mode, where FIONREAD takes everything typed. That is the
    arrangement the test makes now.
- **Still open**: ISIG and TIME; TCSETSF throws away the terminal's own line
  and not what is still in the keyboard's queue behind it; there is one
  terminal and no /dev/tty to name it; a program that dies in raw mode leaves
  the next one in it, as on a real terminal, and nothing here resets it.

**Step 8 (2026-10-01): done, except LTP, which waits for L3.**

- **The corpus runs on VibeOS and is compared with Linux.**
  `tests/corpus/run-l1.sh` runs fifteen workloads - twelve of BusyBox's (the
  shell, ls, cp/mv/rm, find and grep, grep -r, tar, sort/sed/awk, sed -i,
  touch/chmod/ln/stat, mkdir/rmdir, a pipeline, mv), SQLite on a file and in
  memory, and a Lua script - and prints every line of every answer tagged with
  the workload's name. `scripts/dev/corpus-expect.sh` runs the same script
  under the same BusyBox on Linux and keeps what it printed
  (`tests/corpus/l1-expected.txt`, 100 lines). The boot gate stages the script
  on the boot volume, the self-test's shell runs it in `/tmp`, and the gate
  compares the two line for line: `corpus_answers_differ:<workloads>`,
  `corpus_did_not_finish`. All fifteen match.
- **The oracle is Linux.** Nobody wrote down what `ls -la` or `tar tf` should
  print; the commands are shaped so that what two machines may differ in - the
  date, the owner's name, a directory's size, the order it lists in - is not in
  the answer, and everything left has to be identical. The nightly re-runs the
  script on its own Linux first (`corpus-expect.sh --check`), so a BusyBox that
  starts answering differently is named before the boot is compared with a
  stale file.
- **SQLite and Lua** are staged when `corpus-build.sh` has built them, which a
  plain build does not do. Without them their workloads answer "absent", which
  the gate accepts and *says* (`corpus_absent=` on the verdict line); with
  `VIBEOS_SMOKE_CORPUS=require`, which the nightly's corpus job sets, absent is
  a failure. `sqlite3` makes a database, indexes it, vacuums it, and a second
  process reopens it and finds the first one's rows - record locks, pread and
  pwrite, fsync, truncate - with no syscall missing.
- **What running real programs found**, none of it in the syscalls this step
  set out to prove:
  - *`execve` of `/proc/self/exe`.* BusyBox's shell runs every applet not built
    into it by executing "the program I am" under the applet's name. That was
    refused as not found, the shell fell back to a file called after the applet
    on its PATH, and four such files had been staged - which is why `cat` and
    `ls` worked and `sort`, `sed`, `tar` and `awk` never could have. `readlink`
    has answered for that name since A4; `execve` does now.
  - *A forked child did not know what program it was.* Fork copied the image's
    entry point and nothing else of it, so the path it was started from - what
    `/proc/self/exe` answers - was the slot's last tenant's. With the fix above
    the shell's child executed "itself" and started the thread tests, which
    forked children that did the same: the boot did not end. The whole image
    is copied now.
  - *The user stack was sixteen kilobytes and did not grow.* BusyBox's `sed`
    has a function with an 18 KiB frame; every workload prints through `sed`,
    so every workload ended in a segmentation fault. 256 KiB, mapped at exec.
    Growth on demand is L3's.
  - *Files on FAT were not executable.* Reported 0644 where Linux reports a FAT
    volume's files 0755; `exec` never asked, and the script's `[ -x ]` did - so
    SQLite and Lua were on the volume, judged not runnable, and reported
    absent. This one was only visible because "absent" is printed in the
    verdict: the boot was green.
  - `/dev/null` does not exist (L2, with `/proc` and `/etc`); the script does
    not use it.
- **LTP is staged and run, and does not run yet.** `scripts/dev/ltp-run.sh`
  stages any of the 1,530 built tests on the boot volume, runs them after the
  workloads and reads each one's verdict out of the log. Every one is
  "broken" before its first check: LTP's harness keeps its results in a page
  shared between the test and the child that runs it, mapped from a file -
  `mmap(MAP_SHARED)` of a descriptor - and file-backed mappings are L3's (as
  is keeping a shared page shared across fork). None of the built tests uses
  LTP's older harness, which did not need it. So L1's 416 tests are an oracle
  that exists and cannot be consulted until L3; what proves L1 until then is
  the corpus above and the handlers' host tests. Recorded under L3, which now
  owes L1 that run.
- **Sabotage**: five cases at boot, all red. execve refusing
  `/proc/self/exe` is `corpus_did_not_finish` - the shell cannot start `sh`
  itself; a `rename` that reports success and does nothing is
  `corpus_answers_differ:bb-cp-mv-rm+bb-sed-inplace+bb-mv`, exactly the three
  workloads that move files; fork copying only the entry point is a boot that
  never ends; a four-page stack is 23 ring-3 faults; a self-test that does not
  run the script is `corpus_did_not_finish`.
- **One boot in eighteen failed, with M-070's signature** - the open
  swap-slot double release under the reclaim load - and for the first time
  with that finding's instrumentation in the log: init killed on a swapped-out
  entry whose slot was already free when its address space was torn down. Not
  this step's defect and not chased here; the evidence and what it narrows are
  in the tracker under M-070. What this step may have changed is how often:
  sixty more anonymous pages per process is sixty more for reclaim to take.
  Twelve boots of twelve were clean afterwards.

**L1 is closed** as far as it can be without L3: the registry stands at 124
done, 20 partial, 146 missing; 23 of the corpus's 25 workloads have nothing
missing, and fifteen of them are run and compared on every boot. Open, and
written where they belong: LTP (L3); `mknod` of a FIFO, `renameat2`'s EXCHANGE,
`openat2`'s RESOLVE flags, `ioctl`'s ISIG and TIME (L1 partials nothing has
asked for yet); a description that does not follow its file across a rename.

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

L2 was taken after L3, because L3's oracle said what it wanted most: of the
LTP tests for L1 that did not pass, most stopped at something of L2's - a
credential call, `alarm`, a file under /proc or /dev. Seven steps, in the order
that pays that debt first:

1. **Credentials**: who a process is, the calls that change it, and permission
   checks in L1's file operations.
2. **Signals completed**: a fault delivered to a handler that asked for it
   (SIGSEGV, SIGBUS, SIGFPE, SIGILL) with a real `siginfo_t`, `sigaltstack`,
   `rt_sigsuspend`, `rt_sigpending`, `rt_sigtimedwait`, the queueing calls,
   `pause`, and SIGCHLD raised when a child ends.
3. **Timers**: `alarm`, `setitimer`, POSIX timers, `clock_getres`,
   `gettimeofday`, `times` - which is also LTP's own timeout. Setting the clock
   stays EPERM.
4. **Limits and usage**: `getrlimit`/`setrlimit`/`prlimit64` enforced,
   `getrusage`, priorities, `personality`.
5. **Processes**: `waitid`, `getpgid`, `execveat`, `clone3`, pidfds.
6. **/proc and /dev**: `/proc/self/*`, `/proc/<pid>`, `/proc/cpuinfo`,
   `/proc/mounts`; `/dev/null`, `/dev/zero`, `/dev/urandom`, `/dev/tty`.
7. **The programs**: the corpus's shell workloads with `trap`, `timeout`,
   `ps`, `time`; LTP for L2's own calls.

(`nanosleep` and `clock_nanosleep` were done in L3 step 3, which could not run
its oracle without them.)

**Step 1 (2026-10-02): done.**

- **A process is somebody.** Real, effective, saved and filesystem ids for
  user and group, and thirty-two supplementary groups, in the process's state
  (`include/vibeos/cred.h`); all zero - root - for a process nobody changed, a
  copy for a forked child, kept across exec. The rules for changing them are
  Linux's without capabilities ("privileged" is effective user 0) and live in
  one portable file of pure functions, `kernel/sched/cred.c`.
- **Sixteen calls**: `getuid`, `geteuid`, `getgid`, `getegid` report them (they
  answered 0); `setuid`, `setgid`, `setreuid`, `setregid`, `setresuid`,
  `setresgid`, `getresuid`, `getresgid`, `setfsuid`, `setfsgid`, `getgroups`,
  `setgroups`. `setuid(1000)` used to be EPERM: there was one identity.
- **The file operations ask who is asking.** The walk needs search permission
  on every directory it looks a name up in; `open` needs read or write by its
  flags (and write to truncate - asked before anything is cut); a name is made
  or removed only in a directory the caller may write, and in a sticky one
  (`/tmp`) only by the file's owner or the directory's; `chmod` and a chosen
  time are the owner's, `chown` the superuser's except for the owner's own
  groups; `truncate`, `chdir` and `execve` ask for write, search and execute.
  What a process makes is its own. `access` answers for the *real* user and
  `AT_EACCESS` for the effective one. The superuser is refused nothing but
  running a file with no execute bit at all - and for it nothing more is
  looked up than before.
- **Signals**: a process may signal another only if it is the superuser's or
  the target's real or saved user is its real or effective one - on top of the
  session rule already there.
- **Not done, and said**: capabilities; the set-user-id bit on exec; a new
  file taking its directory's group under a set-group-id directory; `setgroups`
  with more than thirty-two groups (the registry's gap); FAT has no owners, so
  everything on the boot volume is root's and nobody else may write there.
- **At boot** the self-test's child gives up root for good and is refused
  root's file, root's directory and root's name in /tmp, owns what it makes,
  and cannot take root back (`credentials_selftest_failed`).
- **Sabotage**: `sched-cred.txt` (8), `abi-cred.txt` (8),
  `abi-cred-names.txt` (6) against the host tests, `abi-cred-boot.txt` and
  `abi-cred-exec.txt` against the boot; all red. One went NOT RED first - "the owner's bits do not decide
  for the owner" - because no test had an owner whose own bits were the
  stricter ones; there is one now. And the boot's own case went NOT RED:
  the child was refused *creating* a file in root's directory, which the
  directory's write bit refuses with or without the walk's search check; it
  reads a file anybody may read there now.
- **Found by a failed boot, not by a test**: exec built the new process state
  without the credentials, so a program was whoever had held the slot before -
  one boot in six, root's shell was the self-test's user 1000 and every create
  on the boot volume was "Permission denied". exec copies them, a new slot
  starts as root, and the self-test's child now runs BusyBox `test -r` on root's
  file and must see it refused (`abi-cred-exec.txt`, red). Nine boots of nine after the fix.
- **LTP, L1's 382 again**: 154 passed (131 before it), 16 failed, 144 broken, 65 not applicable, 2 did not run. What is left is mostly a loop device, `alarm`, and files under /proc and /dev.

**Step 2 (2026-10-03): done.**

- **The frame is Linux's.** A handler is entered with the signal, a
  `siginfo_t` and a `ucontext` - the interrupted registers by Linux's names,
  the mask to return to, the alternate stack - and the vector registers in an
  FXSAVE area that `fpstate` points at. `rt_sigreturn` reads the frame back as
  the program left it, so a handler that moves the saved rip moves where the
  program resumes; the architecture still decides what of it may be trusted
  (`ks_regs_set`: selectors and privileged flags forced, a resume address
  outside user memory refused, H-018). Until this step the frame was private
  to the kernel - a magic, the mask, the raw trap frame - which served a
  handler that only returns, and nothing that asks why or where. The
  registers cross the boundary by name (`vibeos_uregs_t`), the frame's layout
  is the Linux layer's, and the layouts are compared with the host's headers.
- **Why a signal came** travels with it (`include/vibeos/siginfo.h`): who sent
  it, which fault, how a child ended - facts, which the Linux layer puts into
  `si_code` and the rest. Raising and taking are one critical section each
  (`ks_signal_send`, `ks_signal_take`), under a lock per task with interrupts
  off. A blocked signal is kept even when ignored, as Linux keeps it, so
  `sigtimedwait` can take it.
- **A fault goes to a handler that asked for it.** SIGSEGV, SIGBUS, SIGFPE and
  SIGILL with `si_addr` and the right `si_code`, the trap number, error code and
  cr2 in the frame; blocked or ignored, it kills as before. Nothing about a
  fault the program takes is printed as a trap: `[SIG] fault taken by the
  program's handler`. The null page of a Linux program is present - the
  kernel's identity map lies under the low window - so the CPU says
  "protection" where Linux says "nothing mapped"; the program is told whether
  *it* had a mapping.
- **Calls**: `sigaltstack` (with `SS_AUTODISARM`), `rt_sigpending`,
  `rt_sigsuspend` (the handler returns to the program's own mask, not the
  temporary one), `pause`, `rt_sigtimedwait`, `rt_sigqueueinfo`,
  `rt_tgsigqueueinfo`; `SA_ONSTACK`, `SA_NODEFER`, `SA_RESETHAND` honoured.
- **SIGCHLD** is raised when a child ends, with its pid, uid, status and how it
  ended. It never was.
- **Not done, and said**: a real-time signal queued twice is delivered once
  (the registry's gap on 129 and 297); signals sent to a process go to its
  leader rather than to any thread that does not block them; SIGCHLD on stop
  and continue, `SA_NOCLDWAIT`, and a parent that ignores SIGCHLD reaping its
  children automatically.
- **At boot** the musl program that already checked handlers now also recovers
  from a fault on its alternate stack by moving the saved rip, keeps a vector
  register across a handler that overwrites it, reads `sigqueue`'s value, takes
  signals with `sigtimedwait`, waits in `sigsuspend` and `pause`, and is told of
  its child's end by SIGCHLD (`signal_l2_checks_failed`).
- **Sabotage**: `abi-signals.txt` (10) and `abi-signal-calls.txt` (8) against
  the host tests; `abi-signals-boot.txt` (3), `abi-signals-fpu.txt` and
  `abi-signals-value.txt` against the boot; all red. One went NOT RED first -
  a fault the program blocks offered to it anyway - and correctly: delivery
  skips a blocked signal and the task is killed either way; what the check
  keeps from happening is a signal left pending in a task about to die, which
  the test now asks about. Nine boots of nine after it.
- **LTP**: not measured in this step. The first test of the signal list, pause01, waits for its child to show as sleeping in /proc/<pid>/stat (step 6), and LTP's own timeout is setitimer (step 3), so the first test that waits forever takes the rest of its boot: 3 of 48 reported (kill08 passed; kill05 needs System V shared memory; kill06 failed with ESRCH killing a process group, not yet looked into). Measured again with step 3, when the timeout works.

**Step 3 (2026-10-03): done.**

- **Timers that belong to a process** (`kernel/sched/ptimer.c`, portable,
  host-tested): one table for the machine with a lock of its own, armed by the
  process's calls on any core, counted down by every core's tick and fired by
  the clock owner's. The callback that raises the signal runs outside the
  table's lock, because it takes the scheduler's; and a timer whose process is
  gone is counted (`ptimer_orphan`, must be zero) rather than fired at whoever
  has the pid next. The exit path takes a process's timers with it; exec takes
  its POSIX timers and leaves `alarm`, as Linux does.
- **Calls**: `alarm`, `setitimer`, `getitimer` (real, virtual and profiling);
  `timer_create`, `timer_settime`, `timer_gettime`, `timer_getoverrun`,
  `timer_delete` on the machine's clocks and on the process's and a thread's
  CPU time, with `SIGEV_SIGNAL`, `SIGEV_NONE` and `SIGEV_THREAD_ID`;
  `clock_getres`, `gettimeofday`, `times`; `clock_gettime` on the CPU-time
  clocks. `clock_gettime` used to answer every clock number with uptime, so a
  program asking for its CPU time got the wall clock; an unknown clock is
  EINVAL now. Setting the clock is REFUSED (EPERM).
- **What the tick can tell**: a tick is the resolution, and `clock_getres`
  says so. CPU time is charged by the tick to whatever each core was running,
  as user time when it interrupted ring 3 - which is what `ITIMER_VIRTUAL`
  needs. A timer never fires early: it is armed a tick late, and that tick is
  not reported back.
- **Overruns** are Linux's: an expiry that finds its timer's signal still
  pending adds to that signal's count, which `si_overrun` and
  `timer_getoverrun` report; periods that went by with no tick to fire them
  count too.
- **What LTP found once its timeout worked**, none of it in the timer code:
  a thread's id did not name its process for `kill` and `rt_sigqueueinfo`; a
  signal to a process already exiting was EINVAL instead of 0; `tgkill` looked
  up ids that are not positive; `waitpid(INT_MIN)` was ECHILD, not ESRCH;
  SA_RESETHAND cleared SA_SIGINFO; `rt_sigaction` and `rt_sigprocmask`
  accepted a sigset of any size; and `getpgid` did not exist - musl's
  `getpgrp()` is `getpgid(0)`, so every musl program's process group was
  -ENOSYS (taken early from step 5). And the CPU time of a process in a
  recycled slot included everybody who had held the slot: the scheduler's
  accounting is per slot, so each task now records where it started
  (`cpu_base`). `signal06` signals itself thirty thousand times and the
  console line per signal ran its boot out of time; only the first 64 are
  printed now.
- **Not a kernel defect**: LTP's x86-64 `rt_sigaction` wrapper fetches its
  restorer from the C library's `sigaction` old action, and musl does not fill
  that field, so `rt_sigsuspend01` and `rt_sigaction01` return through an
  uninitialised pointer. Built against glibc they would not.
- **Not done, and said**: `times()` reports no system time, and a thread that
  has exited takes its CPU time with it (the registry's gap on 100); the CPU
  clocks of another process (negative clock ids); `SIGEV_THREAD` is the C
  library's and reaches the kernel as a signal to the process. Signal 64 has
  no bit in a 64-bit mask numbered by signal (`sighold02`, `sigrelse01`).
  `clock_settime` stays refused, and four LTP tests say root should be able
  to set the clock: an offset to the timer's uptime would do it, the day a
  program needs it.
- **At boot** the musl signal program waits in `pause` for `alarm(1)`, counts
  four expiries of a periodic `setitimer`, spins until `ITIMER_VIRTUAL` and
  `ITIMER_PROF` fire, takes a POSIX timer's signal with `sigtimedwait` and
  checks `SI_TIMER` and its value, waits for a timer on its own CPU time, uses
  `SIGEV_THREAD_ID`, and reads the clocks (`timers_did_not_fire`); a child that
  arms an alarm and exits leaves nothing behind (`ptimer_orphan`).
- **Sabotage**: `sched-ptimer.txt` (10), `abi-timers.txt` (9) and
  `abi-cpu-base.txt` against the host tests, with four more in
  `abi-signal-calls.txt` and one in `abi-signals.txt` for what LTP found; `abi-timers-boot.txt` (2) and `abi-timers-tick.txt` against the
  boot; all red. Two went NOT RED first, both tests right about the outcome and
  wrong about the arrangement: the wall clock and the process's CPU clock both
  read seven ticks, since the process had done nothing but run; and a bad flag
  was tried together with a time that was not one. Nine boots of nine after it.
- **LTP**: the timer tests and step 2's signal tests together (tests/corpus/ltp-l2.txt's subset, 70 tests): 37 passed, 1 failed (times03: no system time), 24 broken, 7 did not run (kill10 takes the rest of its boot: step 5). Before this step the same list ran 3 of 48. Of the broken: /proc/<pid>/stat and /proc/cpuinfo (step 6), setrlimit (step 4), clock_settime refused, System V shared memory, signal 64, and LTP's musl restorer.

**Step 4 (2026-10-03): done.**

- **Limits** (`kernel/abi/linux/limits.c`): Linux's sixteen, soft and hard,
  in the process state - a child has its parent's, exec keeps them, and the
  first process starts with Linux's defaults where this machine can honour
  them (`linux_procstate_defaults`, called by the architecture's
  `hw_procstate_new`: the third writer every field of the process state has).
  `getrlimit`, `setrlimit` and `prlimit64` (on another process too, for the
  superuser or a caller whose ids match) with Linux's rules: soft above hard is
  EINVAL, raising a hard limit is the superuser's, NOFILE stops at what the
  descriptor table holds.
- **Enforced** where the mechanism already was: NOFILE is the descriptor
  table's own limit; FSIZE cuts a write at the limit in the one place every
  regular-file write passes, and past it is SIGXFSZ and EFBIG; DATA stops `brk`;
  NPROC refuses fork and clone for a user who is not root; CPU is two timers in
  the process-timer table (step 3) on the process's CPU time - SIGXCPU at the
  soft limit and every second after, SIGKILL at the hard one. The rest are kept
  and reported, and the registry says so.
- **`getrusage`** reports CPU time (all of it user time); **`getpriority` and
  `setpriority`** are the scheduler's nice, answering 20 - nice raw as Linux
  does, with Linux's rules for who may renice whom and RLIMIT_NICE's allowance;
  **`personality`** is kept and reported, and changes nothing.
- **Found on the way, and not in this step's code: the scheduler's policy had
  never run on the machine (M-084).** `kernel/sched/sched_policy.c` - classes,
  nice-weighted fairness, affinity - was written on 2026-09-02, host-tested and
  tortured, and the architecture admitted every task into it and asked it to
  pick on every tick. It never initialised it. Every admission into a table of
  no slots was refused, every refusal was ignored, the picker returned nothing
  and the machine fell back to round robin; nor was the policy ever charged the
  virtual time it picks by, so initialising it alone would have picked the
  lowest slot for ever. setpriority returned 0 and changed nothing, which is
  how it was found. The architecture initialises it and charges it on every
  tick now; a refused admission is `sched_admit_refused`, must be zero, and the
  gate asserts the policy was charged (`sched_policy_never_charged`).
- **Not done, and said**: RSS, AS, MEMLOCK, STACK and the rest are kept, not
  enforced (RLIMIT_AS is the registry's gap on 160 and 302); `getrusage` has no
  resident-set high-water mark (98); no personality flag changes behaviour
  (135).
- **At boot** SIGNAL.ELF runs into NOFILE, FSIZE (dies of SIGXFSZ, and with it
  ignored gets EFBIG) and CPU (SIGXCPU after a second of running), and checks
  priorities, `getrusage` and `personality` (`limits_not_enforced`).
- **Sabotage**: `abi-limits.txt` (12), `abi-limits-paths.txt` (2),
  `abi-limits-mm.txt` and `abi-limits-fork.txt` (3) against the host tests,
  `abi-limits-boot.txt` and `sched-policy-live.txt` (2) against the boot; all
  red. Eighteen boots of eighteen after it - double the usual, because the scheduler's policy going live changes every program's scheduling.
- **LTP**: the limit tests (22): 17 passed; the rest need 512 MB free (getrusage03), /proc/cpuinfo (getrusage04, step 6), sched_getaffinity (nice05, L6), select (personality02), /bin/true on the volume (setrlimit04), and useradd (setpriority01, which then waits out its timeout - not looked into). With step 3's 70 alongside: 54 of 94 passed.

**Step 5 (2026-10-03): done.**

- **One way to wait.** wait4 and waitid share one engine, which selects every
  child, one by pid, or a process group's. wait4 used to hear only "any" and
  "this pid": `waitpid(0)` and `waitpid(-pgid)` - a shell's job control - were
  matched as pids and answered ECHILD. wait4 fills its rusage now. `waitid`
  reports what ended as a siginfo (`CLD_EXITED` or `CLD_KILLED`, the pid, the
  user it was when it ended, the status), honours `WNOWAIT` - look without
  reaping - and `WNOHANG` with a record of zeroes, and takes a pidfd as
  `P_PIDFD`.
- **Pidfds** (`kernel/abi/files/pidfd.c`): a description that names a
  process by its pid and the tenancy of its slot, so that a pid reused after
  the reap is not mistaken for it - the reason a program holds one. Readable
  once the process has ended. `pidfd_open` (a process, not a thread; always
  close-on-exec) and `pidfd_send_signal` (kill, or sigqueueinfo with a
  siginfo; ESRCH once the process is reaped, whoever has the number now). It
  is what a Windows process handle is, which is why it lives with the file
  types and not in the Linux layer.
- **`clone3`** is clone's two paths with the arguments in a structure - a
  thread, or a fork - plus `CLONE_PIDFD`; **`execveat`** runs a program
  relative to a directory descriptor, or the file a descriptor names with
  `AT_EMPTY_PATH`, and refuses a link under `AT_SYMLINK_NOFOLLOW`.
- **Not done, and said**: stopped and continued children are never reported
  (waitid's gap); `clone3`, like `clone`, makes no process that shares its
  parent's memory and takes no chosen pid or cgroup (its gap).
- **At boot** SIGNAL.ELF looks at a child with `WNOWAIT` and then reaps it,
  waits for a group, signals a child through a pidfd, sees the pidfd become
  readable and waits for it with `P_PIDFD`, runs BusyBox through `execveat`
  on a descriptor, and forks with `clone3` and `CLONE_PIDFD`
  (`process_calls_failed`).
- **Sabotage**: `abi-processes.txt` (6), `abi-pidfd.txt` and
  `abi-pidfd-calls.txt` (3) against the host tests, `abi-processes-boot.txt`
  against the boot; all red. Six boots after it: 6 of 6 clean.
- **LTP**: the step's own 41 (`build-gcc-Release/ltp-l2s5.txt`): 12 passed, 2 failed, 26 broken, 1 not applicable. It found two defects in this step's clone3 - the exit signal a caller names is ignored and SIGCHLD sent instead (clone301), and five of the argument checks Linux makes are missing (clone302) - and two in older layers that decide most of the rest. Eight tests waited out their timeout; waitid04, the one read, passed its checks and then hung in LTP's checkpoint, a futex shared between processes through a MAP_SHARED page, which the futex table keys per process. And an orphan is never given to init here, so the children of a test killed by its timeout stay zombies nobody reaps: the task table filled, and every fork after that was EAGAIN (seven tests). All three are fixed in the next commits. The rest want /proc (step 6), unshare, cgroups, a loop device, core dumps, ns_last_pid, or LTP resources that are not staged (execveat01, execveat02).

**Step 6 (2026-10-04): done.**

- **/proc is about processes** (`kernel/fs/procfs.c`, rewritten). `self` is a
  link to the asking process's directory, and each process has one: `stat`
  (Linux's 52 fields), `statm`, `status`, `cmdline`, `comm`, `maps`, `mounts`,
  the links `exe`, `cwd` and `root`, and `fd/`, a link per descriptor to what
  it was opened as. The machine has `cpuinfo` (CPUID under Linux's names, a
  clock measured from the TSC, the brand that tells LTP it is virtualised),
  `mounts`, `version`, `uptime` and `loadavg`. The filesystem formats and keeps
  nothing; what it says about processes it asks of a source in the Linux layer
  (`kernel/abi/linux/procsrc.c`), which copies it out under the locks that hold
  it. A file is generated straight into the reader's buffer at any offset, never
  whole on a kernel stack.
- **/proc/self/exe is a walk.** execve and readlink recognised the name by its
  spelling, because there was no /proc to hold it; both special cases are gone,
  deleted rather than generalised.
- **A process that waits shows S.** Every wait here keeps its task runnable, so
  the task's state could not say; a wait's block point marks it and the
  dispatcher clears the mark when the call returns. LTP's harness waits to see
  exactly that before it signals a child that pauses.
- **/dev** (`kernel/fs/devfs.c`): `null`, `zero`, `full`, `random`, `urandom`,
  `tty`, `console`, and the links `fd`, `stdin`, `stdout`, `stderr`. A node is a
  type and a number; what it opens as is decided by the number in the file
  types (`kernel/abi/files/chrdev.c`), and `/dev/tty` is the console's own
  description, which answers as a terminal. stat reports the device number.
- **Random numbers** (`kernel/core/random.c`): a ChaCha20 key everything is
  mixed into and every read replaces, tested against RFC 8439's vector. Fed by
  RDRAND when the processor has it and by every core's timer interrupt -
  ready, under QEMU's TCG processor which has no RDRAND, within a second of the
  timer starting. `getrandom` waits until then (EAGAIN with GRND_NONBLOCK,
  what there is with GRND_INSECURE); it answered ENOSYS before, under a
  registry row that said done. `/dev/random` waits, `/dev/urandom` does not.
- **What step 5's LTP run found, fixed.** A futex word on a MAP_SHARED page is
  the page's, the same word in every process that maps it - LTP's checkpoints -
  and FUTEX_WAIT's timeout is honoured; both had been missing since L3 made
  shared mappings, under a comment that said there were none. A process's
  children go to init when the last of its tasks ends: none ever did, so every
  child of a killed test stayed a zombie holding its slot. clone and clone3 send
  the exit signal they were given, and clone3 checks what Linux checks. And exit
  clears a task's pointer to its process state under the scheduler's lock before
  giving its reference back, so whoever finds the state under that lock - /proc
  now, prlimit before it - can hold it.
- **At boot** SIGNAL.ELF reads its own /proc files, sees a paused child as S,
  uses /dev/null, zero, full and urandom and getrandom, waits on a futex in a
  shared file page from another process, is sent SIGUSR2 by a child cloned with
  it, and sees an orphan adopted (`proc_dev_checks_failed`).
- **Sabotage**: `core-random.txt` (4), `fs-procfs.txt` (6), `fs-devfs.txt`, `abi-chrdev.txt`, `abi-futex-shared.txt` and `abi-clone3-checks.txt` (3 each) against the host tests, `abi-procdev-boot.txt` against the boot; all red. The pool's key not being replaced after a read is not a case, and the case file says why: nothing a caller can see changes. Six boots after it: 6 of 6 clean.
- **LTP**: the step's own tests with step 5's again (`build-gcc-Release/ltp-l2s6.txt`, 79): 41 passed, 9 failed, 21 broken, 4 not applicable, 4 did not run (kill10 still takes the rest of its boot). Step 5's 41 among them: 23 passed, 12 before. What it found, for step 7: a process group left orphaned with stopped members is never sent SIGHUP and SIGCONT, as POSIX says it must be - waitpid13's child moves half its children into a group of its own, they stop themselves, LTP's kill of the test's group does not reach them, and they hold their slots until a fork is EAGAIN (pause01); `/proc/<pid>/task`, which futex_wait03 reads to see its thread asleep and waits out its timeout without; a `/proc/<pid>` directory used as a pidfd (pidfd_send_signal01, 02); /dev/random's ioctls (ioctl07); and one clone3 check more (clone302's invalid pidfd). Capacity, not defects: waitpid03 forks 25 children and futex_wake02 clones more threads than a process may have, on a table of 32 slots. The rest want stopped and continued children reported (waitid07, waitid08, waitpid08, waitpid13), core dumps, /proc/sys, a loop device, mknod of a device, unshare and cgroups.

### L3. Memory (10, plus `mmap` finished)

File-backed mappings - private and shared - through the page cache, `MAP_FIXED`,
`mremap`, `madvise`, `msync`, `mincore`, the `mlock` family, `memfd_create`.
This is the phase that makes **glibc** possible: its dynamic loader maps
libraries with `MAP_FIXED`.

A user stack that grows: a fault below it maps a page (step 5, done second).

**Programs:** the corpus rebuilt against glibc, dynamically linked; `sqlite3`
with memory-mapped I/O; `lua`. **And LTP, for every phase before this one**:
its harness maps a file `MAP_SHARED` for its results and needs that page to
stay shared across fork, so not one of its tests can start until this phase.
L1's 416 are the first owed (`scripts/dev/ltp-run.sh`).

Six steps, in the order that pays L1's debt first:

1. **mmap by the rules**: the flags read as Linux reads them (one of private
   and shared, required), `MAP_FIXED` and `MAP_FIXED_NOREPLACE`, an address
   hint honoured when it is free, the arena stepping over what a fixed mapping
   took, and **private file mappings** - the file's bytes in the process's own
   pages.
2. **Shared mappings**: `MAP_SHARED`, anonymous and of a file whose filesystem
   can hand out its pages (tmpfs), staying shared across fork; `msync`. This is
   what LTP's harness needs.
3. **LTP, for L1**: its 416 tests run through `scripts/dev/ltp-run.sh`, and what
   they find fixed or written down.
4. **The rest of the calls**: `madvise`, `mincore`, the `mlock` family,
   `memfd_create`, `mremap`.
5. **A stack that grows**, and the 256 KiB mapped at exec given back.
6. **The programs**: the corpus against glibc, dynamically linked; `sqlite3`
   with memory-mapped I/O; LTP's own tests for these calls.

**Step 1 (2026-10-01): done.**

- **mmap places a mapping where the program says.** `MAP_FIXED` maps at the
  address given and takes the place of whatever was there - how a dynamic
  loader lays a library's segments over the range it reserved - and
  `MAP_FIXED_NOREPLACE` refuses if anything was (EEXIST). An address hint is
  honoured when the range is free. The arena is still a cursor that moves up,
  but it steps over what is already mapped: a fixed mapping can be ahead of it
  now, and it used to hand out the same pages. Where a program may put a
  mapping is the architecture's to say (`ks_user_fixed_ok`: the high user
  window); an address outside it is ENOMEM.
- **The flags are read as Linux reads them**: exactly one of private and
  shared, or EINVAL; `MAP_ANONYMOUS` ignores the descriptor (it used to refuse
  one that was not -1).
- **A private mapping of a file** is the file's bytes in pages of the
  process's own, read when the mapping is made, from any filesystem - the page
  is mapped writable, filled through the file's own read, and given the
  protection asked for. A store stays in the process; the mapping outlives the
  descriptor. Past the end of the file it reads zeros, where Linux raises
  SIGBUS beyond the file's last page. EACCES for a descriptor that cannot be
  read, ENODEV for what is not a file.
- **Still refused, and said**: a shared mapping of a file (ENOSYS); a shared
  anonymous one is made and is private after a fork. Both are step 2, and the
  registry's `mmap` line names them.
- **The fake kernel has page tables.** The memory handlers could not run in
  the host tests - the fake failed every mapping and said why. It now runs
  `kernel/mm`'s frame layer and address-space layer over a few megabytes, as
  their own tests set them up; a mapped address is read and written through
  the tables (`kf_peek`, `kf_poke`), a kernel-mode store takes the
  copy-on-write fault, and fork clones the space. So mmap, munmap, mprotect and
  brk are host-tested as handlers for the first time, fork's copy-on-write
  included.
- **At boot** the ring-3 self-test maps over a page at a fixed address and
  reads zeros where it had written, is refused by NOREPLACE, and maps its own
  image off the FAT volume privately: the ELF magic, and a store that sticks.
- **Sabotage**: `abi-mmap.txt` (18) on the host, `abi-mmap-boot.txt` (1). Three
  went NOT RED first; two were the test's - a pattern that repeated every 256
  bytes, so the byte at offset 4096 was the byte at 0 - and one was the code's:
  the handler's unmap before a fixed mapping is not what releases the old
  pages (the address-space layer does that), it is what lets the region list
  describe the new mapping.
- **A race in fork, found while reading for M-070, and fixed.** For a page
  that needs no conversion - read-only, or copy-on-write already - fork reads
  the entry and, some instructions later, maps the frame into the child. A
  page-out marks only writable pages, so it can evict that page and release
  the frame in between, with nothing in the entry either side would notice:
  the child is mapped onto a free frame, which the allocator hands to somebody
  else. `clone_one` now pins the frame only if it still has an owner and reads
  the entry again before mapping. A host test puts the page-out exactly there
  through the fork hook, with the frame left free and with it reused; before
  the fix it found the child on a frame the page-out had given away.
  `fork_frame_gone` and `fork_entry_moved` count the two cases on every boot.
- **M-070 itself is not fixed, and is more frequent than it was.** The swap
  map's double release - not seen in ninety boots before L1 step 8 - has been
  seen three times in about sixty since that step gave every process sixty
  cold stack pages, the third time *with* the fix above in place and its
  counters at zero: so that race, real as it is, is not what these boots hit.
  What the three captures agree on: the reclaim load is running; a process is
  killed reading a page whose entry is a valid swapped one (read-only, user);
  its slot is found already free when the address space is torn down, last
  given back by a page-in. What they do not say is whose page-in, or why the
  one that killed the process failed. The kernel says both now -
  `SWAP_IN_FAILED` with the reason and the slot's history, and each slot's
  last writer and last releaser by process - and sixteen boots after adding
  that were clean, so the next capture is still owed. **The boot gate is
  therefore flaky, at something like one boot in twenty**, and it was this
  phase's predecessor that made it so. Step 5 (a stack that grows, no pages
  mapped that nobody touched) removes what raised the rate; it does not
  explain the defect.

**Step 2 (2026-10-01): done.**

- **A shared mapping stays shared.** `MAP_SHARED` marks the page
  (`VIBEOS_PROT_SHARED`, bit 53 of the entry), and a fork maps the same frame
  into the child with the access the parent has, instead of making both sides
  copy-on-write. A store by either is seen by both, before the fork or after.
  mprotect changes the access and leaves the kind; a read-only shared page
  refuses a store rather than copying.
- **A shared mapping of a file is the file.** The filesystem hands out its own
  page (`share_page`, a new optional operation; tmpfs has it) and that page is
  what is mapped: a store through the mapping is read back by `read`, a `write`
  is seen through the mapping, two mappings of one file are one page. A hole is
  given a page. The page comes held - the reference is taken inside the
  filesystem's lock, where it can still vouch for it - and each mapping holds
  it too, so a file truncated or unlinked under a mapping leaves the mapping
  its page, and the page is freed when the last holder lets go.
- **Shared and writable needs a descriptor that can write** (EACCES); shared
  and read-only does not, and neither does private and writable.
- **`msync`** validates and returns: there is no second copy to write back.
  EINVAL for an address that is not a page's or flags that make no sense,
  ENOMEM for a range with a hole in it.
- **A shared page is not paged out.** An entry in swap has nowhere to say
  "shared", and a page that came back without the mark would be private from
  then on (`swap_refused_shared`).
- **What is not done, and said**: a shared mapping of a file on a filesystem
  that keeps no pages - FAT - is ENODEV, and is the registry's `mmap` gap. Only
  the pages holding some of the file are mapped; one past the end is absent,
  and touching it is SIGSEGV where Linux says SIGBUS. A mapping does not follow
  its file: grown after it was mapped, the new pages are not in the mapping;
  cut and grown again, the mapping still has the old page. mprotect will make
  writable a shared mapping made from a read-only descriptor.
- **The layering check gained one exception, written into it**: the hold on a
  file's page is a frame reference taken outside `kernel/mm`, by
  `hw_tmpfs_page_hold` and nothing else.
- **At boot** the self-test maps two anonymous pages and a page of a file on
  /tmp shared, forks, and the child stores into each: the parent has to see the
  stores and `pread` has to return the file's (`shared_mapping_selftest_failed`);
  and the fork has to have handed shared pages on (`fork_never_kept_a_shared_page`).
- **Sabotage**: `abi-mmap-shared.txt` (5), `mm-shared.txt` (4),
  `fs-tmpfs-share.txt` (3) against the host tests, `arch-shared-boot.txt` and
  `arch-shared-leaf-boot.txt` against the boot; all red. 24 boots of 24 clean.
- **LTP**: its harness maps its results page and gets further - every test now stops one step later, at `tst_memutils.c:94: Failed to open FILE '/proc/meminfo'` (eight tests tried). That is step 3's first item, not a mapping's.

**Step 3 (2026-10-02): done - the oracle runs, and what it said.**

`scripts/dev/ltp-run.sh build-gcc-Release --list tests/corpus/ltp-l1.txt` runs
the 382 LTP tests for the syscalls L1 owned (`scripts/dev/ltp-list.py`; the
"416" above was counted against an earlier registry), thirty-two to a boot,
and leaves each verdict in `<build>/ltp-results.txt`. The first run, before
anything below was fixed: 113 passed. The last: 131 passed, 10 failed, 177 broken, 57 not applicable, 7 did not run.

**What had to exist before a test could start.** None of it is a mapping's:

- **`/proc`**, a filesystem (`kernel/fs/procfs.c`) with what the harness reads:
  `/proc/meminfo` and `/proc/sys/kernel/pid_max`. Generated on read, no state.
- **`/etc/passwd` and `/etc/group`** on the boot volume: a C library answers
  `getpwnam("nobody")` out of them.
- **`nanosleep` and `clock_nanosleep`** (L2's, taken early). They did not
  exist: every `sleep` and `usleep` in every program had been returning at
  once, and nothing checks what they return.
- **A call a signal cut short is run again** when the handler was installed
  with `SA_RESTART`, or when no handler ran. Every interrupted wait had been
  EINTR, so a parent whose child signals it - LTP's harness - saw `waitpid`
  fail. A handler returns `VIBEOS_RESTART_CALL` where a wait can simply be
  started again (waiting for a child, a terminal, a pipe, a lock); the
  dispatcher turns it into EINTR and remembers the call; the delivery decides.
- **More than thirty-two programs in a boot** (M-081, below).

**What the tests found in L1's own calls, fixed:**

- `F_GETLK` named whichever conflicting lock the table held first; it names
  the one that starts first (fcntl11, fcntl21).
- A path's first byte was read without asking whose memory it was (**M-082**,
  statx03): ring 0 reads a page that is mapped for nobody, so `statx` of a
  path in a `PROT_NONE` page, or in the kernel, answered ENOENT or EFAULT by
  the byte it found there.
- An empty path was looked up from the directory descriptor: ENOTDIR or EBADF
  where Linux says ENOENT (fchmodat02).
- `preadv`/`pwritev` with a negative length were EFAULT, not EINVAL; reading a
  directory at an offset was ESPIPE, not EISDIR (preadv02, pwritev02).
- `fallocate` with `FALLOC_FL_KEEP_SIZE` did not look at how far it reached:
  past the largest offset is EFBIG (fallocate02).
- A process that opened files until refused was refused by the machine (ENFILE
  at 256) and not by its own limit (EMFILE at 1024): the description pool is
  2048 (creat05, fcntl12).
- `socket()` of a family there is none of was EINVAL, which a C library takes
  for a failure where EAFNOSUPPORT means "no name-service daemon" - so
  `getgrgid` of an unknown group failed (seven tests).

**What they found that is written down and not fixed:**

- **Credentials are L2's**: `setuid`, `seteuid`, `setgid`, `setegid`,
  `setreuid` and `getpgid` are missing or refuse, so every test that becomes
  "nobody" stops in its setup, and the ones that check a permission is
  *refused* cannot (symlink03). The largest group after the next.
- **No block device to borrow**: about ninety tests ask LTP for a loop device
  (`Failed to acquire device`) to make a filesystem on. Not a syscall's fault.
- **`alarm` and `setitimer` are L2's**, and LTP's own timeout is `alarm()`: a
  test that hangs is not stopped, and takes the rest of its boot (flock03 and
  the tests after it in that boot `did not run`).
- `/dev/null`, `/dev/urandom`, a pty, `/proc/mounts`, `/proc/self/maps`,
  `/proc/cpuinfo`, `/proc/version` (L2); `socketpair` (L5); `pthread_create`
  refused under OFD-lock tests (L6).
- Leases (`F_SETLEASE`), `O_PATH` descriptors answering EBADF to I/O, FIFOs
  from `mknod`/`mkfifo`, `RENAME_EXCHANGE`: gaps in L1's own rows, the last two
  already in the registry.
- xattr tests report TCONF, correctly: no filesystem here keeps them.

- **Sabotage**: `abi-ltp-l1.txt` (6), `abi-restart.txt` (4),
  `abi-sleep-restart.txt` (4), `fs-procfs.txt` (3), one more in
  `fs-filelock.txt`; all red. Two went NOT RED first, and the reason was the
  fake kernel: its `vibeos_uaccess_copy` refused a page not mapped for the
  user, which the machine's does not - so the test for M-082 could not fail
  there. The fake's kernel copy now reaches what ring 0 reaches, and the
  self-test checks the same thing on the machine. `check.sh all` green, the sanitized host tests clean, 20 boots of 20 over two runs.

**Step 4 (2026-10-02): done.**

- **`madvise`.** Advice about speed is taken by doing nothing: every page is
  already there. `MADV_DONTNEED` and `MADV_FREE` are not advice - a program
  reads zeros afterwards and an allocator relies on it - so a private anonymous
  page is given back and a zeroed one put in its place. A shared page is left
  as it is; a locked one refuses. Advice that would change what a later call
  does (`MADV_DONTFORK`, the huge-page pair, `MADV_REMOVE`) is EINVAL rather
  than a promise not kept. **Gap, in the registry**: `MADV_DONTNEED` on a
  private page of a file keeps what the program stored, where Linux shows the
  file again.
- **`mincore`**: a byte a page - in memory, or not (in swap, or a part of a
  mapping nothing backs).
- **`mlock`, `mlock2`, `munlock`, `mlockall`, `munlockall`.** A locked page
  carries a mark in its entry (`VIBEOS_PTE_LOCKED`, bit 54) and page-out
  refuses it; a page in swap is brought back when it is locked. The copy a
  write fault makes, a change of protection and a move keep the mark; a fork
  does not hand it to the child. **Gap**: `MCL_FUTURE` is accepted and a
  mapping made afterwards is not locked.
- **`mremap`.** Shrinks; grows where it stands when the address space after it
  is free; moves when it is not and `MREMAP_MAYMOVE` allows, or to the address
  `MREMAP_FIXED` names. A move takes each entry out of the old address and puts
  it at the new one - the same frame, the same marks, the same reference;
  nothing is copied (`vibeos_vmspace_move`). A failure half-way puts back what
  was moved. **Gap**: only a private anonymous mapping grows - a shared one or
  a file's would need more of what it shares, and the region does not remember
  where that came from; a zero old length and `MREMAP_DONTUNMAP` are refused.
- **`memfd_create`.** A file on /tmp that goes when its last descriptor does:
  memory with a file's interface, to size, map shared and pass on. **Gap**: it
  has a name under /tmp while it is open, where Linux's has none anywhere; and
  there are no seals.
- **A region says what it is** - anonymous, a file's bytes in private pages,
  or shared - because three of these calls have to ask.
- **At boot** the self-test discards a page and reads zeros, locks two pages
  and asks mincore, grows a mapping that has to move and finds its contents,
  and shares a memfd across a fork (`memory_calls_selftest_failed`).
- **Sabotage**: `abi-mem4.txt` (14), `mm-lock-move.txt` (4), `abi-memfd.txt`
  (3) against the host tests, `abi-mem4-boot.txt` against the boot; all red.
  `check.sh all` green, the sanitized host tests clean, 21 boots of 21 (run three at a time).
- **LTP**, the tests for these calls and for mmap and msync: 32 tried, 20 pass. `mmap08` found one more wrong answer, fixed (a mapping of a descriptor that is not open is EBADF before a zero length is EINVAL). Of the rest: `mincore03` expects an untouched anonymous page not to be in memory, and here a mapping is populated when it is made; `mremap01` grows a shared mapping of a file (the gap above); the others want `setrlimit`, `/proc/self/maps`, `shmget`, seals or `mount`.

**Step 6 (2026-10-02): done. L3 is closed, with its gaps named.**

- **glibc, dynamically linked, runs.** SQLite's shell and Lua are built a
  second time with the host's compiler against the host's glibc
  (`scripts/dev/corpus-build.sh`: `sqlite3-glibc`, `lua-glibc`), and the
  loader and the libraries they name - `ld-linux-x86-64.so.2`, `libc.so.6`,
  `libm.so.6` - are copied to the boot volume at the paths the programs ask for
  them by. glibc's loader reserves a range and lays each library's segments
  over it with `MAP_FIXED`, private mappings of the file: step 1. Nothing had
  to be added for it; the first program ran on the first try.
- **SQLite with memory-mapped I/O, in write-ahead-log mode.** The WAL's index
  is a file every connection maps shared - the journal mode cannot be entered
  without step 2 - and with `mmap_size` set the database is read through a
  mapping. Two connections, four hundred rows, an update, a checkpoint and an
  integrity check; the `-wal` and `-shm` files are gone afterwards, as on
  Linux.
- **Three workloads more in the corpus** (`glibc-sqlite`, `glibc-lua`,
  `sqlite-wal-mmap`), eighteen in all, compared line for line with what Linux
  printed. They matched on the first boot.
- **Sabotage**: `abi-corpus-l3-boot.txt`, two cases, red - a `MAP_FIXED` that
  is not honoured takes the glibc workloads down, a shared file mapping made
  private takes the WAL one.
- **LTP for L3's own syscalls** (`tests/corpus/ltp-l3.txt`, 66 tests):
  31 of 63 pass, with two left out because they hang (`mincore04`, `mmap18`). It found three more wrong answers, fixed: `MAP_SHARED_VALIDATE` did not refuse an unknown flag, `MAP_LOCKED` was ignored, and `msync(MS_INVALIDATE)` of a locked page was not EBUSY. Not fixed and said: a fault in ring 3 kills the task instead of delivering SIGSEGV or SIGBUS to a handler that asked for it (`mmap05`, `mmap13`, and why `mmap18` hangs); `mincore03` expects an untouched page not to be resident; `mremap01` aborts after `mremap` refuses to grow a shared mapping of a file - most likely in its own cleanup, which was not confirmed. The rest want `setrlimit`, `/proc/self/maps` and `status`, `shmget`, seals, `mount` or a loop device.
- `check.sh all` green, the sanitized host tests clean, 12 boots of 12 three at a time and 5 of 5 through the full gate. A later run of six in parallel ran out of its 300 seconds with every boot still working through the corpus: the host was at 70% load from something else and everything on it ran at half speed.

**What L3 leaves open**, all in the registry or above: a region does not
remember its file, which is one cause with three faces (a shared or file
mapping does not grow under `mremap`, `MADV_DONTNEED` keeps a private page of
a file, a page past the end of a mapped file is absent rather than SIGBUS); a
shared mapping of a file needs a filesystem that keeps pages, so not FAT; a
mapping is populated when it is made, so `mincore` never says "not yet";
`MCL_FUTURE`; seals on a memfd.

**Step 5 (2026-10-01), done ahead of its turn.** Taken before step 2 because of
what step 1 found: the boot gate had become flaky, and the pages mapped at exec
that nobody touched were what had made it so.

- **The stack grows.** Four pages are mapped at exec, as before L1 step 8, and
  the two megabytes below the stack's top are one region the process has from
  the start. A touch of a page in it that nothing maps is given a zeroed page
  by the page fault (`hw_stack_grow`), whichever side the touch comes from -
  ring 3, or the kernel storing a syscall's result into a buffer on the stack.
  Any address in the region grows it, not only one near the stack pointer:
  that is what Linux has done since it dropped the stack-pointer test.
- **The region list is asked**, not only the address. A program that unmapped
  part of its stack, or took its write permission away, is not given a page
  there. The region's lowest page is never mapped and below it is nothing of
  the stack's, so running off the end is a fault.
- **The range check knows.** A syscall's buffer is judged before the handler
  runs, and a page of the stack nobody has touched is the process's to use:
  refused, a `read()` into a large buffer on the stack - where programs keep
  them - would be EFAULT for every page not happened upon first.
- **Under the process's mm lock, which is tried and not waited for**: this is
  a fault handler, a thread of the same process may be forking on another
  core, and returning to fault again is how it waits. The frame comes from the
  privileged door, as a page coming back from swap does: the stack was
  promised at exec, and it is bounded.
- **`RLIMIT_STACK`** reports the two megabytes; it reported the four pages,
  then the sixty-four.
- **At boot** the ring-3 self-test runs a function with a hundred-kilobyte
  frame, has the kernel `read()` into a page a quarter of a megabyte below it,
  and forks a child that stores into the guard page and must die of SIGSEGV
  (`stack_selftest_failed`). The kernel counts the pages it gave
  (`stack_grown`), and a boot in which it gave none fails (`stack_never_grew`):
  the corpus prints through a `sed` that needs them.
- **The gate's count of deliberate faults is two now**, and each is matched by
  the address it touched - null for svc-crash, the guard page for this - where
  it used to excuse whichever ring-3 page fault came first. The crash-record
  check reads every record now and wants svc-crash's among them: it read the
  first one only, and the first 24-boot run failed four times on a record that
  correctly named `SELFTEST.ELF`, whenever that child died before svc-crash.
- **Sabotage**: `arch-stack-boot.txt`, two cases, red. Three went NOT RED
  first. The compiler probes every page of a large frame on entry, so the
  `read()` into the middle of the big array wrote into a page ring 3 had
  already touched - the test reads below the frame now. Widening the address
  test to include the guard page changed nothing, because the region list
  refuses it too. And a loop that zeroed the new page was dead code: the
  allocator hands out nothing but zeroed pages.
- **What it did to M-070**: 48 boots in two runs, none with its signature (the four that failed were the gate's own crash-record check, above), against three in about sixty before with the stack growing. A boot gives
  out about sixty stack pages in all, where it mapped sixty per process. The
  defect is not explained and its instrumentation stays; what changed is how
  much reclaim has to take that nobody is using.

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
