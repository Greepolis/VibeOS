# Driver Framework Progress

Status: In Progress (virtio block/network and PS/2 runtime drivers verified)
Last review: 2026-08-01

## Implemented
- User-space-first driver framework scaffolding in `user/drivers/driver_framework.c`.
- Driver host/device manager integration stubs in `user/devmgr/driver_host.c` and `user/devmgr/device_manager.c`.
- Driver registration/runtime state helpers in public interfaces (`include/vibeos/drivers.h`).
- Basic service manager interaction hooks for supervised startup paths.
- Versioned driver registration path with ABI gating and isolation caps (`vibeos_driver_register_versioned`).
- Fault-state lifecycle support (`VIBEOS_DRIVER_FAULTED`) with state counters and fault accounting.
- Runtime x86_64 drivers now include virtio-blk storage, virtio-net networking, PS/2 keyboard input, serial console, framebuffer output, legacy PIC/PIT and local-APIC support.
- The QEMU runtime uses virtio block/network devices for the booted OS path; this is distinct from the user-space driver-framework model.

## Pending
- Driver isolation contract hardening (capabilities, fault containment).
- Dynamic loading/version negotiation for driver binaries (loader + signature path not yet implemented).
- Hardware probe lifecycle and hotplug policy.
- DMA/IOMMU policy, device reset/recovery and driver fault containment for runtime drivers.

## Next checkpoint
- Define a common runtime driver contract and isolate the existing hardware drivers behind it before adding hotplug or additional buses.
