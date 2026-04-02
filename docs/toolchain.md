# Toolchain and Build Strategy

## Goals

- deterministic and reproducible builds
- a clear split between host tools and target artifacts
- support for kernel-grade debugging and tracing
- a sustainable language strategy for C and C++

## Language split

The project is primarily written in C and C++ with a deliberate role split:

- C for low-level kernel entry, architecture glue, boot interfaces, and areas requiring minimal runtime assumptions
- C++ for higher-level kernel subsystems and user-space services where stronger type modeling improves maintainability

Rules:

- avoid exceptions and RTTI in kernel space unless a future explicit decision reverses this
- minimize dynamic allocation during early boot
- keep the freestanding kernel environment separate from hosted user-space components

## Compiler and linker direction

Initial recommended baseline:

- Clang or LLVM as primary compiler family
- LLD as preferred linker
- GCC compatibility kept as a secondary portability target when feasible

Rationale:

- strong freestanding support
- good diagnostics
- sanitizer and control-flow hardening ecosystem for user-space components
- solid cross-compilation workflow

## Build system direction

Recommended initial build strategy:

- CMake or Meson for host orchestration and developer ergonomics
- Ninja as the default local executor
- explicit target separation for bootloader, kernel, userland, image assembly, and tests

The build system should produce:

- kernel ELF or PE image as required by the boot path
- bootable disk image
- symbol files for debugging
- emulator launch targets

## Cross-compilation model

- host builds run on Linux, macOS, or Windows development machines
- target triples are explicit and never inferred from the host
- freestanding kernel and hosted user-space builds use separate compiler flags and runtime assumptions

## Debug and observability tooling

- QEMU as the first emulator target
- GDB and LLDB support from the start
- serial logging as the first diagnostic channel
- symbolized crash output for kernel panics in debug builds

## Build variants

- `debug` for bring-up and diagnostics
- `release` for performance profiling
- `hardened` for security validation once the base system matures

## Early deliverables for phase 2

- one-command local build
- one-command QEMU boot path
- symbol-aware kernel debug session
- image packaging pipeline with reproducible inputs
