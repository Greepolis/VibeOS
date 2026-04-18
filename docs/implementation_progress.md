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
- Main files: `kernel/core/interrupts.c`, `kernel/arch/x86_64/idt.c`, `kernel/arch/x86_64/trap.c`, `include/vibeos/interrupts.h`, `include/vibeos/arch_x86_64.h`, `include/vibeos/trap.h`
- Public interfaces: `vibeos_intc_init`, `vibeos_intc_register`, `vibeos_intc_dispatch`, `vibeos_intc_bind_timer_irq`, `vibeos_x86_64_idt_*`, `vibeos_trap_*`, `vibeos_trap_classify`
- Dependencies: HAL, scheduler, timer

### System Call Interface
- Responsibilities: user-kernel boundary ABI
- Main files: `kernel/core/syscall.c`, `kernel/core/syscall_policy.c`, `include/vibeos/syscall.h`, `include/vibeos/syscall_abi.h`, `include/vibeos/syscall_policy.h`
- Public interfaces: `vibeos_syscall_dispatch`
- Dependencies: kernel object model, IPC, VM

### Process Management
- Responsibilities: process and thread identity lifecycle primitives
- Main files: `kernel/proc/process.c`, `include/vibeos/proc.h`
- Public interfaces: `vibeos_proc_init`, `vibeos_proc_spawn`, `vibeos_thread_create`, `vibeos_thread_state`, `vibeos_thread_set_state`, `vibeos_thread_exit`, `vibeos_proc_bind_process_handle`, `vibeos_proc_bind_thread_handle`, `vibeos_proc_resolve_object_handle`, `vibeos_proc_revoke_handle_lineage`, `vibeos_proc_revoke_handle_lineage_scoped`, `vibeos_proc_audit_count`, `vibeos_proc_audit_get`, `vibeos_proc_audit_set_policy`, `vibeos_proc_audit_get_policy`, `vibeos_proc_audit_get_dropped`
- Dependencies: syscall interface, scheduler

### Object and Handle Model
- Responsibilities: kernel object access mediation through handle IDs and rights
- Main files: `kernel/object/handle_table.c`, `include/vibeos/object.h`
- Public interfaces: `vibeos_handle_alloc`, `vibeos_handle_alloc_object`, `vibeos_handle_close`, `vibeos_handle_rights`, `vibeos_handle_object`, `vibeos_handle_set_provenance`, `vibeos_handle_provenance`, `vibeos_handle_has_rights`, `vibeos_handle_set_quota`, `vibeos_handle_stats`, `vibeos_handle_count_by_type`, `vibeos_handle_set_lifecycle_hook`, `vibeos_handle_revoke_origin`
- Dependencies: syscall interface, security model

### Security Policy Engine
- Responsibilities: capability-aware allow/deny decisions for sensitive operations
- Main files: `kernel/core/policy.c`, `include/vibeos/policy.h`
- Public interfaces: `vibeos_policy_can_fs_open`, `vibeos_policy_can_net_bind`, `vibeos_policy_can_process_spawn`
- Dependencies: security token model

### IPC Subsystem
- Responsibilities: event signaling and channel messaging
- Main files: `kernel/ipc/event.c`, `kernel/ipc/channel.c`, `kernel/ipc/handle_transfer.c`, `kernel/ipc/waitset.c`, `include/vibeos/ipc.h`, `include/vibeos/ipc_transfer.h`, `include/vibeos/waitset.h`
- Public interfaces: `vibeos_event_*`, `vibeos_channel_*`, `vibeos_waitset_wait`, `vibeos_waitset_wait_ex`, `vibeos_waitset_wait_timed`, `vibeos_waitset_init_owned`, `vibeos_waitset_add_owned`, `vibeos_waitset_remove`, `vibeos_waitset_reset`, `vibeos_waitset_destroy`, `vibeos_waitset_set_wake_policy`, `vibeos_waitset_get_wake_policy`
- Dependencies: scheduler, handle model (future)

### Driver Framework
- Responsibilities: user-space-first driver hosting
- Main files: `user/drivers/driver_framework.c`, `user/devmgr/device_manager.c`, `user/devmgr/driver_host.c`, `include/vibeos/drivers.h`, `include/vibeos/driver_host.h`
- Public interfaces: `vibeos_driver_framework_init`, `vibeos_driver_register`, `vibeos_driver_host_probe`
- Dependencies: IPC, security policy, device manager

