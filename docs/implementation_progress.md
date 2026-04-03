# Kernel Implementation Progress

## Implementation Plan

### Technical roadmap

1. Build and test baseline (`P0`)
- `CMake` build graph for kernel core modules and host-side tests
- deterministic test runner with structured artifact output

2. Kernel bootstrap contracts (`P0`)
- boot information contract validation in kernel entry
- early memory initialization and scheduler bootstrap

3. Core primitive subsystems (`P1`)
- page-frame allocator primitive
- per-CPU-ready scheduler skeleton
- event and bounded message channel IPC primitives

4. Transition hardening (`P1` and `P2`)
- richer VM abstraction
- interrupt and timer model expansion
- first user-space init launch path

### Order of development and dependencies

- `bootloader` -> provides `boot_info` contract consumed by `kernel_core`
- `kernel_core` depends on `memory_manager`, `scheduler`, `ipc`
- `system_call_interface` depends on stable `kernel_core` object model
- `init_system` depends on `kernel_core`, `virtual_memory`, `scheduler`, `ipc`
- `driver_framework` and `filesystem_layer` depend on IPC and process model
- `networking_stack` depends on scheduler, memory manager, driver framework

### Critical kernel areas for early bring-up

- boot contract validation
- memory allocator correctness
- scheduler queue invariants
- IPC queue bounds and signal semantics

## Module Breakdown

### Bootloader
- Responsibilities: kernel image load and boot information handoff
- Main files: `boot/` currently placeholder, shared contract in `include/vibeos/boot.h`
- Public interfaces: `vibeos_boot_info_t`
- Dependencies: firmware and kernel entry contract

### Kernel Core
- Responsibilities: early init orchestration and subsystem bootstrap
- Main files: `kernel/core/kmain.c`, `include/vibeos/kernel.h`
- Public interfaces: `vibeos_kmain`, `vibeos_kernel_t`
- Dependencies: memory manager, scheduler, IPC

### Process Scheduler
- Responsibilities: runqueue management and dispatch primitive
- Main files: `kernel/sched/scheduler.c`, `include/vibeos/scheduler.h`
- Public interfaces: `vibeos_sched_init`, `vibeos_sched_enqueue`, `vibeos_sched_next`
- Dependencies: thread model and CPU topology info

### Memory Manager
- Responsibilities: page-frame allocator bootstrap
- Main files: `kernel/mm/pmm.c`, `include/vibeos/mm.h`
- Public interfaces: `vibeos_pmm_init`, `vibeos_pmm_alloc_page`
- Dependencies: boot memory map

### Virtual Memory
- Responsibilities: address space and mapping policy
- Main files: `kernel/mm/vm.c`, `include/vibeos/vm.h`
- Public interfaces: `vibeos_vm_init`, `vibeos_vm_map`, `vibeos_vm_unmap`, `vibeos_vm_protect`, `vibeos_vm_lookup`
- Dependencies: memory manager, interrupt handling

### Interrupt Handling
- Responsibilities: trap/interrupt entry and dispatch
- Main files: `kernel/core/interrupts.c`, `include/vibeos/interrupts.h`
- Public interfaces: `vibeos_intc_init`, `vibeos_intc_register`, `vibeos_intc_dispatch`
- Dependencies: HAL, scheduler, timer

### System Call Interface
- Responsibilities: user-kernel boundary ABI
- Main files: `kernel/core/syscall.c`, `include/vibeos/syscall.h`
- Public interfaces: `vibeos_syscall_dispatch`
- Dependencies: kernel object model, IPC, VM

### IPC Subsystem
- Responsibilities: event signaling and channel messaging
- Main files: `kernel/ipc/event.c`, `kernel/ipc/channel.c`, `include/vibeos/ipc.h`
- Public interfaces: `vibeos_event_*`, `vibeos_channel_*`
- Dependencies: scheduler, handle model (future)

### Driver Framework
- Responsibilities: user-space-first driver hosting
- Main files: pending under `user/drivers/` and `user/devmgr/`
- Public interfaces: pending
- Dependencies: IPC, security policy, device manager

### Filesystem Layer
- Responsibilities: VFS contract and filesystem service boundaries
- Main files: `user/fs/vfs_service.c`, `include/vibeos/services.h`
- Public interfaces: `vibeos_vfs_start`
- Dependencies: IPC, memory objects, driver framework

### Networking Stack
- Responsibilities: socket and protocol layers
- Main files: `user/net/network_service.c`, `include/vibeos/services.h`
- Public interfaces: `vibeos_net_start`
- Dependencies: scheduler, memory manager, drivers

### User Space Interface
- Responsibilities: native runtime and system library boundary
- Main files: pending under `user/lib/`
- Public interfaces: pending
- Dependencies: syscall interface

### Init System
- Responsibilities: first user-space task and service orchestration
- Main files: `user/init/init_system.c`, `include/vibeos/services.h`
- Public interfaces: `vibeos_init_start`
- Dependencies: process model, IPC, VM

## Directory Structure (implemented modules)

