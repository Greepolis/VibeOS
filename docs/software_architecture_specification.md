# Software Architecture Specification

## 0. Scope and Source Inputs

This document consolidates the project-manager-level material already present in the repository into a single software architecture specification suitable for system engineers. It is the primary technical reference for Phase 2 and later implementation work.

Source inputs:

- project vision and goals
- architectural direction and subsystem design notes
- compatibility strategy
- development roadmap and risk register

Primary source documents already available in the repository:

- `docs/vision.md`
- `docs/architecture.md`
- `docs/boot.md`
- `docs/kernel_design.md`
- `docs/memory_management.md`
- `docs/scheduler.md`
- `docs/filesystem.md`
- `docs/security.md`
- `docs/compatibility.md`
- `docs/ipc.md`
- `docs/toolchain.md`
- `docs/testing_strategy.md`

## 1. Architectural Vision

### 1.1 System mission

VibeOS is a new operating system built from scratch in C and C++. It targets modern x86_64 systems first, with a planned ARM64 path, and is designed to combine:

- a security-first execution model
- low-overhead multicore performance
- modular subsystem evolution
- support for Linux, Windows, and selected Darwin or macOS-adjacent software through compatibility subsystems rather than native-kernel ABI cloning

### 1.2 Architecture style selection

| Option | Strengths | Weaknesses | Decision |
|---|---|---|---|
| Monolithic kernel | best raw syscall and in-kernel call performance; easier early bring-up | large trusted computing base; weak fault isolation; poor fit for compatibility-heavy design | rejected |
| Microkernel | strong isolation; restartable services; clean compatibility placement | IPC cost sensitivity; harder early orchestration; more complex fast paths | rejected as too pure for early goals |
| Hybrid microkernel-inspired modular kernel | keeps critical primitives in ring 0 while pushing risky subsystems out of kernel | requires strict governance to avoid boundary erosion | selected |
| Exokernel | maximal flexibility | high complexity for application and runtime authors; poor fit for project scope | rejected |

### 1.3 Selected kernel model

The system shall use a hybrid microkernel-inspired modular kernel with the following rule:

- the kernel core contains only primitives that are unsafe, impossible, or measurably inefficient to host in user space
- drivers, filesystems, policy engines, compatibility runtimes, and high-risk parsers default to user space
- any privileged exception requires architectural review and benchmark evidence

### 1.4 Design drivers

| Driver | Architectural consequence |
|---|---|
| Performance | per-CPU scheduling state, short syscall paths, shared-memory-assisted IPC, minimal global locks |
| Security | handle-based object access, explicit rights masks, service isolation, sandboxing, secure boot compatibility |
| Modularity | versioned subsystem contracts, user-space services, clear repository boundaries |
| Portability | separation of architecture-specific code from kernel core; UEFI-first boot path |
| Compatibility | foreign-OS behavior implemented above the native kernel ABI via runtimes and subsystem services |

## 2. System Context and High-Level Structure

### 2.1 Layered model

```text
Applications
  -> Native VibeOS apps
  -> Linux compatibility domain
  -> Windows compatibility domain
  -> Darwin/macOS-adjacent runtime domain

System services
  -> init
  -> service manager
  -> device manager
  -> filesystem services
  -> network services
  -> security services
  -> compatibility subsystem hosts

Kernel core
  -> scheduler
  -> virtual memory
  -> object model and handles
  -> IPC primitives
  -> interrupt and exception handling
  -> timekeeping

Platform layer
  -> CPU and SMP bring-up
  -> interrupt controllers
  -> timers
  -> firmware tables
  -> bus discovery support
```

### 2.2 Major execution domains

| Domain | Privilege level | Responsibility |
|---|---|---|
| Kernel core | ring 0 | CPU control, memory protection, scheduling, object model, IPC, trap handling |
| Privileged kernel extensions | ring 0 | only boot-critical or performance-justified fast paths |
| Trusted system services | user mode | service manager, policy services, device manager, core storage and network control |
| Compatibility runtimes | user mode | Linux, Windows, and Darwin-oriented translation or guest integration |
| Applications | user mode | native or foreign workloads under policy and sandbox constraints |

### 2.3 Placement policy

A component may execute in kernel space only if at least one of these is true:

- it is required before trusted user-space services exist
- it is on an interrupt-latency-critical path
- moving it out of kernel space causes unacceptable measured overhead
- it enforces a protection boundary that cannot be delegated safely

All other components shall remain in user space.

## 3. Kernel Architecture

### 3.1 Process management

The process model is object-based.

Each process owns:

- one virtual address space
- one handle table
- one security identity and rights context
- one resource-accounting record
- one or more threads

