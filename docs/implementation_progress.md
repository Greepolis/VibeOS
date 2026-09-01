# Implementation Progress (Macro Areas)

Last review: 2026-08-29

This document is now split by macro area for easier maintenance.

| Macro Area | Status | Details |
| --- | --- | --- |
| Bootloader | Completed (Phase 4 boot-to-CLI verified) | [bootloader.md](implementation_progress/bootloader.md) |
| Kernel Core | In Progress (ring-3 runtime and shell baseline verified) | [kernel_core.md](implementation_progress/kernel_core.md) |
| Process Scheduler | In Progress (preemptive SMP runtime and signal-carrying process lifecycle verified) | [process_scheduler.md](implementation_progress/process_scheduler.md) |
| Boot Repeatability | **24 boots clean out of 24**, from a long-standing 1 in 8. Six defects fixed: three were in the diagnostics rather than the kernel they watched, and three came from CI and nightly logs - a copy-on-write fast path deciding on a fact its compare-exchange could not guard, a kernel address handed to ring 3 as AT_BASE, and a panic that stopped one core instead of the machine, which is what separated every silent wedge from its cause. 24/24 is a good number and not a proof; what is stronger is that two of the three are asserted deterministically and the third turns a silence into a named panic | [boot_repeatability.md](implementation_progress/boot_repeatability.md) |
| Tasks and Scheduling | **Rewrite underway** ([docs/sched/](sched/README.md)): S-P0 to S-P2 done - counters and a `tasks` console view, a transition table that refuses illegal state changes rather than performing them, tenancy on every task reference, and a run queue that is a tested data structure. Next: S-P3, lifetime, which is the phase that repairs the defects | [sched.md](implementation_progress/sched.md) |
| Memory Manager | **Rewrite underway** (ADR-0007). **P0, P1, P2 and P3 done; P4 at steps 1-2 of 3**. Physical frames have one owner in `kernel/mm/frame.c`; address spaces record what they own in the page-table entry (`kernel/mm/vmspace.c`), so nothing infers ownership from permission bits any more. No page-table write outside `kernel/mm/`, checked on every build. Host tests, 150 torture seeds and single boots are green; the repeat-boot criterion is not, and closing that comes before P3. Plan in [docs/mm/](mm/README.md); current state in [memory_manager.md](implementation_progress/memory_manager.md) | [memory_manager.md](implementation_progress/memory_manager.md) |
| Program Loading | **Rewrite underway** ([docs/exec/](exec/README.md)). **X-P0, X-P1, X-P3 and X-P4 done**: every exec refusal names itself and is counted, the segment layout and startup block are host-tested (twelve cases), a failed exec no longer leaks the address space it had already built - eleven refusal points route through one unwinding path - and the interpreter substitution is one function the build checks for. **X-P2 mapping on**: image pages come from the page cache, 4242 mapped against 237 copied. It took four attempts; the cause was that `kernel/mm/backing.c` had **no lock** - invisible while the cache had one caller under the exec lock, and a corruption the moment the loader became a second one. Fourth layer in this project to need a `set_lock`. The staging buffers and demand paging are still open | [exec.md](implementation_progress/exec.md) |
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
