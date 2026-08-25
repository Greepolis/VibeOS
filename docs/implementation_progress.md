# Implementation Progress (Macro Areas)

Last review: 2026-08-25

This document is now split by macro area for easier maintenance.

| Macro Area | Status | Details |
| --- | --- | --- |
| Bootloader | Completed (Phase 4 boot-to-CLI verified) | [bootloader.md](implementation_progress/bootloader.md) |
| Kernel Core | In Progress (ring-3 runtime and shell baseline verified) | [kernel_core.md](implementation_progress/kernel_core.md) |
| Process Scheduler | In Progress (preemptive SMP runtime and signal-carrying process lifecycle verified) | [process_scheduler.md](implementation_progress/process_scheduler.md) |
| Memory Manager | In Progress (runtime PMM allocation and frame refcounting verified) | [memory_manager.md](implementation_progress/memory_manager.md) |
| Virtual Memory | In Progress (hardware paging, per-process isolation and copy-on-write fork verified) | [virtual_memory.md](implementation_progress/virtual_memory.md) |
| Interrupt Handling | In Progress (CPU traps, PIT and APIC timer paths verified) | [interrupt_handling.md](implementation_progress/interrupt_handling.md) |
| System Call Interface | In Progress (Linux process semantics verified from ring 3) | [system_call_interface.md](implementation_progress/system_call_interface.md) |
| IPC Subsystem | In Progress | [ipc_subsystem.md](implementation_progress/ipc_subsystem.md) |
| Driver Framework | In Progress (virtio block/network, PS/2 keyboard and mouse, framebuffer desktop verified) | [driver_framework.md](implementation_progress/driver_framework.md) |
| Filesystem Layer | In Progress (runtime FAT read/write verified) | [filesystem_layer.md](implementation_progress/filesystem_layer.md) |
| Networking Stack | In Progress (runtime virtio-net TCP/IP baseline verified) | [networking_stack.md](implementation_progress/networking_stack.md) |
| User Space Interface | In Progress (unmodified static Linux binaries verified from ring 3) | [user_space_interface.md](implementation_progress/user_space_interface.md) |
| Init System | In Progress | [init_system.md](implementation_progress/init_system.md) |

## Notes

- Status reflects code currently present in repository, not only planned milestones.
- "Verified" here means a gate fails if it regresses, unless the entry says otherwise. The framebuffer desktop is the standing exception: its serial-visible state is gated, its pixels are checked by hand with `scripts/dev/screenshot.py`.
- Each macro-area file contains:
  - implemented items verified in code
  - pending items
  - next execution checkpoint
