# ADR-0005: Virtual Memory Model

## Status

Accepted

## Context

The OS must support strong isolation, future compatibility runtimes, memory-mapped files, and multicore-safe execution.

## Decision

The virtual memory model uses:

- per-process address spaces
- higher-half kernel mapping
- direct physical map for kernel internal operations
- explicit memory objects backing mappings
- copy-on-write support
- file-backed and shared-memory mappings

## Consequences

Positive:

- clear ownership and accounting for mappings
- strong base for compatibility runtimes
- natural integration with page cache and filesystem services

Negative:

- VM subsystem design is more complex than a minimal flat mapper
- TLB invalidation and mapping lifecycle need careful SMP handling

## Implementation notes

- the first bring-up should prioritize early paging correctness, kernel map stability, and user-space address-space creation
- demand paging may be staged after initial static mapping support if needed for schedule control