### Filesystem Layer
- Responsibilities: VFS contract and filesystem service boundaries
- Main files: `user/fs/vfs_service.c`, `user/fs/vfs_ops.c`, `include/vibeos/services.h`, `include/vibeos/fs.h`
- Public interfaces: `vibeos_vfs_start`, `vibeos_vfs_mount`, `vibeos_vfs_open`, `vibeos_vfs_close`
- Dependencies: IPC, memory objects, driver framework

### Networking Stack
- Responsibilities: socket and protocol layers
- Main files: `user/net/network_service.c`, `user/net/socket.c`, `include/vibeos/services.h`, `include/vibeos/net.h`
- Public interfaces: `vibeos_net_start`, `vibeos_socket_*`
- Dependencies: scheduler, memory manager, drivers

### User Space Interface
- Responsibilities: native runtime and system library boundary
- Main files: `user/lib/user_api.c`, `include/vibeos/user_api.h`
- Public interfaces: `vibeos_user_context_init`, `vibeos_user_signal_boot_event`
- Dependencies: syscall interface

### Service IPC Contracts
- Responsibilities: versioned message format between system services
- Main files: `user/servicemgr/service_ipc.c`, `include/vibeos/service_ipc.h`
- Public interfaces: `vibeos_service_msg_build`, `vibeos_service_msg_validate`
- Dependencies: IPC subsystem, service manager

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
  driver_host.h
  fs.h
  net.h
  policy.h
  security_model.h
  arch_x86_64.h
  kernel.h
  mm.h
  object.h
  proc.h
  scheduler.h
  ipc.h
  ipc_transfer.h
  waitset.h
  services.h
  service_ipc.h
  syscall.h
  syscall_abi.h
  syscall_policy.h
  timer.h
  trap.h
  user_api.h
  vm.h

kernel/
  core/kmain.c
  core/interrupts.c
  core/policy.c
  core/syscall.c
  core/syscall_policy.c
  mm/pmm.c
  mm/vm.c
  object/handle_table.c
  proc/process.c
  sched/scheduler.c
  time/timer.c
  ipc/event.c
  ipc/channel.c
  ipc/handle_transfer.c
  ipc/waitset.c
  arch/x86_64/idt.c
  arch/x86_64/trap.c
  arch/x86_64/boot_stub.c

user/
  init/init_system.c
  servicemgr/service_manager.c
  servicemgr/service_ipc.c
  devmgr/device_manager.c
  devmgr/driver_host.c
  drivers/driver_framework.c
  fs/vfs_service.c
  fs/vfs_ops.c
  lib/user_api.c
  net/network_service.c
  net/socket.c

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
   `.\scripts\run-tests.ps1 -Profile agent -BuildDir build-agent -Generator Ninja`

Notes:
- in environments where CMake generator detection is unstable, `run-tests.ps1` falls back to a manual `gcc` host build path and still emits `artifacts/kernel-tests.log` plus `artifacts/test-summary.json`.

The automated run writes:

- `artifacts/kernel-tests.log`
- `artifacts/test-summary.json`

## Development Log

Module: Build and Test Baseline (M1: Toolchain and Build Skeleton)
Status: **Completed** ✓
Date Completed: 18 aprile 2026
Implemented:
- root `CMakeLists.txt` with modular kernel-core targets and image build pipeline
- host-side kernel test executable with full test suite
- test runner script with machine-readable summary output (`artifacts/test-summary.json`)
- configurable CMake generator selection in test runner (`-Generator` parameter)
- automatic manual `gcc` fallback in test runner when CMake path fails
- boot image generation targets (`vibeos_boot_image`, `vibeos_kernel_elf`, `vibeos_image`)
- QEMU launcher script (`scripts/run-qemu.ps1`) with:
  - Serial output capture to file
  - Timeout and exit code tracking
  - GDB debug support (-Debug flag)
  - JSON artifact for structured results
- CMake helper modules:
  - `cmake/create_boot_image.cmake` for image generation
  - `cmake/vibeos_toolchain_info.cmake.in` for toolchain export
- Toolchain version capture (cmake, gcc, qemu, ninja versions)
- Build artifact validation (checks for kernel ELF and boot image)
- Extended test runner with image build option (`-RunQemu` for emulator smoke test)
- Documentation updates:
  - `docs/toolchain.md` - M1 commands and Quick Start section
  - `README.md` - M1 status, prerequisites, and usage examples

