# System Call Interface

## Design goals

- compact, stable, orthogonal kernel API
- low overhead on hot paths
- minimal policy in the syscall surface
- compatibility enablement without kernel ABI explosion

## Design philosophy

VibeOS does not expose Linux, Win32, or Darwin syscalls directly as the native kernel ABI. Instead, it provides a smaller native syscall set centered on:

- process and thread management
- virtual memory operations
- IPC and synchronization
- time and timers
- object and handle management
- device and I/O primitives
- security token and capability operations

Compatibility layers translate foreign application behavior into this native model.

## Interface style

- handle-based operations rather than global integer descriptors where possible
- explicit rights checks at object boundaries
- structured error codes with deterministic semantics
- capability negotiation for optional features

## ABI v0 argument mapping (frozen baseline)

`vibeos_syscall_frame_t` keeps the fixed layout `(id, arg0, arg1, arg2, result)`.

Argument semantics for current syscall groups:

| Syscall | arg0 | arg1 | arg2 |
| --- | --- | --- | --- |
| `HANDLE_ALLOC` | rights mask | reserved (`0`) | caller pid (`0` = kernel context) |
| `HANDLE_CLOSE` | handle id | reserved (`0`) | caller pid (`0` = kernel context) |
| `EVENT_SIGNAL` | event handle | reserved (`0`) | reserved (`0`) |
| `WAITSET_ADD_EVENT` | event handle | waitset owner pid | caller pid |
| `WAITSET_STATS_GET` | reserved (`0`) | reserved (`0`) | caller pid (`0` = kernel or waitset owner) |
| `WAITSET_STATS_EXT_GET` | reserved (`0`) | reserved (`0`) | caller pid (`0` = kernel or waitset owner) |
| `WAITSET_WAKE_POLICY_SET` | wake policy enum (`0` FIFO, `1` REVERSE) | reserved (`0`) | caller pid (`0` = kernel or waitset owner) |
| `WAITSET_WAKE_POLICY_GET` | reserved (`0`) | reserved (`0`) | caller pid (`0` = kernel or waitset owner) |
| `WAITSET_STATS_RESET` | reserved (`0`) | reserved (`0`) | caller pid (`0` = kernel or waitset owner) |
| `WAITSET_OWNER_GET` | reserved (`0`) | reserved (`0`) | caller pid (`0` = kernel or waitset owner) |
| `WAITSET_SNAPSHOT_GET` | reserved (`0`) | reserved (`0`) | caller pid (`0` = kernel or waitset owner) |
| `PROCESS_SPAWN` | parent pid | reserved (`0`) | caller pid (`0` = kernel context) |
| `PROCESS_STATE_GET` | target pid | reserved (`0`) | caller pid (`0` = kernel; otherwise self or directly related process) |
| `PROCESS_STATE_SET` | target pid | target process state enum | caller pid (`0` = kernel; otherwise self only) |
| `PROCESS_TERMINATE` | target pid | reserved (`0`) | caller pid (`0` = kernel; otherwise self only) |
| `PROCESS_COUNT_GET` | reserved (`0`) | reserved (`0`) | reserved (`0`) |
| `THREAD_COUNT_GET` | reserved (`0`) | reserved (`0`) | reserved (`0`) |
| `PROCESS_LIVE_COUNT_GET` | reserved (`0`) | reserved (`0`) | reserved (`0`) |
| `PROCESS_TERMINATED_COUNT_GET` | reserved (`0`) | reserved (`0`) | reserved (`0`) |
| `PROCESS_STATE_COUNT_GET` | process state enum | reserved (`0`) | reserved (`0`) |
| `PROCESS_STATE_SUMMARY_GET` | reserved (`0`) | reserved (`0`) | reserved (`0`) |
| `PROC_TRANSITION_COUNTERS_GET` | reserved (`0`) | reserved (`0`) | reserved (`0`) |
| `PROC_TRANSITION_COUNTERS_RESET` | reserved (`0`) | reserved (`0`) | caller pid (`0` required) |
| `THREAD_CREATE` | pid | reserved (`0`) | reserved (`0`) |
| `THREAD_STATE_GET` | tid | reserved (`0`) | caller pid (`0` = kernel; otherwise owner pid only) |
| `THREAD_STATE_SET` | tid | target state enum | caller pid (`0` = kernel; otherwise owner pid only) |
| `THREAD_EXIT` | tid | reserved (`0`) | caller pid (`0` = kernel; otherwise owner pid only) |
| `THREAD_STATE_COUNT_GET` | thread state enum | reserved (`0`) | reserved (`0`) |
| `THREAD_STATE_SUMMARY_GET` | reserved (`0`) | reserved (`0`) | reserved (`0`) |
| `VM_MAP` | va | pa | len |
| `VM_UNMAP` | va | len | reserved (`0`) |
| `VM_PROTECT` | va | len | perms |
| `PROC_AUDIT_COUNT` | reserved (`0`) | reserved (`0`) | caller pid (`0` = kernel/global view) |
| `PROC_AUDIT_GET` | audit index (caller-local if `arg2 != 0`) | out: `success` | caller pid input, then out: `revoked_count` |
| `PROC_AUDIT_POLICY_SET` | retention policy (`0` overwrite-oldest, `1` drop-newest) | reserved (`0`) | caller pid (`0` required) |
| `PROC_AUDIT_POLICY_GET` | reserved (`0`) | reserved (`0`) | caller pid (`0` required) |
| `PROC_AUDIT_DROPPED` | reserved (`0`) | reserved (`0`) | caller pid (`0` required) |
| `SCHED_RUNNABLE_GET` | reserved (`0`) | reserved (`0`) | reserved (`0`) |
| `SCHED_RUNQUEUE_DEPTH_GET` | cpu id | reserved (`0`) | reserved (`0`) |
| `SCHED_CPU_COUNT_GET` | reserved (`0`) | reserved (`0`) | reserved (`0`) |
| `SCHED_PREEMPTIONS_GET` | cpu id | reserved (`0`) | reserved (`0`) |
| `SCHED_WAIT_TIMEOUTS_GET` | cpu id | reserved (`0`) | reserved (`0`) |
| `SCHED_WAIT_WAKES_GET` | cpu id | reserved (`0`) | reserved (`0`) |
| `SCHED_PREEMPTIONS_TOTAL_GET` | reserved (`0`) | reserved (`0`) | reserved (`0`) |
| `SCHED_WAIT_TIMEOUTS_TOTAL_GET` | reserved (`0`) | reserved (`0`) | reserved (`0`) |
| `SCHED_WAIT_WAKES_TOTAL_GET` | reserved (`0`) | reserved (`0`) | reserved (`0`) |
| `SCHED_COUNTER_SUMMARY_GET` | reserved (`0`) | reserved (`0`) | reserved (`0`) |
| `SCHED_COUNTERS_RESET` | reserved (`0`) | reserved (`0`) | caller pid (`0` required) |

