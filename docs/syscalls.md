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