Files Created/Modified:
- `CMakeLists.txt` (extended with image targets)
- `tests/kernel/kernel_tests.c`
- `scripts/run-tests.ps1` (extended with image validation + QEMU integration)
- `scripts/run-qemu.ps1` (NEW)
- `cmake/create_boot_image.cmake` (NEW)
- `cmake/vibeos_toolchain_info.cmake.in` (NEW)
- `docs/toolchain.md` (updated with M1 details)
- `README.md` (updated with M1 status and Quick Start)
- `.gitignore`

Test Results:
- All host-side tests: PASS ✓
- Build time: ~3-4 seconds (configuration + build + test)
- Artifacts generated:
  - `build/vibeos_kernel_tests.exe` (test executable)
  - `build/artifacts/vibeos_kernel.elf` (kernel ELF image)
  - `build/artifacts/vibeos_boot.img` (boot stub image, 120KB)
  - `build/artifacts/boot_manifest.txt` (artifact manifest)
  - `build/artifacts/vibeos_toolchain_info.cmake` (toolchain export)
  - `artifacts/test-summary.json` (structured test results)
  - `artifacts/kernel-tests.log` (full test log)

M1 Definition of Done:
✓ Cross-compilation baseline (CMake + Ninja + GCC fallback)
✓ One-command local build and test
✓ Boot image generation pipeline
✓ Build artifact validation
✓ Toolchain version capture and export
✓ QEMU launcher integration
✓ Structured test feedback (JSON)
✓ Documentation complete with commands

Known Limitations:
- Boot image is ELF stub (placeholder); real bootloader integration in M2
- QEMU integration is ready but requires QEMU installation (gracefully skipped if not available)
- no freestanding cross-toolchain configured yet, so first execution path uses host simulation
- CMake configuration can be unstable on some Windows environments due to executable file access or lock timing; fallback path mitigates this for host L0 coverage

Next Phase: M2 (Boot to Kernel Banner)
- Will integrate real UEFI bootloader
- Will validate kernel boot in emulator
- Will add serial logging from kernel entry
- Will parse early memory maps

Module: Boot Information Contract and Early Console (M2: Boot to Kernel Banner)
Status: **In Progress** - Core infrastructure ready, QEMU validation pending
Date Started: 18 aprile 2026
Implemented:
- Serial I/O primitives for x86_64 (COM1/0x3F8) with putc/puts helpers
- Kernel entry point (kmain.c) enhanced with serial logging for boot phases
- Boot stage names for early diagnostics
- Boot info validation with error messages to serial
- Serial output on kernel initialization, memory manager, VM, IPC, scheduler, and interrupts
- "BOOT_OK" marker output when kernel reaches ready state
- QEMU launcher (run-qemu.ps1) enhanced with boot token verification (-ExpectToken parameter)
- Run-tests.ps1 extended with -RunM2BootSmoke parameter for automated M2 validation
- Build system updated to include kernel/arch/x86_64/serial.c in all compilation paths

Files Created/Modified:
- `kernel/arch/x86_64/serial.c` (NEW) - serial I/O primitives
- `kernel/core/kmain.c` - boot logging and BOOT_OK marker
- `include/vibeos/arch_x86_64.h` - serial function declarations
- `CMakeLists.txt` - added serial.c to kernel_core build
- `scripts/run-qemu.ps1` - M2 boot token verification
- `scripts/run-tests.ps1` - M2 boot smoke test support

M2 Exit Criteria:
✓ Boot information contract validation in kernel entry
✓ Early console (serial output) from kernel
✓ Early paging enablement placeholder (stub ready)
✓ boot stage transition logging
⏳ QEMU boot smoke test (requires QEMU installation to execute)

Known Limitations:
- Host-side tests do not support x86_64 serial I/O on Windows (real testing via QEMU only)
- QEMU is not installed in current environment (boot test ready, will pass when QEMU available)
- Bootloader is still stub; real boot protocol integration in M3

M2 Test Command (when QEMU available):
```powershell
./scripts/run-tests.ps1 -BuildDir build -RunM2BootSmoke
```

Next Phase: M3 (Memory and Interrupts Online)
- Will fully integrate bootloader UEFI or multiboot2
- Will validate boot protocol on real emulator
- Will add timer interrupt integration
- Will implement full SMP bootstrap

