# Milestones

## Current Snapshot (2026-08-02)

| Milestone | Status | Evidence |
| --- | --- | --- |
| M0 project charter | Completed | Phase 1 documents |
| M1 toolchain and build | Completed | gcc/clang x Debug/Release matrix, green on every push |
| M2 boot to kernel banner | Completed | UEFI boot gate under OVMF, required in CI |
| M3 memory and interrupts | Completed | paging, traps, PIT and APIC timer, all boot-verified |
| M4 scheduler and multitasking | Completed | preemptive SMP across all cores, per-process address spaces |
| M5 first user-space service | Completed | ring-3 programs with fork/exec/wait |
| M6 storage and shell | Completed | virtio-blk, FAT read and write, serial shell |
| M7 networked native system | Completed | TCP/IP over virtio-net, TCP round trip to a host server in CI |
| M8 Linux CLI compatibility | In progress | Linux ABI served in ring 3; startup ABI (stack, auxv, TLS) verified. Remaining: the syscalls a real static binary needs after startup |
| M9 Windows console compatibility | Not started | - |
| M10 architecture review | Ongoing | design documents updated alongside the code |

Status here reflects what boots and passes a gate, not what has been designed.
The April assessment (`docs/project_status_assessment_2026-04-03.md`) predates
most of this work and is kept as a historical record rather than a current
view.

## M0: project charter complete

- all Phase 1 documents created
- repository structure agreed
- architectural choice documented

## M1: toolchain and build skeleton

- cross compiler selection
- build scripts
- image packaging
- emulator launch flow
- first automated boot smoke test

## M2: boot to kernel banner

- bootloader loads kernel
- serial output works
- early memory map parsed

## M3: memory and interrupts online

- IDT or equivalent configured
- paging stable
- physical allocator functional

## M4: scheduler and multitasking

- threads run and preempt
- timer tick or event scheduling works
- SMP bootstrap on x86_64

## M5: first user-space service

- init process starts
- IPC path usable
- service restart path proven
- service manager and device manager boundaries validated

## M6: storage and shell

- filesystem mounted
- shell executes native commands
- file I/O stable

## M7: networked native system

- TCP/IP available
- remote diagnostics or package ingress possible

## M8: Linux CLI compatibility demo

- shell utilities or a small Linux user program run under compatibility mode

## M9: Windows console compatibility demo

- basic Windows user-space executable runs in controlled scope

## M10: architecture review for expansion

- reassess kernel and service boundaries
- validate security posture
- review risk register and compatibility scope
- decide ARM64 and GUI next steps