| `PROC_AUDIT_COUNT_ACTION` | action enum | reserved (`0`) | caller pid (`0` = global, non-zero = caller-scoped) |
| `PROC_AUDIT_COUNT_SUCCESS` | success value (`0`/`1`) | reserved (`0`) | caller pid (`0` = global, non-zero = caller-scoped) |
| `PROC_AUDIT_SUMMARY` | reserved (`0`) | reserved (`0`) | caller pid (`0` = global, non-zero = caller-scoped) |
| `PROCESS_TOKEN_GET` | target pid | reserved (`0`) | caller pid (`0` = kernel; otherwise self or directly related process) |
| `PROCESS_TOKEN_SET` | target pid | capability mask | caller pid (`0` = kernel; otherwise self only) |
| `POLICY_CAPABILITY_GET` | policy target enum (`1` fs-open, `2` net-bind, `3` process-spawn, `4` process-interact override) | reserved (`0`) | caller pid (advisory, unrestricted read) |
| `POLICY_CAPABILITY_SET` | policy target enum (`1` fs-open, `2` net-bind, `3` process-spawn, `4` process-interact override) | required capability bit (`0..31`) | caller pid (`0` required) |
| `POLICY_SUMMARY_GET` | reserved (`0`) | reserved (`0`) | caller pid (advisory, unrestricted read) |
| `SEC_AUDIT_COUNT` | reserved (`0`) | reserved (`0`) | caller pid (`0` = global, non-zero = caller-scoped) |
| `SEC_AUDIT_GET` | event index (caller-local if `arg2 != 0`) | reserved (`0`) | caller pid (`0` = global, non-zero = caller-scoped) |
| `SEC_AUDIT_COUNT_ACTION` | security-audit action enum | reserved (`0`) | caller pid (`0` = global, non-zero = caller-scoped) |
| `SEC_AUDIT_COUNT_SUCCESS` | success value (`0`/`1`) | reserved (`0`) | caller pid (`0` = global, non-zero = caller-scoped) |
| `SEC_AUDIT_SUMMARY` | reserved (`0`) | reserved (`0`) | caller pid (`0` = global, non-zero = caller-scoped) |
| `SEC_AUDIT_RESET` | reserved (`0`) | reserved (`0`) | caller pid (`0` required) |
| `PROCESS_SECURITY_LABEL_GET` | target pid | reserved (`0`) | caller pid (`0` = kernel; otherwise self or directly related process) |
| `PROCESS_SECURITY_LABEL_SET` | target pid | security label | caller pid (`0` = kernel; otherwise self only) |
| `PROCESS_INTERACT_CHECK` | target pid | reserved (`0`) | caller pid (`!= 0`, no kernel shortcut) |

