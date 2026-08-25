# System Call Interface Progress

Status: In Progress (Linux process semantics verified from ring 3)
Last review: 2026-08-25

## Implemented
- **Real on-metal ring-3 -> ring-0 syscall entry** (`kernel/arch/x86_64/arch_hw.c` + `isr.S`), two paths into one Linux-ABI dispatcher (`vibeos_x86_64_linux_syscall`):
  - **Native `syscall`/`sysret`** (the real Linux x86-64 fast path): EFER.SCE enabled, STAR/LSTAR/SFMASK programmed, and an LSTAR trampoline that switches to a kernel stack, marshals the Linux ABI (nr in rax; args in rdi/rsi/rdx) into the C ABI, dispatches, and `sysretq`s back to ring 3.
  - **`int 0x80` gate** (DPL 3) as a legacy/bring-up path into the same dispatcher.
  - User GDT segments ordered data-then-code (for SYSRET) + a TSS with RSP0.
  - Linux syscall translation covers the runtime subset used by the user programs, including console I/O, process lifecycle and FAT-backed file operations; unsupported calls return `-ENOSYS` and are named in the serial log, so a missing number is visible rather than silent.
  - **The opening sequence of a C runtime.** A static libc runs a fixed set of syscalls before it reaches `main` and dies if any of them fails. Implemented: `arch_prctl` (`ARCH_SET_FS`/`ARCH_GET_FS`), `set_tid_address`, `set_robust_list`, `gettid`, `ioctl`, `writev`, `readv`, `uname`, `clock_gettime`, `prlimit64`, `futex`, `sched_yield`, `rt_sigprocmask`, `rt_sigaction`, and the `getuid`/`getgid` family. `rseq` and `getrandom` return `-ENOSYS` deliberately: both have mandatory fallbacks, and a fake success would be worse than a refusal.
  - **Thread-local storage is real.** `arch_prctl(ARCH_SET_FS)` writes `MSR_FS_BASE`, the value is per task, and it is reloaded on every context switch - without that, one program reads another's thread state. A base outside user memory is refused before the write, because a non-canonical value would fault in ring 0. Verified from ring 3 by reading the TLS self-pointer back through `%fs:0` after the task has been preempted and rescheduled many times, not by checking that the syscall returned zero.
  - **`mmap`, `mprotect` and `munmap` act on the page tables.** `mprotect` grants and revokes write permission on the real entries and flushes the TLB; `munmap` unmaps and frees the frames, so a use-after-unmap faults here as it would on Linux. `MAP_FIXED` and file-backed mappings are refused rather than quietly ignored - a program that asks for a specific address and silently gets another one corrupts itself later, far from the call.
  - **What a program does once it is running**, beyond the startup sequence:
    `fstat`, `newfstatat`, `openat`, `getcwd`, `readlinkat`, `prctl`,
    `setuid`/`setgid`, `getppid`, `time`, and `clone` for the flags that mean
    `fork`. These are the calls that separate "a libc starts" from "a program
    works": BusyBox reaches the filesystem through `openat`, `fstat`, `read`
    and `getdents64`, and the boot fails on `busybox_file_operations_failed`
    if any of them regresses.
  - **Signal delivery is implemented, not stubbed.** `rt_sigaction` and
    `rt_sigprocmask` install real dispositions and masks; `kill`, `tkill` and
    `tgkill` raise; a raise sets a pending bit and wakes a task blocked in
    `read()`. Delivery happens on the way back to user space, where the
    process's own stack exists: the frame goes below the 128-byte red zone,
    aligned as a call site, with the C library's `SA_RESTORER` trampoline as
    the return address. `rt_sigreturn` restores that frame but forces `cs`,
    `ss` and `rflags` back to safe values and checks a magic word first,
    because the frame is user-writable and nothing read out of it may decide
    privilege. `SIGKILL` and `SIGSTOP` are neither catchable nor blockable.
    Linux `sigset_t` bits are numbered from zero (bit 0 is signal 1) and are
    converted only in `hw_sigset_from_user`/`hw_sigset_to_user`. A wait status
    is not an exit code: exit code in the high byte, signal in the low seven
    bits. Gated - the boot fails on `signal_delivery_broken` if
    `tests/linux/musl_signal.c`, built by `musl-gcc` against a real libc, does
    not report `SIG_OK`.
  - **Pipes and descriptor plumbing**: `pipe`, `pipe2`, `dup`, `dup2`,
    inheritance across `fork` with the pipe's reader/writer counts adjusted,
    release on exit, and `SIGPIPE` plus `EPIPE` for a write with no reader. A
    pipe outlives its last writer so a reader can drain it. `dup2` onto 0, 1
    or 2 records a standard-descriptor redirection, since the console is not
    an entry in the descriptor table. Gated: `ls /EFI/BOOT | wc -l` runs in
    BusyBox's shell and the boot fails on `pipeline_did_not_complete`.
  - Verified in QEMU (GCC and Clang): a **real user ELF** is loaded (parsed by the portable, host-tested `kernel/core/elf.c`), run in ring 3, and prints via a native `syscall`, then `exit` returns cleanly to the kernel. This hardware front end will route into the portable dispatcher and the Linux personality (`user/compat/linux`) next.
- Central syscall dispatcher in `kernel/core/syscall.c` (portable model).
- ABI helper mappings in `include/vibeos/syscall_abi.h`.
- Rights/policy lookup layer in `kernel/core/syscall_policy.c`.
- Process/thread lifecycle syscall family.
- Scheduler observability and runtime-control syscall family.
- Waitset ownership/stats/wake-policy syscall paths.
- Security/audit/policy control syscalls with kernel-vs-user authorization checks.
- ABI version/compatibility syscalls (`VIBEOS_SYSCALL_ABI_VERSION_GET`, `VIBEOS_SYSCALL_ABI_COMPAT_CHECK`) with compatibility helpers in `syscall_abi.h`.
- The on-metal dispatcher validates user pointers before dereference for the implemented Linux-ABI subset.

## Pending
- `nanosleep` and `poll`. A shell that waits on several descriptors at once needs `poll`; the pipeline path currently blocks on one at a time.
- `clone` with `CLONE_VM`/`CLONE_THREAD` returns `-ENOSYS`, and real futexes belong with real threads rather than before them.
- Signal gaps: no `SA_SIGINFO` `siginfo_t`, no alternate signal stack (`sigaltstack`), no `rt_sigsuspend`/`rt_sigpending`, and a handler installed without `SA_RESTORER` cannot be entered at all - the process is terminated instead of being given a kernel-supplied trampoline. A three-argument `SA_SIGINFO` handler is entered with its second and third arguments zeroed.
- `openat` accepts only `AT_FDCWD` and there is no per-process working directory, so relative paths resolve against the volume root.
- `sendfile` is refused; callers are expected to take their read-and-write fallback.
- Copy-on-write statistics are printed (`[MM] COW_STATS`) but no gate asserts them.
- An entropy source, so `getrandom` can answer and `AT_RANDOM` can carry something better than nothing.
- Stronger multi-tenant authorization model beyond relation-based checks.
- Syscall ABI versioning strategy with compatibility negotiation.
- Per-syscall latency telemetry and regression thresholds.
- Consolidate the hardware Linux-ABI dispatcher and portable syscall policy dispatcher behind one audited authorization boundary.

## Next checkpoint
- `poll` and `nanosleep`, so a shell can wait on more than one descriptor and a program can sleep without spinning.
- Consolidate the runtime and portable dispatch paths, then add negative pointer/permission tests for every exposed syscall.