Optional higher-order containment objects, called jobs, group multiple processes for:

- resource accounting
- lifetime control
- sandbox policy application
- compatibility-domain scoping

Process creation modes shall include:

- native image spawn
- fork-like compatibility mode implemented above the native API
- service host spawn
- restricted sandbox spawn

### 3.2 Thread model

The kernel uses a 1:1 kernel-thread model for schedulable entities. User-space runtimes may build additional user-thread or coroutine schedulers, but the kernel is aware only of kernel threads.

Each thread owns:

- CPU register state
- scheduling attributes and affinity mask
- exception state
- thread-local storage metadata
- wait state for IPC, timers, and synchronization objects

### 3.3 Scheduling

The scheduler is preemptive and priority-aware with fair distribution inside each priority class.

Mandatory properties:

- per-CPU run queues
- timer-driven preemption
- migration support for SMP balancing
- wake-up latency optimization for interactive threads
- starvation prevention through aging

Initial classes:

- `realtime` reserved for future controlled use
- `interactive`
- `normal`
- `background`

The first implementation may use round-robin within each priority band, but the contract shall allow later evolution toward weighted fair scheduling and deadline classes.

### 3.4 IPC

IPC is a first-class kernel subsystem because the architecture relies on service isolation.

Mandatory IPC primitives:

- message channels with preserved message boundaries
- shared memory objects for bulk transfer
- events for low-cost signaling
- wait sets for multiplexed blocking

IPC requirements:

- secure handle transfer
- bounded queues with backpressure
- synchronous request-response capability
- asynchronous notification support
- traceable latency and queue metrics

### 3.5 System call interface

The native syscall interface shall be compact, handle-based, and versioned.

Native syscall groups:

- process and thread lifecycle
- address-space and memory-object management
- channel, event, timer, and wait operations
- device and I/O primitives
- trace and diagnostic support
- security token and rights operations

The native syscall ABI shall not mirror Linux, Win32, or Darwin kernel APIs directly. Foreign-OS compatibility must be implemented above the native interface.

## 4. Hardware Abstraction Layer

### 4.1 Scope

The HAL provides the minimum architecture-specific layer required to hide platform differences from the kernel core.

Responsibilities:

- CPU feature discovery
- privileged instruction wrappers
- interrupt controller integration
- timer device integration
- memory-map ingestion from firmware
- SMP bring-up support
- bus and DMA mediation hooks for drivers

### 4.2 CPU management

For x86_64, the HAL shall support:

- bootstrap processor and application processor bring-up
- CPU-local storage and per-CPU structures
- context-switch support
- feature detection for paging, timers, and security capabilities

The design must isolate architecture-dependent code under `kernel/arch/` and `kernel/platform/` to permit ARM64 introduction without rewriting generic schedulers, IPC, or object-management logic.

### 4.3 Interrupt handling

Interrupt flow:

1. low-level trap entry in architecture code
2. interrupt source acknowledgement
3. dispatch to kernel interrupt handling layer
4. wake or schedule affected kernel or user-mode service work

Requirements:

- bounded interrupt-disabled sections
- deferred work outside the hardest interrupt path
- explicit distinction between exceptions, interrupts, and inter-processor interrupts

### 4.4 Hardware compatibility

Phase 1 assumptions:

- UEFI firmware
- x86_64 SMP
- QEMU as the first validation platform
- PCIe-centric discovery path

Later hardware enablement should proceed by adding device classes and platform adapters, not by widening kernel-global assumptions.

## 5. Memory Management

### 5.1 Physical memory management

The physical memory manager shall:

- ingest the firmware memory map
- classify regions into usable, reserved, firmware, device, and special-purpose ranges
- expose page-frame allocation services
- support DMA-constrained pools where required

Recommended first implementation:

- early boot bump allocator
- runtime buddy allocator for general page frames

### 5.2 Virtual memory

The virtual memory subsystem shall support:

- per-process address spaces
- higher-half kernel mapping
- direct physical map for kernel internals
- demand paging
- copy-on-write
- shared memory mappings
- file-backed mappings
- explicit protection changes

### 5.3 Allocators

| Allocator | Scope | Notes |
|---|---|---|
| boot bump allocator | early kernel startup | no free operation required |
| buddy allocator | physical frames | page-granular allocation |
| slab or object caches | kernel fixed-size objects | process, thread, channel, timer, handle table elements |
| general kernel heap | variable-size kernel allocations | non-boot dynamic data |
| user-space allocators | libc/runtime level | built on native memory syscalls |

### 5.4 Protection and hardening

Mandatory protections:

- user/kernel address-space separation
- per-page read, write, execute permissions
- W^X where compatible
- stack guard regions
- ASLR roadmap for kernel and user space