`PROC_AUDIT_GET` returns:
- `result` = `event.seq` (positive on success)
- `arg0` = `event.action`
- `arg1` = `event.success`
- `arg2` = `event.revoked_count`

`PROC_AUDIT_POLICY_GET` returns:
- `result` = current retention policy enum value.

`PROC_AUDIT_DROPPED` returns:
- `result` = cumulative number of dropped audit events (only when retention mode is `DROP_NEWEST`).

`WAITSET_STATS_GET` returns:
- `arg0` = `added`
- `arg1` = `removed`
- `arg2` = `wait_calls`
- `result` = `wait_timeouts`

`WAITSET_STATS_EXT_GET` returns:
- `arg0` = `wait_wakes`
- `arg1` = `ownership_denials`
- `arg2` = `owner_pid`
- `result` = current waitset event count

`WAITSET_WAKE_POLICY_GET` returns:
- `result` = current wake policy enum (`0` FIFO, `1` REVERSE).

`WAITSET_OWNER_GET` returns:
- `result` = waitset owner pid
- `arg0` = ownership enforcement flag
- `arg1` = current waitset event count

`WAITSET_SNAPSHOT_GET` returns:
- `result` = waitset owner pid
- `arg0` = ownership enforcement flag
- `arg1` = wake policy enum (`0` FIFO, `1` REVERSE)
- `arg2` = current waitset event count

`PROCESS_STATE_SUMMARY_GET` returns:
- `arg0` = count in `NEW`
- `arg1` = count in `RUNNING`
- `arg2` = count in `BLOCKED`
- `result` = count in `TERMINATED`

`THREAD_STATE_SUMMARY_GET` returns:
- `arg0` = count in `NEW`
- `arg1` = count in `RUNNABLE`
- `arg2` = count in `BLOCKED`
- `result` = count in `EXITED`

`PROC_TRANSITION_COUNTERS_GET` returns:
- `arg0` = process-state transition count
- `arg1` = thread-state transition count
- `arg2` = process-termination count
- `result` = thread-exit count

`SCHED_COUNTER_SUMMARY_GET` returns:
- `arg0` = total preemptions
- `arg1` = total wait timeouts
- `arg2` = total wait wakes
- `result` = total runnable threads

`PROC_AUDIT_SUMMARY` returns:
- `arg0` = total visible audit events
- `arg1` = visible successful events
- `arg2` = visible failed events
- `result` = `0` on success

