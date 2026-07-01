# Interrupt Handling Progress

Status: In Progress
Last review: 2026-07-01

## Implemented
- Interrupt controller registration/dispatch primitives in `kernel/core/interrupts.c`.
- IRQ diagnostics counters and mask/unmask controls.
- Denied-dispatch diagnostics with explicit cause counters (`bad_irq`, `unhandled`, `masked`, `disabled`) and counter reset API.
- x86_64 IDT scaffolding in `kernel/arch/x86_64/idt.c`.
- Trap state and classification flow in `kernel/arch/x86_64/trap.c`.
- Extended trap decision flow with explicit actions (`CONTINUE`, `KILL_CURRENT`, `PANIC`), user/kernel CPL classification, fault-address tracking, action counters, and kernel-log integration for directed fault diagnostics.
- Timer IRQ binding path from interrupt controller to timer source.

## Pending
- Full architecture-specific exception vector coverage.
- Runtime assembly/IDT handoff for page fault and general protection fault frames, including CR2 capture on real hardware/QEMU faults.
- SMP interrupt routing policy and APIC-level refinement.

## Next checkpoint
- Wire real x86_64 exception stubs into `vibeos_trap_dispatch_ex` and validate user fault kill vs kernel panic in QEMU.
