# Vision

## Mission

VibeOS aims to be a modern operating system built from scratch in C and C++ for production-grade research, advanced systems experimentation, and eventual real-world deployment. The system is designed to provide strong performance, principled security, and a compatibility strategy that can host applications originating from Linux, Windows, and macOS.

## Design principles

### 1. Security first

Security is a first-class architectural property, not an add-on. The system is designed around least privilege, strong isolation, memory protection, capability-aware resource access, and explicit trust boundaries between kernel, services, drivers, and applications.

### 2. Performance without hidden complexity

Fast paths must stay simple, measurable, and observable. Kernel transitions, scheduling, IPC, memory management, and I/O paths should be designed with low overhead and scalability on many-core systems.

### 3. Modular evolution

The OS must support distributed development by allowing major subsystems to evolve independently behind stable interfaces. This includes driver hosting, filesystem services, compatibility runtimes, networking, and userland service layers.

### 4. Compatibility as architecture

Running third-party software from Linux, Windows, and macOS must influence core design decisions from day one. The OS must expose the right primitives for compatibility layers, ABI shims, containerization, and lightweight virtualization.

### 5. Portability by construction

The architecture should separate machine-independent kernel code from platform-specific code. Initial implementation targets x86_64, but interfaces should anticipate future ARM64 support.

## Product goals

### Performance goals

- low-latency scheduling and interrupt handling
- low-overhead system call and IPC paths
- scalable multicore execution with minimized global contention
- NUMA-aware evolution path for future hardware targets
- efficient page fault handling and virtual memory operations

### Security goals

- strong process and service isolation
- explicit privilege domains
- memory-safe subsystem boundaries where practical through language and interface discipline
- hardened kernel build and runtime protections
- sandboxing for untrusted applications and compatibility runtimes

### Compatibility goals

- Linux application support as the first compatibility priority
- Windows user-mode compatibility as the second major priority
- macOS compatibility pursued selectively through runtime, API translation, and managed execution strategies rather than full binary parity in early phases
- architecture support for containers, para-virtualization, and restricted guest environments

## Non-goals for early phases

- immediate support for all hardware classes
- complete POSIX, Win32, or Darwin kernel parity in the first milestones
- broad consumer desktop readiness before core kernel reliability
- unrestricted kernel extension ABI stability before internal interfaces mature

## Success criteria for the project foundation

Phase 1 is successful when the project has:

- a clear architectural stance
- documented subsystem boundaries
- repository organization suitable for long-term work
- milestone sequencing that de-risks the kernel bring-up path
- a compatibility strategy that does not compromise kernel integrity
