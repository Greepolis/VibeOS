# Compatibility Strategy

## Objective

VibeOS must support applications from Linux, Windows, and macOS without compromising kernel integrity or forcing the native kernel ABI to mimic every foreign operating system.

## Strategy overview

The compatibility approach is layered:

- native VibeOS ABI for first-party software
- subsystem runtimes for translated execution
- containers and personalities for compatible environments
- lightweight virtualization for workloads requiring stronger fidelity

## Linux compatibility

### Priority

Linux is the first compatibility target because:

- it unlocks developer tooling quickly
- POSIX ecosystems are large and well documented
- container-oriented workflows align well with the system design

### Proposed approach

- Linux personality runtime in user space
- syscall translation to native kernel objects and services
- ELF loader support
- POSIX-compatible libc strategy in the compatibility environment
- container and namespace model for isolating Linux workloads

### Fallback

For software requiring tight kernel semantic fidelity, provide lightweight VM execution with shared filesystem and graphics bridges.

## Windows compatibility

### Proposed approach

- Windows subsystem service implementing Win32 and NT-oriented user-mode behaviors incrementally
- PE loader
- registry-like service abstraction
- object and handle semantic translation where possible
- graphics and multimedia support deferred after core process and file APIs

### Key challenge

Windows compatibility requires significant semantic mapping around process creation, handles, synchronization, and filesystem expectations. This should remain outside the kernel core.

## macOS compatibility

### Proposed approach

macOS compatibility is the most constrained and should be phased carefully:

- Mach-like messaging semantics can be emulated in user space where needed
- selected Unix and userland tooling may run via POSIX compatibility
- native app compatibility may require a managed runtime, translation environment, or guest-backed execution depending on legal and technical constraints

### Policy

Early phases should not promise broad unmodified macOS binary support. The architecture should remain open to targeted support without overcommitting the kernel.

## Shared enabling technologies

- unified object and handle translation layer
- filesystem path semantics adapters
- signal or exception translation
- environment and process image loaders
- graphics and input broker services
- sandboxed compatibility domains

## Recommended compatibility rollout

1. Native toolchain and minimal userland
2. Linux userspace and CLI compatibility
3. Linux container runtime
4. Windows basic console and service-process compatibility
5. Windows GUI-oriented expansion
6. Selective macOS-compatible runtime experiments
