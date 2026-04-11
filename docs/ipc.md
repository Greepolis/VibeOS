# IPC Design

## Why IPC is central

VibeOS depends on IPC not only for ordinary application communication but also for core system structure. Drivers, filesystems, compatibility layers, policy services, and service supervision all rely on IPC as the primary contract surface between protection domains.

## Goals

- low latency for request-response service calls
- secure transfer of handles and shared memory references
- bounded memory usage under load
- clean support for async and sync communication
- debuggable and versionable service contracts

## Primitive set

### Channels

Bidirectional or paired message channels are the default control-path primitive.

Properties:

- message boundaries preserved
- kernel-validated handle passing
- optional reply capability for RPC-style interactions
- cancellation-aware waiting

### Shared memory

Shared memory complements channels for bulk transfer.

Use cases:

- filesystem I/O buffers
- graphics surfaces
- network packet rings
- compatibility runtime staging buffers

### Events and wait sets

Events allow low-cost signaling. Wait sets allow a service to block on multiple channels, timers, and events without ad hoc polling loops.

Current runtime notes:
- wait sets support `FIFO`, `REVERSE`, and `ROUND_ROBIN` wake policy modes.
- round-robin wake policy keeps a rotating cursor to improve fairness under sustained multi-event contention.

## Contract model

- every long-lived system service should publish a versioned IPC contract
- contracts should be documented separately from implementation code
- incompatible protocol changes require explicit version negotiation or side-by-side compatibility
- security-sensitive services should prefer brokered narrow interfaces over generic escape hatches

## Performance strategy

- fast path for small fixed-size messages
- shared-memory-assisted transfer for large payloads
- per-CPU delivery queues where appropriate
- backpressure on overloaded receivers
- tracing hooks to measure queue depth, latency, and copy counts

## Security strategy

- sender identity and rights checked at receive boundary
- no ambient authority through channel inheritance by default
- handle transfer allowed only with explicit rights masks
- broker services mediate privileged operations for sandboxed runtimes

## Early implementation scope

- local same-machine IPC only
- synchronous request-response and asynchronous notification
- handle passing
- wait sets and timeout support

Distributed IPC or transparent remote IPC is explicitly out of scope for early phases.
