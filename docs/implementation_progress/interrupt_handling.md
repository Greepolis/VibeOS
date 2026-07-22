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
- Resumable-debug-trap handling: `#DB`(1)/`#BP`(3)/`#OF`(4) now yield `CONTINUE` instead of a kernel `PANIC` (covered by `test_trap_debug_resumable`).
- **Real on-metal ISR handler routes CPU exceptions through `vibeos_trap_dispatch_ex`** using a bring-up-local trap_state, honoring the returned action (CONTINUE resumes via `iretq`; PANIC/KILL halt during bring-up). Verified in QEMU: real `int3` -> dispatch -> `action=CONTINUE count=1` -> resume.
- Timer IRQ binding path from interrupt controller to timer source (host model).
- **Real hardware timer IRQ on metal** (`arch_hw.c`): 8259 PIC remapped to vectors 0x20-0x2F, PIT programmed to 100 Hz, IRQ ISR stubs 32-47, EOI handling, `sti`, and a tick counter incremented from IRQ0. Verified in QEMU: `TIMER_IRQ_OK ticks=3` (bounded spin confirms periodic delivery), boot continues to CLI with interrupts enabled.

## Pending
- Point the on-metal handler at the live kernel `trap_state` and current PID (currently a bring-up-local trap_state) so KILL_CURRENT can terminate a real process.
- Drive the scheduler from the real timer tick (preemption) instead of the host-modeled timer path.
- APIC / IO-APIC support (currently legacy 8259 PIC; only IRQ0 unmasked).
- SMP interrupt routing policy.

## Next checkpoint
- Wire the timer tick into `vibeos_sched_*` so on-metal preemption is exercised in QEMU.
- Validate user-fault kill vs kernel panic in QEMU once ring-3 user mode exists.
