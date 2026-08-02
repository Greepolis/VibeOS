# Compatibility Strategy

## Objective

VibeOS must support applications from Linux, Windows, and macOS without compromising kernel integrity or forcing the native kernel ABI to mimic every foreign operating system.

## Strategy overview

The compatibility approach is layered:

- native VibeOS ABI for first-party software
- subsystem runtimes for translated execution
- containers and personalities for compatible environments
- lightweight virtualization for workloads requiring stronger fidelity

## Support tiers

Compatibility promises are intentionally tiered:

- Tier 0: native VibeOS applications using the native ABI
- Tier 1: Linux command-line and service workloads through translation
- Tier 2: Linux containerized workloads with stronger environment fidelity
- Tier 3: Windows console and service-style workloads through subsystem services
- Tier 4: selected Windows GUI workloads once graphics and input stacks mature
- Tier 5: selective Darwin-oriented and macOS-adjacent runtime experiments, not broad parity

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

### What exists today

Ahead of the user-space personality, the x86-64 port serves the Linux ABI
directly from the kernel, which is what makes it possible to run real programs
now rather than after the full stack is built:

- static `ET_EXEC` ELF64 images are loaded by a portable, host-tested parser;
  `ET_DYN` and anything needing an interpreter are refused clearly rather than
  loaded and left to fault at a puzzling address
- programs start on a real System V stack with `argc`, `argv`, `envp` and an
  auxiliary vector, so a C runtime can find the program headers it was loaded
  from
- thread-local storage works through `arch_prctl`, per task and preserved
  across context switches
- the syscall surface is listed in [syscalls.md](syscalls.md)

What is still missing for an unmodified static binary is not the startup
sequence but what comes after it: `fstat`, `openat`, `readlinkat`, `getcwd`,
signal delivery, and threads with real futexes. Stubbing the full Linux table
is explicitly not the plan - a program that receives a fabricated success dies
later and less legibly than one that receives `-ENOSYS`.

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

### Legal and ecosystem constraint

macOS compatibility must be evaluated not only as a technical problem but also as an ecosystem and legal one. The project should treat guest-backed execution, POSIX compatibility, and developer-tool portability as the realistic early focus rather than broad claims of native macOS application support.

## Shared enabling technologies

- unified object and handle translation layer
- filesystem path semantics adapters
- signal or exception translation
- environment and process image loaders
- graphics and input broker services
- sandboxed compatibility domains

## Current runtime prototype status

- compatibility runtime state machine is implemented for Linux, Windows, and macOS targets (`enable`, `translate`, `stats`).
- initial syscall translation probes are available per target to exercise end-to-end ABI bridging paths in host tests.
- translation statistics expose translated vs denied syscall counts for observability and safety gating.

## Recommended compatibility rollout

1. Native toolchain and minimal userland
2. Linux userspace and CLI compatibility
3. Linux container runtime
4. Windows basic console and service-process compatibility
5. Windows GUI-oriented expansion
6. Selective Darwin and macOS-adjacent runtime experiments
