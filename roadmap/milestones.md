# Milestones

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
