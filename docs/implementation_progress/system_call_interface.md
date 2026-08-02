# System Call Interface Progress

Status: In Progress (Linux startup ABI verified from ring 3)
Last review: 2026-08-02

## Implemented
- **Real on-metal ring-3 -> ring-0 syscall entry** (`kernel/arch/x86_64/arch_hw.c` + `isr.S`), two paths into one Linux-ABI dispatcher (`vibeos_x86_64_linux_syscall`):
  - **Native `syscall`/`sysret`** (the real Linux x86-64 fast path): EFER.SCE enabled, STAR/LSTAR/SFMASK programmed, and an LSTAR trampoline that switches to a kernel stack, marshals the Linux ABI (nr in rax; args in rdi/rsi/rdx) into the C ABI, dispatches, and `sysretq`s back to ring 3.
  - **`int 0x80` gate** (DPL 3) as a legacy/bring-up path into the same dispatcher.
  - User GDT segments ordered data-then-code (for SYSRET) + a TSS with RSP0.
  - Linux syscall translation covers the runtime subset used by the user programs, including console I/O, process lifecycle and FAT-backed file operations; unsupported calls return `-ENOSYS` and are named in the serial log, so a missing number is visible rather than silent.
  - **The opening sequence of a C runtime.** A static libc runs a fixed set of syscalls before it reaches `main` and dies if any of them fails. Implemented: `arch_prctl` (`ARCH_SET_FS`/`ARCH_GET_FS`), `set_tid_address`, `set_robust_list`, `gettid`, `ioctl`, `writev`, `readv`, `uname`, `clock_gettime`, `prlimit64`, `futex`, `sched_yield`, `rt_sigprocmask`, `rt_sigaction`, and the `getuid`/`getgid` family. `rseq` and `getrandom` return `-ENOSYS` deliberately: both have mandatory fallbacks, and a fake success would be worse than a refusal.
  - **Thread-local storage is real.** `arch_prctl(ARCH_SET_FS)` writes `MSR_FS_BASE`, the value is per task, and it is reloaded on every context switch - without that, one program reads another's thread state. A base outside user memory is refused before the write, because a non-canonical value would fault in ring 0. Verified from ring 3 by reading the TLS self-pointer back through `%fs:0` after the task has been preempted and rescheduled many times, not by checking that the syscall returned zero.
  - **`mmap`, `mprotect` and `munmap` act on the page tables.** `mprotect` grants and revokes write permission on the real entries and flushes the TLB; `munmap` unmaps and frees the frames, so a use-after-unmap faults here as it would on Linux. `MAP_FIXED` and file-backed mappings are refused rather than quietly ignored - a program that asks for a specific address and silently gets another one corrupts itself later, far from the call.
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
- The syscalls a real static binary needs beyond startup: `fstat`/`newfstatat`, `openat`, `readlinkat`, `getcwd`, `nanosleep`, `dup`/`dup2`, `poll`. These are what separate "a libc starts" from "a program runs".
- Signal delivery. `rt_sigaction` and `rt_sigprocmask` are accurate only because no signal is ever delivered; they become lies the moment one is.
- Real futexes, which belong with real threads rather than before them.
- An entropy source, so `getrandom` can answer and `AT_RANDOM` can carry something better than nothing.
- Stronger multi-tenant authorization model beyond relation-based checks.
- Syscall ABI versioning strategy with compatibility negotiation.
- Per-syscall latency telemetry and regression thresholds.
- Consolidate the hardware Linux-ABI dispatcher and portable syscall policy dispatcher behind one audited authorization boundary.

## Next checkpoint
- Run an unmodified static Linux binary (musl or BusyBox) end to end, and implement exactly the syscalls its trace shows are missing.
- Consolidate the runtime and portable dispatch paths, then add negative pointer/permission tests for every exposed syscall.
