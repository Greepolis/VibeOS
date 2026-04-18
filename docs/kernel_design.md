# Kernel Design

## Kernel scope

The kernel core is intentionally small and responsible only for primitives that are unsafe or inefficient to place in user space.

## Core responsibilities

- interrupt and exception handling
- low-level memory management
- scheduling and dispatch
- process and thread lifecycle primitives
- inter-process communication primitives
- synchronization objects
- timekeeping and timers
- capability-aware object management
- architecture and platform abstraction

## Object model

Every kernel-visible resource is represented as a typed object:

- process
- thread
- address space
- channel
- shared memory object
- event
- timer
- device endpoint
- job or resource domain

Objects are referenced through handles. Handles carry rights masks and can be transferred in controlled ways through IPC.

Current runtime prototype updates:
- handle tables support configurable per-table quota limits.
- handle allocation failure counters are tracked for observability.
- object-type counters are available for process/thread handle accounting.
- handle lifecycle hooks are available for alloc/close/revoke event instrumentation and cleanup orchestration.

## Process model

### Process

A process owns:

- one address space
- one handle table
- resource accounting state
- security token and capability set
- one or more threads

### Thread

A thread owns:

- register state
- scheduling attributes
- signal or exception delivery state
- thread-local storage metadata

## IPC model

IPC is central to the system design.

See also: `docs/ipc.md` for the dedicated transport and contract strategy.

### Requirements

- low overhead for local RPC-style service calls
- secure handle passing
- async messaging for service-oriented architecture
- support for compatibility runtimes that need brokering

### Mechanisms

- message channels with bounded queues
- shared-memory regions for bulk data
- event signaling and wait sets
- optional zero-copy path for large transfers where mappings permit

## Kernel module policy

Kernel modules are permitted only when they satisfy at least one of these conditions:

- required during early boot
- latency-sensitive enough to justify privileged execution
- too tightly coupled to interrupt-time hardware handling

All other subsystems should prefer user-space services.

## Failure containment

- drivers and filesystems should restart independently when hosted out of kernel
- kernel panics should preserve diagnosable crash data
- watchdog supervision should detect failed core services

## Observability

The kernel should expose:

- structured tracepoints
- per-CPU scheduling statistics
- memory and IPC counters
- fault reporting interfaces
- boot and runtime diagnostics suitable for automated testing

## Current runtime prototype status

- interrupt controller runtime now supports IRQ masking/unmasking and global enable/disable gates.
- timer runtime includes tick-to-time conversion helpers, deadline arming/expiry checks, and IRQ-backend binding semantics.
- process security model includes token capabilities, security labels, and policy-driven interaction checks.