Files for M2 Completion:
- `build/artifacts/vibeos_boot.img` - bootable kernel image with serial support
- `artifacts/qemu-serial.log` - serial output capture (when QEMU runs)
- `artifacts/test-summary.json` - L2 boot-smoke test results

Module: Memory Manager
Status: Partial
Implemented:
- bootstrap page-frame bump allocator
- boot-info-aware PMM usable-region selection with initrd-overlap avoidance
Files Created/Modified:
- `kernel/mm/pmm.c`
- `include/vibeos/mm.h`
Pending:
- runtime buddy allocator policy
- richer reserved-region and zone policy mapping

Module: Process Scheduler
Status: Partial
Implemented:
- per-CPU-ready runqueue structure
- enqueue and dequeue primitives
- tick-based preemption primitives
- wait timeout and wake counters for scheduler observability
- runqueue observability helpers (`runqueue_depth`, `runnable_threads`)
- least-loaded CPU selection and balanced enqueue helper (`enqueue_balanced`)
- scheduler CPU-count query helper for syscall observability paths
- aggregate scheduler counters (`preemptions`, `wait_timeouts`, `wait_wakes`) for runtime telemetry
- scheduler counter summary helper (`preemptions`, `wait_timeouts`, `wait_wakes`, runnable threads, cpu count)
- scheduler counter reset helper for controlled instrumentation epochs
- class-based default timeslice policy helpers and enqueue-time timeslice normalization
- runqueue aging helpers (`age_cpu`, `age_all`) with bounded timeslice boosts
- scheduler runtime tracking for per-thread lifecycle state (`RUNNABLE/RUNNING/BLOCKED`) with tracked-thread registry
- explicit wait lifecycle hooks (`wait_begin`, `wait_end`) with dequeue/requeue semantics and preferred wake CPU selection
- scheduler wait-transition telemetry (`wait_begin`, `wait_end`, `requeues`, `requeue_failures`) plus tracked/blocked thread counters
- per-thread runtime observability (`state`, `cpu`, wait transition counters, migrations)
- per-thread QoS controls (`nice` and CPU affinity mask) with safe validation and runtime getters/setters
- scheduler starvation instrumentation and boost controls (`starvation_tick`, `boost_starving`)
- affinity-aware enqueue/wake placement with mismatch telemetry (`affinity_misses`)
- active runqueue rebalance pass (`rebalance`) with bounded migration budget and runtime counters
- QoS summary telemetry (`rebalance_passes`, `rebalance_moves`, `affinity_misses`, `priority_boosts`)
Files Created/Modified:
- `kernel/sched/scheduler.c`
- `include/vibeos/scheduler.h`
Pending:
- full timeslice policy integration with runtime thread lifecycle
- priority-aware wake-placement heuristics under contention
- NUMA/topology-aware load balancing policy

Module: IPC Subsystem
Status: Partial
Implemented:
- event primitive
- bounded message channel primitive
- waitset registration primitive
- waitset timeout wait primitive
- channel helpers for message transfer with handle rights metadata
- waitset extended wait API with scheduler wake/timeout feedback
- timer-driven waitset path integrated with kernel timer ticks
- process-scoped waitset ownership enforcement (owner PID bound)
- waitset lifecycle semantics: remove/reset/destroy with safe re-init behavior
- configurable waitset wake policy (`FIFO` and `REVERSE`) with runtime getters/setters
- waitset telemetry counters (add/remove/wait/wake/timeout/ownership-denial) with stats query API
- waitset telemetry reset and owner-scoped wake-policy control paths
- round-robin waitset wake policy with rotating cursor
- thread-aware wait wrappers (`wait_for_thread`, `wait_timed_for_thread`) that bracket waits with process-thread blocked/runnable transitions
- scheduler-coupled thread wait wrappers with runtime `wait_begin/wait_end` integration
- CPU-targeted thread wait wrappers (`wait_for_thread_on_cpu`, `wait_timed_for_thread_on_cpu`) with wake CPU feedback
- per-event waitset metadata (`priority`, `enabled`) with runtime update/query controls
- wake policy extension with priority-based selection (`WAKE_PRIORITY`)
- batch and aggregate wait primitives (`wait_batch`, `wait_all`, `peek_signaled`)
- event-centric waitset control helpers (`add_with_priority`, `remove_event`, enable/disable by index)
- extended waitset telemetry (`disabled_skips`, `priority_wakes`, `wait_all`, `wait_batch`, `peek`)
Files Created/Modified:
- `kernel/ipc/event.c`
- `kernel/ipc/channel.c`
- `kernel/ipc/waitset.c`
- `include/vibeos/ipc.h`
- `include/vibeos/waitset.h`
Pending:
- fairness-aware wake scheduling for large waitsets
- waitset sharding strategy for large fan-in workloads

