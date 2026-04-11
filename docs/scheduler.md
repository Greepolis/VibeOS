# Scheduler

## Goals

- low wake-up latency
- fairness under mixed interactive and background workloads
- multicore scalability
- affinity and topology awareness
- support for real-time and near-real-time classes in later phases

## Scheduling model

VibeOS uses a preemptive priority-based scheduler with fair scheduling within priority bands.

## Thread classes

- realtime reserved for strict bounded-latency system tasks in later phases
- interactive for UI and latency-sensitive applications
- normal for standard user workloads
- background for maintenance and batch work

## Core design

### Per-CPU run queues

Each CPU maintains a local run queue to reduce lock contention and cache disruption.

### Load balancing

Periodic and event-driven balancing migrates threads across CPUs based on:

- CPU utilization
- thread affinity
- cache locality hints
- NUMA domain awareness in future phases

### Priority handling

- dynamic boosts for wake-up responsiveness
- starvation prevention through aging
- priority ceilings for specific kernel-mediated synchronization paths when necessary

## Timeslice policy

- interactive threads receive shorter slices with stronger latency preference
- throughput-oriented threads receive longer slices
- kernel workers should avoid monopolizing CPU time by default

## Multicore strategy

- minimal global scheduler state
- per-CPU timer events where possible
- explicit handling for idle CPUs and wake-up targets
- future support for heterogeneous core topologies

## Roadmap

### Early implementation

- fixed priority bands
- round-robin within queues
- basic SMP balancing

### Mid-stage evolution

- fair scheduling weights
- better I/O wake heuristics
- CPU topology awareness

### Advanced stage

- real-time scheduling class
- deadline scheduling for selected workloads
- energy-aware placement for mobile-class platforms

## Current runtime prototype status

- class-based default timeslice selection is implemented (`background=8`, `normal=4`, `interactive=2`, `realtime=1`).
- enqueue paths normalize zero-timeslice threads to class defaults.
- per-CPU and aggregate scheduler counters remain available through runtime telemetry helpers and syscall observability paths.
- runqueue aging helpers are available (`age_cpu`, `age_all`) to apply bounded timeslice boosts and reduce starvation risk.
