# Implementation Progress (Macro Areas)

Last review: 2026-08-29

This document is now split by macro area for easier maintenance.

| Macro Area | Status | Details |
| --- | --- | --- |
| Bootloader | Completed (Phase 4 boot-to-CLI verified) | [bootloader.md](implementation_progress/bootloader.md) |
| Kernel Core | In Progress (ring-3 runtime and shell baseline verified) | [kernel_core.md](implementation_progress/kernel_core.md) |
| Process Scheduler | In Progress (preemptive SMP runtime and signal-carrying process lifecycle verified) | [process_scheduler.md](implementation_progress/process_scheduler.md) |
| Memory Manager | **Rewrite underway** (ADR-0007), P0 and P1 done: physical frames have one owner in `kernel/mm/frame.c`, an allocation owns its frame (decision D9 = A), the bootstrap bump allocator is closed, and `meminfo` reports a breakdown the boot gate checks adds up, including fragmentation. Next: P2, where ownership stops being inferred from hardware bits - the cause of the defect that survived four fixes. Plan in [docs/mm/](mm/README.md); current state in [memory_manager.md](implementation_progress/memory_manager.md) | [memory_manager.md](implementation_progress/memory_manager.md) |
| Virtual Memory | In Progress (hardware paging, per-process isolation, reference-counted copy-on-write fork and unmap, TLB shootdown across cores verified) | [virtual_memory.md](implementation_progress/virtual_memory.md) |
| Interrupt Handling | In Progress (CPU traps, PIT and APIC timer paths verified) | [interrupt_handling.md](implementation_progress/interrupt_handling.md) |
| System Call Interface | In Progress (Linux process semantics verified from ring 3) | [system_call_interface.md](implementation_progress/system_call_interface.md) |
| IPC Subsystem | In Progress | [ipc_subsystem.md](implementation_progress/ipc_subsystem.md) |
| Driver Framework | In Progress (virtio-blk **and AHCI/SATA**, virtio-net, PS/2 keyboard and mouse, framebuffer desktop verified; both disk controllers gated in CI) | [driver_framework.md](implementation_progress/driver_framework.md) |
| Filesystem Layer | In Progress (FAT, ext2, ISO9660, exFAT and NTFS behind one VFS; journalled writes verified against power loss) | [filesystem_layer.md](implementation_progress/filesystem_layer.md) |
| Networking Stack | In Progress (IPv4 runtime plus portable route/firewall data-path enforcement verified) | [networking_stack.md](implementation_progress/networking_stack.md) |
| User Space Interface | In Progress (unmodified static Linux binaries verified from ring 3) | [user_space_interface.md](implementation_progress/user_space_interface.md) |
| Diagnostics & Observability | In Progress (six runtime detectors, all gated; they closed the premature-free defect that had been chased three times from the far end) | [diagnostics.md](implementation_progress/diagnostics.md) |
| Init System | In Progress (native ring-3 init is PID 1 and supervises a four-service manifest in the guest: start, clean stop, bounded restart, and a real crash the kernel survives) | [init_system.md](implementation_progress/init_system.md) |
| Linux Compatibility | In Progress (static, position-independent **and dynamically linked** binaries all run end to end and are gated) | [linux_compatibility.md](implementation_progress/linux_compatibility.md) |
| Windows Compatibility | Not started (a two-entry translation table, no runtime) | [windows_compatibility.md](implementation_progress/windows_compatibility.md) |
| macOS Compatibility | Not started (same, and deliberately dormant) | [macos_compatibility.md](implementation_progress/macos_compatibility.md) |

## Notes

- Status reflects code currently present in repository, not only planned milestones.
- A file existing under `user/compat/` is not progress. The Windows and macOS
  translators are twenty-four lines each, know two syscall numbers, and nothing
  calls them; they are tracked as not started for that reason. The Linux
  compatibility that works is the kernel syscall layer, which is a different
  piece of code entirely.
- "Verified" here means a gate fails if it regresses, unless the entry says otherwise. The framebuffer desktop is the standing exception: its serial-visible state is gated, its pixels are checked by hand with `scripts/dev/screenshot.py`.
- Each macro-area file contains:
  - implemented items verified in code
  - pending items
  - next execution checkpoint
