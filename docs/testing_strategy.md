# Testing Strategy

## Goals

- catch regressions before they reach low-level integration work
- validate contracts between kernel and services
- measure compatibility progress with repeatable suites
- keep bring-up fast enough for daily development

See also:

- `docs/test_automation_spec.md`
- `docs/test_feedback_profiles.md`

## Test layers

### Unit tests

For pure logic that can run on the host:

- allocators where host execution is possible
- parser code
- protocol encoders and decoders
- compatibility translation helpers

### Kernel subsystem tests

For logic requiring the target environment:

- paging and address-space operations
- scheduler invariants
- IPC semantics
- timer behavior
- handle-rights enforcement

### Service integration tests

- init and service manager lifecycle
- driver host restart behavior
- filesystem service contracts
- network stack control-plane flows

### System boot tests

- boot image loads in emulator
- kernel reaches known checkpoints
- basic user-space services start in order

### Compatibility tests

- Linux syscall and libc behavior subsets
- Windows process, file, and console behavior subsets
- selected Darwin and POSIX semantic checks where applicable

## Execution environments

- host-native tests for portable libraries
- QEMU for deterministic bring-up and CI
- optional hardware validation later for timing and device realism

## Release gates

No milestone should be considered complete without automated validation for its core claims. In early phases this means emulator-based smoke tests at minimum; later phases require subsystem-specific regression suites.

## Required early artifacts

- test harness runner
- boot smoke test
- serial log assertions
- golden compatibility samples for Linux-first milestones

## Debugging expectations

- failures should capture build variant, git revision, emulator configuration, and log excerpts
- kernel failures should preserve a structured panic signature where practical
- compatibility failures should distinguish loader, translation, and runtime-service faults
