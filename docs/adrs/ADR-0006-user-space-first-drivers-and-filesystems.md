# ADR-0006: User-Space-First Drivers and Filesystems

## Status

Accepted

## Context

Drivers and filesystems are major sources of complexity, parser risk, and fault amplification in operating systems. VibeOS also needs a natural place for compatibility-related mediation and policy enforcement.

## Decision

Drivers and concrete filesystem implementations default to user-space hosts or services.

Kernel-space exceptions are allowed only when:

- required before trusted user-space exists
- the interrupt or latency path cannot tolerate the boundary
- benchmark evidence justifies privileged placement

## Consequences

Positive:

- failure containment improves
- parser-heavy code stays outside the kernel
- service restart and supervision become possible

Negative:

- IPC and shared-memory data paths become performance-critical
- DMA and MMIO mediation need careful design

## Implementation notes

- the device manager and filesystem service contracts must be versioned
- storage and network fast paths should be re-evaluated after profiling, not guessed in advance
