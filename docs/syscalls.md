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

## Compatibility note

Linux compatibility may expose a Linux-like user ABI inside its subsystem, but translation happens above the native kernel API unless a measured exception is justified.
