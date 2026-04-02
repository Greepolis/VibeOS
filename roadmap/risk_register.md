# Risk Register

## R1: hybrid kernel boundary erosion

Risk:

The project may keep moving subsystems into kernel space for convenience, undermining isolation and maintainability.

Mitigation:

- require documented justification for privileged placement
- benchmark before and after any boundary change
- review boundary exceptions in architecture reviews

## R2: IPC overhead harms the service-oriented model

Risk:

Poor IPC design could make user-space drivers, filesystems, and compatibility services too expensive.

Mitigation:

- design IPC early as a first-class subsystem
- measure latency and copy counts from initial bring-up
- use shared memory and batching for bulk traffic

## R3: compatibility scope expands faster than the core OS matures

Risk:

Linux, Windows, and macOS support could become a source of uncontrolled scope growth.

Mitigation:

- use tiered compatibility commitments
- prioritize Linux first
- isolate foreign behavior in runtimes and guest paths rather than in the native kernel ABI

## R4: boot and hardware bring-up complexity delays progress

Risk:

Early platform work can absorb too much time before the kernel core becomes testable.

Mitigation:

- constrain Phase 2 and Phase 3 to x86_64 plus UEFI and QEMU first
- keep the initial device set intentionally narrow
- treat custom bootloader maturity as staged rather than all-or-nothing

## R5: security model remains aspirational instead of enforced

Risk:

Capability language and sandboxing goals could stay descriptive without becoming concrete implementation rules.

Mitigation:

- define handle-rights and policy hooks before large user-space expansion
- add security-focused tests to milestone gates
- review every new subsystem for privilege boundary assumptions