`PROCESS_TOKEN_GET` returns:
- `arg0` = token uid
- `arg1` = token gid
- `arg2` = token capability mask
- `result` = `0` on success

`POLICY_CAPABILITY_GET` returns:
- `result` = required capability bit for the requested target

`POLICY_SUMMARY_GET` returns:
- `arg0` = required capability bit for fs-open policy checks
- `arg1` = required capability bit for net-bind policy checks
- `arg2` = required capability bit for process-spawn policy checks
- `result` = required capability bit for process-interact override checks

`SEC_AUDIT_GET` returns:
- `result` = security audit sequence (caller-local redacted sequence when `caller_pid != 0`)
- `arg0` = action enum (`1` process-spawn, `2` process-token-set, `3` policy-capability-set)
- `arg1` = success flag (`0`/`1`)
- `arg2` = target pid (`0` when not process-targeted)

`SEC_AUDIT_SUMMARY` returns:
- `arg0` = total visible security audit events
- `arg1` = visible successful events
- `arg2` = visible failed events
- `result` = `0` on success

`PROCESS_SECURITY_LABEL_GET` returns:
- `result` = target process security label

`PROCESS_INTERACT_CHECK` returns:
- `result` = `1` if caller is currently allowed to interact with target process according to label and override capability policy, `0` if denied

Access policy:
- `caller_pid == 0`: full global audit stream.
- `caller_pid != 0`: only events where `event.owner_pid == caller_pid`; `result` is redacted to caller-local sequence (`index + 1`).
- `PROC_AUDIT_POLICY_SET`, `PROC_AUDIT_POLICY_GET`, `PROC_AUDIT_DROPPED` are kernel-only (`caller_pid == 0`) in ABI v0.
- `PROCESS_STATE_GET` is kernel-only, self-introspection, or directly related process introspection (parent or child).
- `PROCESS_STATE_SET` and `PROCESS_TERMINATE` are kernel-only or self-targeted.
- `PROCESS_TOKEN_GET` is kernel-only, self-introspection, or directly related process introspection (parent or child).
- `PROCESS_TOKEN_SET` is kernel-only or self-targeted.
- `PROCESS_SECURITY_LABEL_GET` is kernel-only, self-introspection, or directly related process introspection (parent or child).
- `PROCESS_SECURITY_LABEL_SET` is kernel-only or self-targeted.
- `SEC_AUDIT_COUNT`, `SEC_AUDIT_GET`, `SEC_AUDIT_COUNT_ACTION`, and `SEC_AUDIT_SUMMARY` are kernel-global for `caller_pid == 0`, caller-scoped otherwise.
- `THREAD_STATE_GET`, `THREAD_STATE_SET`, `THREAD_EXIT` are kernel-only or thread-owner scoped.
- `WAITSET_STATS_GET` and `WAITSET_STATS_EXT_GET` are kernel-only or waitset-owner scoped.
- `WAITSET_WAKE_POLICY_SET`, `WAITSET_WAKE_POLICY_GET`, and `WAITSET_STATS_RESET` are kernel-only or waitset-owner scoped.
- `WAITSET_OWNER_GET` is kernel-only or waitset-owner scoped.
- `WAITSET_SNAPSHOT_GET` is kernel-only or waitset-owner scoped.
- `PROC_TRANSITION_COUNTERS_RESET` is kernel-only.
- `SCHED_COUNTERS_RESET` is kernel-only.
- `POLICY_CAPABILITY_SET` is kernel-only.
- `SEC_AUDIT_RESET` is kernel-only.

Implementation helpers for ABI v0 are centralized in `include/vibeos/syscall_abi.h` and should be preferred over direct field writes in kernel tests and user-space glue.

## ABI stability strategy

- native ABI is versioned
- deprecated calls remain shimmed for defined support windows
- compatibility runtimes own most foreign ABI churn

## Performance considerations

- vDSO-style user mappings for clock and other safe queries in future phases
- batched IPC and handle transfer operations
- minimal-copy I/O design for large transfers

## Early syscall groups

