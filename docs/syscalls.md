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
| `PROCESS_SPAWN` | parent pid | reserved (`0`) | reserved (`0`) |
| `PROCESS_STATE_GET` | target pid | reserved (`0`) | caller pid (`0` = kernel; otherwise self or directly related process) |
| `PROCESS_STATE_SET` | target pid | target process state enum | caller pid (`0` = kernel; otherwise self only) |
| `PROCESS_TERMINATE` | target pid | reserved (`0`) | caller pid (`0` = kernel; otherwise self only) |
| `PROCESS_COUNT_GET` | reserved (`0`) | reserved (`0`) | reserved (`0`) |
| `THREAD_COUNT_GET` | reserved (`0`) | reserved (`0`) | reserved (`0`) |
| `PROCESS_LIVE_COUNT_GET` | reserved (`0`) | reserved (`0`) | reserved (`0`) |
| `PROCESS_TERMINATED_COUNT_GET` | reserved (`0`) | reserved (`0`) | reserved (`0`) |
| `THREAD_CREATE` | pid | reserved (`0`) | reserved (`0`) |
| `THREAD_STATE_GET` | tid | reserved (`0`) | caller pid (`0` = kernel; otherwise owner pid only) |
| `THREAD_STATE_SET` | tid | target state enum | caller pid (`0` = kernel; otherwise owner pid only) |
| `THREAD_EXIT` | tid | reserved (`0`) | caller pid (`0` = kernel; otherwise owner pid only) |
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

`PROC_AUDIT_GET` returns:
- `result` = `event.seq` (positive on success)
- `arg0` = `event.action`
- `arg1` = `event.success`
- `arg2` = `event.revoked_count`

`PROC_AUDIT_POLICY_GET` returns:
- `result` = current retention policy enum value.

`PROC_AUDIT_DROPPED` returns:
- `result` = cumulative number of dropped audit events (only when retention mode is `DROP_NEWEST`).

Access policy:
- `caller_pid == 0`: full global audit stream.
- `caller_pid != 0`: only events where `event.owner_pid == caller_pid`; `result` is redacted to caller-local sequence (`index + 1`).
- `PROC_AUDIT_POLICY_SET`, `PROC_AUDIT_POLICY_GET`, `PROC_AUDIT_DROPPED` are kernel-only (`caller_pid == 0`) in ABI v0.
- `PROCESS_STATE_GET` is kernel-only, self-introspection, or directly related process introspection (parent or child).
- `PROCESS_STATE_SET` and `PROCESS_TERMINATE` are kernel-only or self-targeted.
- `THREAD_STATE_GET`, `THREAD_STATE_SET`, `THREAD_EXIT` are kernel-only or thread-owner scoped.

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

## Compatibility note

Linux compatibility may expose a Linux-like user ABI inside its subsystem, but translation happens above the native kernel API unless a measured exception is justified.