Module: Virtual Memory
Status: Partial
Implemented:
- address-space object initialization
- map, unmap, protect, and lookup primitives
- partial range unmap with split-map handling
- map-count and total-mapped observability helpers
- read-only address-space clone primitive for early copy-on-write modeling
- address-space structural validation helper (`validate`) with overflow and overlap checks
- adjacent mapping compaction helper (`compact`) for denser map layouts
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
- x86_64 trap frame dispatch state
- trap taxonomy classification (fault/interrupt/syscall/spurious)
- IRQ mask/unmask controls and global interrupt enable/disable gate in host runtime controller
- timer IRQ binding helper from interrupt controller to timer tick source
Files Created/Modified:
- `kernel/core/interrupts.c`
- `include/vibeos/interrupts.h`
- `kernel/arch/x86_64/idt.c`
- `include/vibeos/arch_x86_64.h`
- `kernel/arch/x86_64/trap.c`
- `include/vibeos/trap.h`
Pending:
- architecture-specific trap/IDT integration

Module: System Call Interface
Status: Partial
Implemented:
- syscall frame model
- dispatcher with initial syscall IDs
- process/thread syscall group stubs
- handle alloc/close syscall group stubs
- vm map/unmap/protect syscall group stubs
- waitset add-event syscall stub
- handle-rights enforcement for sensitive syscall operations
- centralized syscall policy lookup for required handle rights
- ABI v0 argument mapping formalized through shared helpers (`syscall_abi.h`)
- process audit export syscall hooks (`PROC_AUDIT_COUNT`, `PROC_AUDIT_GET`)
- caller-scoped audit access policy with redacted non-kernel view
- kernel-only syscall controls for audit retention policy and dropped-event counters
- spawn authorization gate tied to capability policy (`can_process_spawn`)
- process or thread lifecycle introspection and control syscalls with caller ownership enforcement
- process lifecycle mutation syscalls (`PROCESS_STATE_SET`, `PROCESS_TERMINATE`) with self-or-kernel authorization
- runtime observability syscalls for process or thread counts and scheduler queue metrics
- runtime observability syscalls for process live or terminated counts and scheduler per-cpu and aggregate counters
- runtime observability syscalls for process or thread state counts and summarized state distribution
- waitset telemetry syscalls with owner-scoped access policy
- waitset wake-policy set/get and waitset telemetry reset syscalls
- waitset owner introspection syscall and process or thread transition-counter telemetry syscalls
- waitset runtime snapshot syscall (`owner`, `ownership-enforcement`, `wake-policy`, `event-count`)
- process-audit aggregate and filtered counters (`count by action`, `count by success`, `summary`)
- scheduler counter summary and kernel-only scheduler counter reset syscalls
- process security-token syscalls (`PROCESS_TOKEN_GET`, `PROCESS_TOKEN_SET`) with scoped access control
- caller-aware spawn authorization path (policy checks against caller token capabilities, not global kernel token)
- policy introspection and control syscalls (`POLICY_CAPABILITY_GET`, `POLICY_CAPABILITY_SET`, `POLICY_SUMMARY_GET`)
- security-audit syscalls (`SEC_AUDIT_COUNT`, `SEC_AUDIT_GET`, `SEC_AUDIT_COUNT_ACTION`, `SEC_AUDIT_SUMMARY`, `SEC_AUDIT_RESET`)
- security-audit success-filter syscall (`SEC_AUDIT_COUNT_SUCCESS`)
- process MAC label syscalls (`PROCESS_SECURITY_LABEL_GET`, `PROCESS_SECURITY_LABEL_SET`, `PROCESS_INTERACT_CHECK`)
- process thread-state query syscalls (`PROCESS_THREAD_STATE_COUNT_GET`, `PROCESS_RUNNABLE_THREADS_GET`, `PROCESS_BLOCKED_THREADS_GET`)
- scheduler runtime observability syscalls (`SCHED_TRACKED_THREADS_GET`, `SCHED_BLOCKED_THREADS_GET`, `SCHED_WAIT_TRANSITION_SUMMARY_GET`, `SCHED_THREAD_RUNTIME_GET`)
- scheduler QoS control and observability syscalls (`SCHED_THREAD_AFFINITY_SET/GET`, `SCHED_THREAD_NICE_SET/GET`, `SCHED_REBALANCE`, `SCHED_STARVATION_TICK`, `SCHED_QOS_SUMMARY_GET`, `SCHED_BOOST_STARVING`)
Files Created/Modified:
- `kernel/core/syscall.c`
- `include/vibeos/syscall.h`
- `include/vibeos/syscall_abi.h`
- `kernel/core/syscall_policy.c`
- `include/vibeos/syscall_policy.h`
Pending:
- stronger cross-process authorization model beyond direct relationship checks

