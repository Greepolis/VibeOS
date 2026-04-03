# ADR-0002: Platform Bring-Up Baseline

## Status

Accepted

## Context

The project needs a constrained initial target to reduce bring-up complexity and enable fast iteration in emulator-based workflows.

## Decision

The initial platform baseline is:

- x86_64
- UEFI firmware
- SMP enabled from the first bring-up architecture
- QEMU as the primary validation environment

The project-owned bootloader remains the long-term target, but early bring-up may temporarily use a standards-compliant boot handoff if it accelerates kernel validation without importing foreign kernel code.

## Consequences

Positive:

- faster path to early kernel execution
- reproducible development environment
- direct alignment with CI and emulator-driven testing

Negative:

- legacy BIOS is deferred
- ARM64 work is deferred to a later portability phase
- some real hardware issues may appear after emulator success

## Implementation notes

- boot information handoff must be versioned from the start
- early serial output is mandatory
- the first hardware profile should remain intentionally narrow: timer, interrupt controller, framebuffer or serial, PCIe discovery, and one storage path