```text
include/vibeos/
  boot.h
  kernel.h
  mm.h
  scheduler.h
  ipc.h

kernel/
  core/kmain.c
  core/interrupts.c
  core/syscall.c
  mm/pmm.c
  mm/vm.c
  sched/scheduler.c
  ipc/event.c
  ipc/channel.c
  arch/x86_64/boot_stub.c

user/
  init/init_system.c
  devmgr/device_manager.c
  fs/vfs_service.c
  net/network_service.c

tests/kernel/
  kernel_tests.c

scripts/
  run-tests.ps1
```

## How to test

1. Configure and build:
   `cmake -S . -B build -DVIBEOS_BUILD_TESTS=ON`
2. Build artifacts:
   `cmake --build build`
3. Run tests:
   `.\build\vibeos_kernel_tests.exe`
4. Run automated feedback profile:
   `.\scripts\run-tests.ps1 -Profile agent`

The automated run writes:

- `artifacts/kernel-tests.log`
- `artifacts/test-summary.json`

## Development Log

Module: Build and Test Baseline
Status: Completed
Implemented:
- root `CMakeLists.txt` with modular kernel-core targets
- host-side kernel test executable
- test runner script with machine-readable summary output
Files Created/Modified:
- `CMakeLists.txt`
- `tests/kernel/kernel_tests.c`
- `scripts/run-tests.ps1`
- `.gitignore`
Problems:
- no freestanding cross-toolchain configured yet, so first execution path uses host simulation
- local environment currently reports `INFRA_CMAKE_MISSING`; test harness correctly classifies it as `infra_error` in `artifacts/test-summary.json`

Module: Kernel Core Bootstrap
Status: Partial
Implemented:
- `vibeos_kmain` entry sequence with boot contract checks
- boot-stage state transitions and initialization event signaling
Files Created/Modified:
- `kernel/core/kmain.c`
- `include/vibeos/kernel.h`
- `include/vibeos/boot.h`
Pending:
- real UEFI bootloader integration
- early interrupt/timer initialization

Module: Memory Manager
Status: Partial
Implemented:
- bootstrap page-frame bump allocator
Files Created/Modified:
- `kernel/mm/pmm.c`
- `include/vibeos/mm.h`
Pending:
- runtime buddy allocator policy
- reserved-region filtering and mapping policies

Module: Process Scheduler
Status: Partial
Implemented:
- per-CPU-ready runqueue structure
- enqueue and dequeue primitives
Files Created/Modified:
- `kernel/sched/scheduler.c`
- `include/vibeos/scheduler.h`
Pending:
- preemption timers
- priority aging and balancing policies

Module: IPC Subsystem
Status: Partial
Implemented:
- event primitive
- bounded message channel primitive
Files Created/Modified:
- `kernel/ipc/event.c`
- `kernel/ipc/channel.c`
- `include/vibeos/ipc.h`
Pending:
- handle transfer semantics
- wait sets and timeout policies

Module: Virtual Memory
Status: Partial
Implemented:
- address-space object initialization
- map, unmap, protect, and lookup primitives
Files Created/Modified:
- `kernel/mm/vm.c`
- `include/vibeos/vm.h`
Pending:
- page-table backend integration
- copy-on-write fault path

Module: Interrupt Handling
Status: Partial
Implemented:
- interrupt controller registration and dispatch primitives
- per-IRQ counters for diagnostics
Files Created/Modified:
- `kernel/core/interrupts.c`
- `include/vibeos/interrupts.h`
Pending:
- architecture-specific trap/IDT integration
- timer IRQ source wiring

Module: System Call Interface
Status: Partial
Implemented:
- syscall frame model
- dispatcher with initial syscall IDs
Files Created/Modified:
- `kernel/core/syscall.c`
- `include/vibeos/syscall.h`
Pending:
- process/thread syscall groups
- handle-based object syscall surface

Module: User Space Services
Status: Partial
Implemented:
- init service stub
- device manager stub
- filesystem service stub
- networking service stub
Files Created/Modified:
- `user/init/init_system.c`
- `user/devmgr/device_manager.c`
- `user/fs/vfs_service.c`
- `user/net/network_service.c`
- `include/vibeos/services.h`
Pending:
- service manager process supervision
- real IPC contracts between services

## System Implementation Status

| Component | Status | Notes |
| --- | --- | --- |
| Bootloader | Pending | only boot contract type is defined |
| Kernel Core | In Progress | `kmain` bootstrap logic implemented |
| Process Scheduler | In Progress | queue primitives ready, no timer preemption yet |
| Memory Manager | In Progress | bump allocator implemented |
| Virtual Memory | In Progress | address-space mapping primitives implemented |
| Interrupt Handling | In Progress | interrupt controller dispatch primitives implemented |
| System Call Interface | In Progress | minimal syscall dispatcher implemented |
| IPC Subsystem | In Progress | event + channel primitives implemented |
| Driver Framework | Pending | user-space driver host not implemented yet |
| Filesystem Layer | In Progress | VFS service stub implemented |
| Networking Stack | In Progress | network service stub implemented |
| User Space Interface | Pending | native runtime API layer not implemented yet |
| Init System | In Progress | init service stub implemented |