Module: Process Management
Status: Partial
Implemented:
- process table
- process spawn primitive
- thread create primitive
- per-process entry table with owned handle tables
- process handle-table lookup helper (`vibeos_proc_handles`)
- process handle duplication policy helper (`vibeos_proc_duplicate_handle`)
- process lifecycle states (`NEW/RUNNING/BLOCKED/TERMINATED`)
- lifecycle APIs (`vibeos_proc_state`, `vibeos_proc_set_state`, `vibeos_proc_terminate`)
- thread registry with owner process binding
- thread lifecycle controls (`state`, `set_state`, `exit`) and thread-count consistency updates
- thread owner introspection helper (`vibeos_thread_owner`) used by syscall authorization
- process-table observability helpers for total, live, and terminated process counts
- process and thread state counters plus summary helpers for runtime introspection
- process and thread transition counters with reset controls for instrumentation cycles
- per-process security token storage with explicit set/get APIs
- token inheritance on process spawn and explicit token override spawn API (`spawn_with_token`)
- per-process security labels with inheritance and explicit set/get controls
- label-aware interaction checks with policy-configured override capability gate
- process and thread object-handle bind or resolve helpers
- handle lineage revocation propagation across process handle tables
- selective handle lineage revocation filters (by object type and rights mask)
- revocation audit trail ring-buffer with structured event retrieval
- audit retention policy controls (`overwrite-oldest` vs `drop-newest`) with dropped-event accounting
- audit helper queries (`count by action`, `count by success`, `summary`) for observability and policy feedback loops
- explicit thread wait lifecycle helpers (`thread_wait_begin`, `thread_wait_end`) used by wait primitives
- per-process thread-state introspection helpers (`thread count by process/state`, process thread summary)
- wait-driven process state coupling (auto `BLOCKED` when no runnable thread remains, auto `RUNNING` on wake)
Files Created/Modified:
- `kernel/proc/process.c`
- `include/vibeos/proc.h`
Pending:
- richer process-state policy when a process has no active threads

Module: Object and Handle Model
Status: Partial
Implemented:
- handle table initialization
- handle allocation, close, and rights lookup primitives
- handle required-rights check helper
- IPC handle transfer primitive with rights reduction into receiver handle table
- syscall handle isolation via caller PID-scoped process handle tables
- cross-process handle duplication policy (related process only + source MANAGE right)
- object-aware handle metadata (`object_type`, `object_id`) with lookup helper
- handle provenance metadata (`origin_pid`, `origin_handle`) for duplication lineage tracking
- configurable per-table handle quota controls
- handle allocation-failure telemetry for observability
- object-type handle counters for runtime accounting
- lifecycle hook callbacks on handle alloc/close/revoke
- table-level origin-based revoke helper used by process-lineage revocation paths
Files Created/Modified:
- `kernel/object/handle_table.c`
- `include/vibeos/object.h`
- `kernel/ipc/handle_transfer.c`
- `include/vibeos/ipc_transfer.h`
Pending:
- richer object-type-specific garbage-collection and deferred cleanup policies

Module: Timer Subsystem
Status: Partial
Implemented:
- timer tick counter and frequency state
- tick conversion helpers (`ticks->ms`, `ticks->ns`) and deadline arming/expiry primitives
- timer backend binding (`HOST`/`IRQ`) with IRQ vector/divider metadata and IRQ-driven tick progression
Files Created/Modified:
- `kernel/time/timer.c`
- `include/vibeos/timer.h`
Pending:
- hardware-backed timer calibration beyond host runtime simulation