### 5.5 Cache and page-cache management

The system shall distinguish:

- CPU caches, managed indirectly through allocator locality and coherence rules
- kernel object caches for frequent fixed-size allocations
- unified page cache for file-backed data where feasible

The page cache must integrate with:

- filesystem services
- memory-mapped file objects
- invalidation on write-back and truncation

## 6. File System and Storage

### 6.1 VFS

The VFS shall provide:

- path resolution
- mount namespace handling
- file and directory object abstraction
- permission and policy hooks
- cache integration

### 6.2 Native filesystem

The native filesystem, currently referred to as `AuroraFS`, shall target:

- copy-on-write metadata updates
- metadata checksums
- optional data checksums
- extent-based allocation
- snapshot-ready design

Integrity model:

- metadata integrity is mandatory
- crash consistency must be provided by transactional or journaling-equivalent mechanisms

### 6.3 Storage stack

The storage architecture shall separate:

- block-device interfaces
- storage driver hosts
- filesystem services
- cache and memory-object integration

### 6.4 Foreign filesystem support

Planned interoperability path:

- FAT32 or exFAT for boot media and removable storage
- ext4 read-first support
- NTFS read support in later phases
- APFS deferred due to complexity and ecosystem constraints

Foreign filesystem write support requires explicit validation and must not be enabled based only on parser completeness.

## 7. Security Model

### 7.1 Security domains

The security model distinguishes:

- kernel core
- privileged services
- standard services
- compatibility runtimes
- sandboxed applications
- guest-backed workloads

### 7.2 Access control

The baseline mechanism is rights-controlled handle access.

Each handle contains:

- object type
- rights mask
- ownership or provenance metadata as needed

On top of handle rights, the system shall support a mandatory policy layer governing:

- filesystem access
- device access
- network permissions
- process interaction
- compatibility-domain restrictions

### 7.3 Authentication and identity

Early phases require local identities for:

- users
- system services
- sandbox principals

Enterprise identity, network authentication, and federated login are out of scope for the first implementation phases.

### 7.4 Sandboxing

Sandbox policy shall be able to restrict:

- visible filesystem roots
- device classes
- outbound and inbound network access
- IPC peers
- executable memory permissions

Compatibility runtimes must execute inside explicit compatibility domains with separate policy envelopes.

### 7.5 Capability and MAC direction

The preferred model is hybrid:

- capability-like handle rights for concrete object access
- MAC-like policy decisions for cross-cutting rules

This combines explicit kernel object authorization with system-wide policy control.

## 8. Networking Stack

### 8.1 Stack scope

The initial stack supports standard host networking for native and compatibility workloads.

Layer responsibilities:

- link layer integration through drivers
- IP routing and packet processing
- transport protocols
- socket API mediation
- control-plane configuration services

### 8.2 Required protocol support

Phase-ordered support:

- Ethernet
- IPv4
- IPv6
- ARP and neighbor discovery
- ICMP
- UDP
- TCP

Later candidates:

- firewalling and traffic policy
- TLS assist hooks
- QUIC-friendly APIs
- virtual switching and namespace-aware networking

### 8.3 Socket model

The native socket model shall support:

- blocking and non-blocking operation
- event-driven readiness
- compatibility translation for POSIX and Winsock semantics
- brokered raw-socket access for privileged use only

## 9. Driver Architecture

### 9.1 Driver model

Default rule: drivers run in user-space driver hosts.

Exception rule: a driver may run in privileged space only if it is:

- required before user-space services are available
- in a path that cannot tolerate user-space round-trips
- too tightly coupled to interrupt-time mechanisms

### 9.2 Driver manager

The device manager service shall handle:

- device discovery
- driver matching
- driver host spawning
- lifecycle supervision
- crash and restart policy

### 9.3 Dynamic loading and safety

Driver loading must be:

- explicit and policy-controlled
- version-checked against published driver-service contracts
- restartable for user-space hosted drivers

Direct unrestricted DMA or MMIO access must not be handed to drivers without mediation and auditing support.

## 10. User Space

### 10.1 Core services

Mandatory early services:

- `init`
- `servicemgr`
- `devmgr`
- filesystem service
- network service
- security policy service
- logging or tracing collector

### 10.2 System libraries and APIs

The user-space API stack shall include:

- a native C runtime
- a system library exposing the native kernel API
- optional C++ support for higher-level services

The native API is the only stable first-party contract. POSIX-like, Win32-like, or Darwin-like behavior belongs in compatibility layers.

### 10.3 Runtime environments

Supported runtime categories:

- native VibeOS runtime
- Linux compatibility runtime
- Windows subsystem runtime
- Darwin-oriented compatibility or guest-assist runtime

