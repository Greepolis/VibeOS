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
- Main files: `boot/bootloader_stub.c`, `include/vibeos/boot.h`, `include/vibeos/bootloader.h`
- Public interfaces: `vibeos_boot_info_t`, `vibeos_bootloader_build_boot_info`
- Dependencies: firmware and kernel entry contract

### Kernel Core
- Responsibilities: early init orchestration and subsystem bootstrap
- Main files: `kernel/core/kmain.c`, `include/vibeos/kernel.h`
- Public interfaces: `vibeos_kmain`, `vibeos_kernel_t`
- Dependencies: memory manager, scheduler, IPC

### Process Scheduler
- Responsibilities: runqueue management and dispatch primitive
- Main files: `kernel/sched/scheduler.c`, `include/vibeos/scheduler.h`
- Public interfaces: `vibeos_sched_init`, `vibeos_sched_enqueue`, `vibeos_sched_next`, `vibeos_sched_tick`
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
- Main files: `kernel/core/interrupts.c`, `kernel/arch/x86_64/idt.c`, `include/vibeos/interrupts.h`, `include/vibeos/arch_x86_64.h`
- Public interfaces: `vibeos_intc_init`, `vibeos_intc_register`, `vibeos_intc_dispatch`, `vibeos_x86_64_idt_*`
- Dependencies: HAL, scheduler, timer

### System Call Interface
- Responsibilities: user-kernel boundary ABI
- Main files: `kernel/core/syscall.c`, `include/vibeos/syscall.h`
- Public interfaces: `vibeos_syscall_dispatch`
- Dependencies: kernel object model, IPC, VM

### Process Management
- Responsibilities: process and thread identity lifecycle primitives
- Main files: `kernel/proc/process.c`, `include/vibeos/proc.h`
- Public interfaces: `vibeos_proc_init`, `vibeos_proc_spawn`, `vibeos_thread_create`
- Dependencies: syscall interface, scheduler

### Object and Handle Model
- Responsibilities: kernel object access mediation through handle IDs and rights
- Main files: `kernel/object/handle_table.c`, `include/vibeos/object.h`
- Public interfaces: `vibeos_handle_alloc`, `vibeos_handle_close`, `vibeos_handle_rights`
- Dependencies: syscall interface, security model

### IPC Subsystem
- Responsibilities: event signaling and channel messaging
- Main files: `kernel/ipc/event.c`, `kernel/ipc/channel.c`, `kernel/ipc/waitset.c`, `include/vibeos/ipc.h`, `include/vibeos/waitset.h`
- Public interfaces: `vibeos_event_*`, `vibeos_channel_*`
- Dependencies: scheduler, handle model (future)

### Driver Framework
- Responsibilities: user-space-first driver hosting
- Main files: `user/drivers/driver_framework.c`, `user/devmgr/device_manager.c`, `include/vibeos/drivers.h`
- Public interfaces: `vibeos_driver_framework_init`, `vibeos_driver_register`
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
- Main files: `user/lib/user_api.c`, `include/vibeos/user_api.h`
- Public interfaces: `vibeos_user_context_init`, `vibeos_user_signal_boot_event`
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
  bootloader.h
  drivers.h
  arch_x86_64.h
  kernel.h
  mm.h
  object.h
  proc.h
  scheduler.h
  ipc.h
  waitset.h
  services.h
  syscall.h
  timer.h
  user_api.h
  vm.h

kernel/
  core/kmain.c
  core/interrupts.c
  core/syscall.c
  mm/pmm.c
  mm/vm.c
  object/handle_table.c
  proc/process.c
  sched/scheduler.c
  time/timer.c
  ipc/event.c
  ipc/channel.c
  ipc/waitset.c
  arch/x86_64/idt.c
  arch/x86_64/boot_stub.c

user/
  init/init_system.c
  servicemgr/service_manager.c
  devmgr/device_manager.c
  drivers/driver_framework.c
  fs/vfs_service.c
  lib/user_api.c
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
- process table, timer, and IDT bootstrap hooks
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
- tick-based preemption primitives
Files Created/Modified:
- `kernel/sched/scheduler.c`
- `include/vibeos/scheduler.h`
Pending:
- full timeslice policy integration with runtime thread lifecycle
- priority aging and balancing policies