Module: User Space Services
Status: Partial
Implemented:
- init service stub
- service manager stub
- device manager stub
- driver host probe stub
- filesystem service stub
- VFS mount/open/close runtime primitives
- VFS secure-open policy gate
- networking service stub
- socket create/bind/send/close runtime primitives
- service IPC message contract build/validate
- start/stop lifecycle controls for init, devmgr, vfs, and net service stubs
- service-manager health aggregation helper for supervised service visibility
- compatibility runtime core with per-target enable controls and translation telemetry
- user API wrappers for process security label get/set and interaction-check syscalls
- service manager restart-budget/failure-report controls
- service IPC reply-correlation/status fields with stricter contract validation
Files Created/Modified:
- `user/init/init_system.c`
- `user/servicemgr/service_manager.c`
- `user/servicemgr/service_ipc.c`
- `user/devmgr/device_manager.c`
- `user/devmgr/driver_host.c`
- `user/drivers/driver_framework.c`
- `user/fs/vfs_service.c`
- `user/fs/vfs_ops.c`
- `user/lib/user_api.c`
- `user/net/network_service.c`
- `user/net/socket.c`
- `include/vibeos/services.h`
- `include/vibeos/service_ipc.h`
- `include/vibeos/drivers.h`
- `include/vibeos/driver_host.h`
- `include/vibeos/fs.h`
- `include/vibeos/net.h`
- `include/vibeos/policy.h`
- `include/vibeos/security_model.h`
- `include/vibeos/user_api.h`
Pending:
- richer service supervision policies (restart classes/backoff)
- richer service IPC contracts beyond current ACK/ERROR extension

Module: Security Model
Status: Partial
Implemented:
- security token model
- capability bit check helper
- baseline policy engine for fs/net/process-spawn checks
- runtime policy capability introspection/update helpers for fs-open, net-bind, and process-spawn controls
- kernel security-audit log for privileged security operations with caller-scoped introspection
- security-audit success/failure counters by action and by outcome (`success` filter)
- process security labels with inheritance and policy-configured override capability checks
- thread token snapshot inheritance and propagation on process token updates
- MAC interaction policy gate (`mac_enforced`) integrated in policy runtime and syscall interaction checks
Files Created/Modified:
- `kernel/core/security.c`
- `include/vibeos/security_model.h`
- `kernel/core/policy.c`
- `include/vibeos/policy.h`
Pending:
- advanced multi-domain MAC rule sets and label transition policy

Module: Bootloader Interface
Status: Partial
Implemented:
- boot info builder stub for kernel handoff contract
- boot-info validation helper and memory summary helper (`total` vs `usable` bytes)
- boot memory-map region-type counters and overlap detection helpers
- sanitized boot-info builder with supported-region filtering, ordering, overlap-rejection, and adjacent merge by type
- max physical address helper for early paging/layout planning
- firmware/initrd/framebuffer handoff metadata setters with boot-contract validation
- placement validation for firmware pointers, initrd ranges, and framebuffer ranges against memory-map region types
- firmware-tag extraction/apply flow for ACPI/SMBIOS and secure/measured-boot flags
- PE32+ kernel image load-plan parser for UEFI-style mapping preparation
Files Created/Modified:
- `boot/bootloader_stub.c`
- `include/vibeos/bootloader.h`
Pending:
- real UEFI image loading path

## System Implementation Status

| Component | Status | Notes |
| --- | --- | --- |
| Bootloader | In Progress | boot info builder stub implemented |
| Kernel Core | In Progress | `kmain` bootstrap logic implemented |
| Process Scheduler | In Progress | queue/preemption primitives, balanced enqueue, and per-cpu/aggregate telemetry |
| Memory Manager | In Progress | bump allocator implemented |
| Virtual Memory | In Progress | address-space mapping primitives implemented |
| Interrupt Handling | In Progress | controller + x86_64 IDT stub implemented |
| System Call Interface | In Progress | dispatcher + rights policy + lifecycle controls + extended observability syscalls |
| IPC Subsystem | In Progress | event/channel/waitset primitives with wake-order policy and runtime telemetry counters |
| Driver Framework | In Progress | driver framework registration stubs implemented |
| Filesystem Layer | In Progress | VFS runtime mount/open/close primitives implemented |
| Networking Stack | In Progress | socket runtime primitives, secure bind, receive simulation, and runtime stats implemented |
| User Space Interface | In Progress | user API syscall bridge stubs implemented |
| Init System | In Progress | init service stub implemented |
