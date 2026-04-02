# Architecture

## Kernel architecture options evaluated

### Monolithic kernel

Advantages:

- best-case performance due to fewer context switches and direct internal calls
- simpler early boot and subsystem bring-up
- mature development pattern used by several high-performance kernels

Disadvantages:

- weak fault isolation for drivers and services
- larger trusted computing base
- harder long-term maintainability for a compatibility-heavy OS
- greater risk when integrating experimental subsystems

### Microkernel

Advantages:

- strong isolation for drivers, filesystems, and services
- smaller privileged kernel core
- better resilience and restartability of failed services
- natural fit for compatibility subsystems living outside kernel mode

Disadvantages:

- IPC overhead can harm performance if not designed carefully
- more complex service orchestration and bootstrapping
- harder path for latency-critical subsystems if purity is enforced rigidly

### Hybrid kernel

Advantages:

- allows microkernel-like compartmentalization without forcing every service out of kernel space
- lets the project keep latency-critical primitives in privileged space
- supports pragmatic evolution as performance data becomes available

Disadvantages:

- architectural discipline is harder to maintain
- can devolve into a monolith without strict module boundaries
- requires careful criteria for what may run in kernel space

### Modular monolithic kernel

Advantages:

- practical implementation path
- kernel modules can be developed independently
- strong performance characteristics

Disadvantages:

- modules still share the same failure domain
- weaker security boundaries for drivers and compatibility components
- poor fit for long-term least-privilege goals

## Final decision

VibeOS adopts a hybrid microkernel-inspired modular architecture.

### Rationale

This design best satisfies the project goals:

- performance-critical primitives remain in a compact kernel core
- compatibility services and risky subsystems can run outside the kernel
- the system can evolve toward stronger isolation without rewriting the core
- user-space services can host Linux, Windows, and macOS translation layers more safely than a monolithic compatibility design

## High-level structure

### Ring 0 kernel core

- trap and interrupt management
- virtual memory primitives
- scheduler and CPU topology management
- thread, process, and handle core objects
- IPC and synchronization primitives
- kernel object namespace and capability enforcement
- timekeeping and low-level hardware abstraction

### Privileged kernel modules

Allowed only for measured performance or boot-critical reasons:

- physical and virtual memory backends
- select storage and network fast paths
- architecture-specific low-level platform support

### User-space system services

- driver hosts
- filesystem servers
- network control plane services
- process manager extensions
- security policy engines
- compatibility subsystems
- service manager and session manager

## Placement policy

To prevent the hybrid design from collapsing into an undisciplined monolith, a subsystem may run in kernel space only if it satisfies at least one of the following:

- it is required before the first trusted user-space service can start
- it sits directly on an interrupt-latency-critical path
- it would impose unacceptable context-switch or copying overhead if moved out of kernel space
- it enforces a protection boundary that cannot safely be delegated

Everything else defaults to user space. Any exception should be justified by measurement, not convenience.

## Core abstractions

### Execution model

- CPU
- kernel thread
- user thread
- process
- job or cgroup-like resource domain
- protection domain

### Resource model

- handle-based object access
- capability-tagged rights for sensitive objects
- explicit namespace boundaries
- service endpoints for message-based interaction

### Communication model

- fast local IPC with shared-memory assisted channels
- async and sync message passing
- event objects and wait sets
- brokered cross-domain communication for sandboxed applications

## Portability strategy

The codebase is split into:

- `kernel/arch/` architecture-specific code
- `kernel/platform/` firmware and board integration
- `kernel/` architecture-independent core
- `user/` user-space services and applications

Initial target:

- x86_64 with UEFI boot and SMP

Planned next target:

- ARM64 with UEFI or platform loader support

## Compatibility architecture placement

Compatibility is not implemented inside the core kernel ABI. Instead, VibeOS exposes kernel primitives that allow:

- Linux personality runtimes
- Windows subsystem servers
- macOS-oriented runtime environments
- lightweight virtual machines for unmodified workloads where translation is not viable

This reduces kernel ABI bloat and keeps policy and emulation logic outside ring 0.

## Architectural constraints

The design deliberately avoids:

- exposing foreign kernel ABIs as first-class native kernel interfaces
- allowing user-space subsystems to depend on unstable private kernel structures
- placing full filesystem parsers or broad driver stacks in ring 0 without demonstrated need
- promising complete foreign application parity before subsystem maturity and legal review