Module: IPC Subsystem
Status: Partial
Implemented:
- event primitive
- bounded message channel primitive
- waitset registration primitive
Files Created/Modified:
- `kernel/ipc/event.c`
- `kernel/ipc/channel.c`
- `kernel/ipc/waitset.c`
- `include/vibeos/ipc.h`
- `include/vibeos/waitset.h`
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
- x86_64 IDT presence table stub and timer vector setup
Files Created/Modified:
- `kernel/core/interrupts.c`
- `include/vibeos/interrupts.h`
- `kernel/arch/x86_64/idt.c`
- `include/vibeos/arch_x86_64.h`
Pending:
- architecture-specific trap/IDT integration
- timer IRQ source wiring

Module: System Call Interface
Status: Partial
Implemented:
- syscall frame model
- dispatcher with initial syscall IDs
- process/thread syscall group stubs
- handle alloc/close syscall group stubs
Files Created/Modified:
- `kernel/core/syscall.c`
- `include/vibeos/syscall.h`
Pending:
- richer handle rights enforcement across subsystem APIs

Module: Process Management
Status: Partial
Implemented:
- process table
- process spawn primitive
- thread create primitive
Files Created/Modified:
- `kernel/proc/process.c`
- `include/vibeos/proc.h`
Pending:
- process lifecycle states
- handle-based process and thread object integration

Module: Object and Handle Model
Status: Partial
Implemented:
- handle table initialization
- handle allocation, close, and rights lookup primitives
Files Created/Modified:
- `kernel/object/handle_table.c`
- `include/vibeos/object.h`
Pending:
- capability propagation in IPC transfers
- per-process handle table isolation

Module: Timer Subsystem
Status: Partial
Implemented:
- timer tick counter and frequency state
Files Created/Modified:
- `kernel/time/timer.c`
- `include/vibeos/timer.h`
Pending:
- hardware timer backend wiring
- periodic interrupt source integration

Module: User Space Services
Status: Partial
Implemented:
- init service stub
- service manager stub
- device manager stub
- filesystem service stub
- networking service stub
Files Created/Modified:
- `user/init/init_system.c`
- `user/servicemgr/service_manager.c`
- `user/devmgr/device_manager.c`
- `user/drivers/driver_framework.c`
- `user/fs/vfs_service.c`
- `user/lib/user_api.c`
- `user/net/network_service.c`
- `include/vibeos/services.h`
- `include/vibeos/drivers.h`
- `include/vibeos/user_api.h`
Pending:
- richer service supervision policies
- real IPC contracts between services

Module: Bootloader Interface
Status: Partial
Implemented:
- boot info builder stub for kernel handoff contract
Files Created/Modified:
- `boot/bootloader_stub.c`
- `include/vibeos/bootloader.h`
Pending:
- real UEFI image loading path
- firmware table extraction and handoff

## System Implementation Status

| Component | Status | Notes |
| --- | --- | --- |
| Bootloader | In Progress | boot info builder stub implemented |
| Kernel Core | In Progress | `kmain` bootstrap logic implemented |
| Process Scheduler | In Progress | queue + tick preemption primitives implemented |
| Memory Manager | In Progress | bump allocator implemented |
| Virtual Memory | In Progress | address-space mapping primitives implemented |
| Interrupt Handling | In Progress | controller + x86_64 IDT stub implemented |
| System Call Interface | In Progress | dispatcher + process/thread/handle syscall stubs |
| IPC Subsystem | In Progress | event + channel primitives implemented |
| Driver Framework | In Progress | driver framework registration stubs implemented |
| Filesystem Layer | In Progress | VFS service stub implemented |
| Networking Stack | In Progress | network service stub implemented |
| User Space Interface | In Progress | user API syscall bridge stubs implemented |
| Init System | In Progress | init service stub implemented |
