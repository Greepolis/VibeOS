# ADR-0003: Handle-Based Native ABI

## Status

Accepted

## Context

The OS must support multiple compatibility environments without bloating the native kernel ABI. A small, orthogonal API surface simplifies security review and long-term maintenance.

## Decision

The native syscall ABI is handle-based and object-oriented:

- kernel resources are represented as typed objects
- user space references them through handles carrying explicit rights masks
- foreign ABI behavior is translated above this interface

The native kernel ABI shall not attempt to expose Linux, Win32, or Darwin syscalls directly.

## Consequences

Positive:

- cleaner rights enforcement
- easier policy mediation at object boundaries
- compatibility layers can evolve independently of native ABI shape

Negative:

- compatibility runtimes must do non-trivial semantic translation
- some foreign descriptor or handle conventions may be awkward to map

## Implementation notes

- initial object set should include process, thread, address space, channel, event, timer, memory object, and device endpoint
- handle transfer through IPC must preserve rights semantics
