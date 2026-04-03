# ADR-0004: Scheduler Foundation

## Status

Accepted

## Context

The system targets multicore performance from the start but cannot afford scheduler complexity that blocks bring-up.

## Decision

The first scheduler implementation uses:

- preemptive scheduling
- priority bands
- per-CPU run queues
- round-robin within priority bands
- SMP load balancing with affinity awareness

The design must preserve an upgrade path to weighted fairness, topology awareness, and later real-time classes.

## Consequences

Positive:

- implementation remains tractable for early milestones
- contention is reduced relative to a single global queue
- wake-up and balancing logic can evolve incrementally

Negative:

- fairness is approximate in early versions
- topology and energy awareness are deferred

## Implementation notes

- starvation prevention via aging is required even in the first version
- scheduler tracing hooks are mandatory so later tuning is evidence-based
- preemption and timer behavior must be testable in QEMU