- process
- thread
- address_space
- memory_object
- channel
- event
- timer
- device
- debug and trace

## The Linux ABI as implemented on metal

The native ABI above is the design. Separately, and in parallel, the x86-64 port
serves the Linux system-call ABI directly to ring 3, because that is what lets
real programs run before a native userland exists. Entry is the `syscall`
instruction (with `int 0x80` kept as a bring-up path); both reach one dispatcher
in `kernel/arch/x86_64/arch_hw.c`.

Two rules govern what goes in it:

- **A syscall either does the thing or returns the error Linux returns.**
  Reporting a success that did not happen makes the program fail later,
  somewhere unrelated, with nothing pointing back to the kernel. `rseq` and
  `getrandom` return `-ENOSYS` for exactly this reason: both have mandatory
  fallbacks, and a fabricated answer is worse than a refusal - particularly for
  the syscall a program uses to get key material.
- **Unimplemented numbers are named in the serial log**, so a missing one is
  visible the first time it is reached rather than inferred from a crash.

The list below is the `switch` in `vibeos_x86_64_linux_syscall`, not an
aspiration. Anything not in it is refused with `-ENOSYS` and named in the log.

| Group | Calls |
| --- | --- |
| Process | `fork`, `clone`, `execve`, `wait4`, `exit`, `exit_group`, `getpid`, `getppid`, `gettid`, `prctl`, `set_tid_address`, `set_robust_list`, `setuid`, `setgid` |
| Signals | `rt_sigaction`, `rt_sigprocmask`, `rt_sigreturn`, `kill`, `tkill`, `tgkill` |
| Memory | `brk`, `mmap`, `mprotect`, `munmap` |
| Files | `open`, `openat`, `close`, `read`, `write`, `readv`, `writev`, `lseek`, `fstat`, `newfstatat`, `getdents64`, `getcwd`, `readlinkat`, `unlink`, `mkdir`, `ioctl` |
| Descriptors | `pipe`, `pipe2`, `dup`, `dup2` |
| Sockets | `socket`, `bind`, `listen`, `connect`, `accept`, `sendto`, `recvfrom` |
| Time | `clock_gettime`, `time` |
| Runtime startup | `arch_prctl`, `uname`, `prlimit64`, `futex`, `sched_yield`, `getuid`/`geteuid`/`getgid`/`getegid` |

Deliberately refused with `-ENOSYS`: `rseq`, `getrandom`, `sendfile`, and
`clone` when `CLONE_VM` or `CLONE_THREAD` is set. Each has a documented caller
fallback; a fabricated success would move the failure somewhere unrelated.

Notable semantics:

- `arch_prctl(ARCH_SET_FS)` writes `MSR_FS_BASE` and the value is per task,
  reloaded on every context switch - thread-local storage is the first thing a
  C runtime touches, and leaving one task's base loaded for another would let a
  program read someone else's thread state. A base outside user memory is
  refused before the write, since a non-canonical value faults in ring 0.
- `mprotect` and `munmap` change the real page-table entries and flush the TLB.
  `MAP_FIXED` and file-backed `mmap` are refused rather than ignored.
- `ioctl` returns `-ENOTTY`. There is no terminal device here, and that is the
  answer a libc uses to decide stdout should be block buffered. `fstat` on a
  standard descriptor agrees with it: a character device that is not a
  terminal.
- `fork` shares pages instead of copying them. Every writable user leaf becomes
  read-only and copy-on-write in both address spaces, the frame's reference
  count goes up, and the first write from either side faults and duplicates it.
  Three details make it work rather than nearly work: the fault handler must
  accept a fault raised in ring 0, because the kernel writes user buffers with
  `CR0.WP` set and `read()` filling a freshly forked buffer is exactly such a
  write; `hw_user_range_ok` must treat a copy-on-write page as writable, or a
  syscall rejects the buffer before the fault can fix it; and a second `fork`
  must recognise an already-COW page rather than filing it under "read-only, so
  it never needs copying", which would leave it permanently unwritable.
