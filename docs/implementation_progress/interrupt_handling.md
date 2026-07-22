# Interrupt Handling Progress

Status: In Progress (real CPU exception handling verified in QEMU)
Last review: 2026-07-22

## Implemented
- Interrupt controller registration/dispatch primitives in `kernel/core/interrupts.c`.
- IRQ diagnostics counters and mask/unmask controls.
- Denied-dispatch diagnostics with explicit cause counters (`bad_irq`, `unhandled`, `masked`, `disabled`) and counter reset API.
- x86_64 IDT scaffolding (host model) in `kernel/arch/x86_64/idt.c`.
- **Real on-metal GDT + IDT bring-up in `kernel/arch/x86_64/arch_hw.c` + `isr.S`** (image-only, not host tests):
  - Minimal flat GDT (kernel CS=0x08, DS=0x10) with `lgdt` and far-return CS reload.
  - 256-entry IDT with 64-bit interrupt gates, `lidt`, and assembly ISR stubs for CPU exceptions 0-31.
  - Uniform saved-register frame passed to a C handler; CR2 capture on #PF (vector 14).
  - Invoked from `entry.s` before `vibeos_kmain`, preserving RDI/RSI handoff registers.
  - Verified in QEMU+OVMF: `int3` self-test raises a real #BP, is caught by the ISR (vector=3, rip in kernel .text), resumes via `iretq`, and boot continues to CLI (`HW_INIT_OK`).
- Trap state and classification flow in `kernel/arch/x86_64/trap.c`.
- Extended trap decision flow with explicit actions (`CONTINUE`, `KILL_CURRENT`, `PANIC`), user/kernel CPL classification, fault-address tracking, action counters, and kernel-log integration for directed fault diagnostics.
- Timer IRQ binding path from interrupt controller to timer source.

## Pending
- Wire the real ISR C handler into `vibeos_kernel_dispatch_trap` / `vibeos_trap_dispatch_ex` so on-metal faults drive the existing decision model (kill-current vs panic).
- PIC/APIC init to enable hardware IRQ delivery (currently only CPU exceptions are handled; external IRQs not yet unmasked).
- SMP interrupt routing policy and APIC-level refinement.

## Next checkpoint
- Route real #PF/#GP frames through `vibeos_trap_dispatch_ex` and validate user-fault kill vs kernel panic in QEMU (needs the trap_state available to the handler and a deliberate faulting probe).