Compatibility runtimes must be separately versioned and testable.

## 11. Boot Process

### 11.1 Boot sequence

```text
UEFI
  -> bootloader
  -> kernel image load
  -> early memory bootstrap
  -> trap and timer setup
  -> virtual memory enablement
  -> SMP startup
  -> init process
  -> service manager
  -> core services
  -> compatibility services
```

### 11.2 Bootloader responsibilities

The bootloader shall:

- load the kernel image
- provide a structured boot information block
- pass memory map and firmware tables
- hand over framebuffer or console information
- optionally verify images in secure boot configurations

### 11.3 Kernel initialization phases

1. early architecture setup
2. early allocation and memory-map parsing
3. interrupt and exception descriptor setup
4. scheduler bootstrap
5. SMP processor bring-up
6. transition to full allocators
7. launch first user-space process

## 12. Observability

### 12.1 Logging

Mandatory early logging channels:

- serial console
- in-memory kernel log buffer

Later channels:

- structured event stream
- persistent diagnostic storage

### 12.2 Tracing

The system shall provide tracepoints for:

- scheduler events
- IPC latency
- page faults and mapping changes
- driver and service failures
- compatibility runtime translation faults

### 12.3 Debugging

Required debugging capabilities:

- symbolized panic output in debug builds
- emulator-friendly debug attachment
- kernel assertions
- crash signature capture

## 13. Scalability and Performance

### 13.1 Multicore strategy

The architecture targets SMP from the beginning.

Mandatory properties:

- per-CPU run queues
- per-CPU statistics and local data where beneficial
- minimal global scheduler locks
- inter-processor interrupt support for wake-ups and TLB shootdowns

### 13.2 Optimization strategy

The performance plan prioritizes:

- short syscall paths
- shared-memory-assisted IPC
- lock partitioning
- reduced-copy I/O for large transfers
- locality-aware allocation

### 13.3 Scalability envelope

The first implementation targets developer and workstation-class systems. The architecture should remain extensible toward:

- higher core counts
- NUMA-aware scheduling and allocation
- container-heavy workloads
- mixed native and compatibility-domain concurrency

## 14. Modularity and Extensibility

### 14.1 Module boundaries

Subsystem boundaries are defined by:

- stable kernel-internal interfaces for core modules
- versioned IPC contracts for user-space services
- explicit compatibility-runtime interfaces

### 14.2 Plugin and module policy

VibeOS is extensible, but unrestricted in-kernel plugin models are not a design goal.

Preferred extension mechanisms:

- user-space services
- driver hosts
- compatibility runtime modules
- versioned user-space libraries

Kernel modules are permitted only under the placement policy defined earlier.

### 14.3 API and ABI versioning

Versioning rules:

- native kernel ABI is versioned
- service protocols are versioned independently
- compatibility runtimes track their own supported foreign ABI surface
- breaking changes require compatibility windows or migration tooling

## 15. Architecture Decisions Requiring Ongoing Validation

The following decisions are accepted for now but must be revalidated during implementation:

- whether specific storage or network fast paths remain out of kernel space
- whether the first native filesystem uses transactional metadata, journaling, or a hybrid integrity model
- how much Linux compatibility can be implemented through translation before requiring guest-backed execution for practicality
- whether selected Windows subsystem pieces need privileged acceleration

## 16. Open Questions / Missing Requirements

- What is the primary product target for the first usable release: developer workstation, headless server, embedded appliance, or research platform?
- Is a GUI stack required before the first public milestone, or is a command-line-only release acceptable?
- What security posture is mandatory for version 1: secure boot optional, recommended, or required?
- Is disk encryption in scope for the first storage milestone?
- What hardware profile defines the minimum supported machine: RAM floor, CPU feature floor, storage type, and network assumptions?
- Is realtime scheduling a firm requirement or only a future extensibility goal?
- What degree of POSIX conformance is required for Linux compatibility milestones?
- What classes of Windows software matter first: console tools, services, GUI applications, or specific enterprise workloads?
- Is any level of unmodified macOS application support actually required, or is Darwin-adjacent developer-tool portability sufficient?
- What legal and distribution constraints will govern compatibility work, especially for macOS-adjacent behavior and foreign binary testing?
- Is hypervisor support required in the base OS, or is lightweight guest execution allowed to depend on a later subsystem?
- What persistence and recovery guarantees are required for the first native filesystem milestone?
- Are hot-plug, suspend/resume, and power-management features required in the initial hardware support envelope?
- Which observability outputs are mandatory in CI: serial logs only, trace artifacts, crash dumps, or performance counters?
- What formal stability promise, if any, should be made for the native syscall ABI before version 1?
