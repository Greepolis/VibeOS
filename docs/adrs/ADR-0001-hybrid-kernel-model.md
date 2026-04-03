# ADR-0001: Hybrid Kernel Model

## Status

Accepted

## Context

VibeOS requires:

- strong isolation for risky components
- low overhead on kernel hot paths
- room for compatibility runtimes and service-oriented evolution

A pure monolithic kernel conflicts with the security and compatibility goals. A pure microkernel increases risk for early implementation complexity and performance regressions if IPC and service orchestration are not yet mature.

## Decision

VibeOS adopts a hybrid microkernel-inspired modular architecture:

- kernel core remains small and primitive-oriented
- user-space services host drivers, filesystems, policy engines, and compatibility runtimes by default
- privileged execution is allowed only for boot-critical, interrupt-critical, or benchmark-justified components

## Consequences

Positive:

- smaller trusted computing base than a monolithic design
- safer placement for compatibility layers
- controlled path for restartable services

Negative:

- architectural governance burden increases
- IPC performance becomes a first-order success factor
- some subsystems may need later relocation based on measurement

## Implementation notes

- placement exceptions require benchmark evidence
- subsystem contracts must be explicit and versioned
- the first design review in Phase 2 must verify that early bootstrap code does not create accidental permanent kernel dependencies