- Signals are real. `rt_sigaction` and `rt_sigprocmask` install dispositions and
  masks that are honoured; a raise sets a pending bit and wakes the task if it
  was blocked; delivery happens on the way back to user space, where the
  process's own stack is available. The frame is built below the 128-byte red
  zone, 16-byte aligned with room for a return address, and the return address
  is the C library's `SA_RESTORER` trampoline - a handler installed without one
  cannot return anywhere, so the signal takes its default action instead.
  `rt_sigreturn` restores the saved frame but forces `cs`, `ss` and `rflags`
  back to safe values, because that frame lives in memory the program can
  write and nothing read out of it may decide privilege. A magic word in the
  frame is checked for the same reason.
- A Linux `sigset_t` numbers its bits from zero: bit 0 is signal 1. This kernel
  numbers them by signal, and converts only in `hw_sigset_from_user` /
  `hw_sigset_to_user`. Getting it wrong shifts every mask by one.
- `SIGKILL` and `SIGSTOP` can be neither caught nor blocked: `rt_sigaction`
  returns `EINVAL` for them and `rt_sigprocmask` accepts the request and drops
  the two bits, as Linux does.
- A wait status from `wait4` is not an exit code. The exit code sits in the
  high byte and a killing signal in the low seven bits; `128 + sig` is what a
  shell prints, not what the kernel stores.
- `pipe2` allocates one buffer and two descriptors, read end first; `O_CLOEXEC`
  is accepted and ignored, since there is no close-on-exec list. `dup2` onto 0,
  1 or 2 records a standard-descriptor redirection, which is how a shell
  attaches a pipe to a program that knows nothing about it. `dup` is `dup2`
  onto the lowest free descriptor. Descriptors are inherited across `fork` with
  the pipe's reader and writer counts incremented, and released on exit - a
  pipe stays alive until both ends are gone, so a reader can still drain data
  after the last writer closed. Writing to a pipe with no readers raises
  `SIGPIPE` and returns `EPIPE`.
- `clone` is what a C library actually calls for `fork`. Flags that mean a
  thread (`CLONE_VM`, `CLONE_THREAD`) return `-ENOSYS`; anything else forks.
  `tkill` and `tgkill` reduce to `kill`, since there is one thread per process.
- `getcwd` returns `/` and `readlinkat` answers only `/proc/self/exe`, from the
  path `execve` was given. Both say what is true rather than inventing a path a
  program would then fail to open.
- `openat` accepts only `AT_FDCWD`; `newfstatat` additionally handles an empty
  path with `AT_EMPTY_PATH`, and distinguishes a directory from a file by
  trying to list it, because `ls` behaves differently for each.
- `setuid`/`setgid` succeed for 0 and return `EPERM` otherwise. There is one
  identity here and it is root.
- `time` and `clock_gettime` are derived from the timer tick, not from a
  real-time clock.

### How much of this a gate defends

The boot gate (`scripts/qemu-cli-smoke-linux.py`) drives these paths through
real programs rather than a test written to agree with the kernel. It fails on
`signal_delivery_broken` if a program built by `musl-gcc` against a real libc
(`tests/linux/musl_signal.c`, staged as `SIGNAL.ELF`) does not print `SIG_OK`
after exercising handlers, masking, ignoring and default actions - the
trampoline a handler returns through comes from that libc, not from a test that
agrees with the kernel; on `pipeline_did_not_complete` if
`ls /EFI/BOOT | wc -l` in BusyBox's shell does not get as far as the `PIPE_OK`
that follows it; and on `busybox_file_operations_failed` if `openat`, `fstat`,
`read` or `getdents64` regress, since those are BusyBox's own error messages.

Copy-on-write is reported, not asserted: the kernel prints
`[MM] COW_STATS shared=... copied=...` at the end of the self-test - 1221 and 24
on the boot this was written against - but no invariant checks the numbers. What
the gate defends is that forking and exec'ing programs keep working.

## Compatibility note

Linux compatibility may expose a Linux-like user ABI inside its subsystem, but translation happens above the native kernel API unless a measured exception is justified.
