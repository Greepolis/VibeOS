# Driver Framework Progress

Status: In Progress (virtio block/network and PS/2 runtime drivers verified)
Last review: 2026-08-25

## Implemented
- User-space-first driver framework scaffolding in `user/drivers/driver_framework.c`.
- Driver host/device manager integration stubs in `user/devmgr/driver_host.c` and `user/devmgr/device_manager.c`.
- Driver registration/runtime state helpers in public interfaces (`include/vibeos/drivers.h`).
- Basic service manager interaction hooks for supervised startup paths.
- Versioned driver registration path with ABI gating and isolation caps (`vibeos_driver_register_versioned`).
- Fault-state lifecycle support (`VIBEOS_DRIVER_FAULTED`) with state counters and fault accounting.
- Runtime x86_64 drivers now include virtio-blk storage, virtio-net networking, PS/2 keyboard input, PS/2 mouse input, serial console, framebuffer output, legacy PIC/PIT and local-APIC support.
- The QEMU runtime uses virtio block/network devices for the booted OS path; this is distinct from the user-space driver-framework model.
- **PS/2 mouse on IRQ12** (`kernel/arch/x86_64/mouse.c`). The controller multiplexes two devices onto one pair of ports, so every mouse command is prefixed with `0xD4` and its data arrives on the keyboard's port, distinguished only by which interrupt fired. Two things are handled rather than assumed: the three-byte packets are a stream with no framing, so bit 3 of the first byte - always set - is checked as the only resynchronisation point there is; and movement is sign-and-magnitude, with the sign in the status byte, so reading the byte as a signed char gives a cursor that moves the right distance in the wrong direction half the time. Every wait on the controller is bounded, because a missing or wedged PS/2 controller must not hang the boot.
- **A framebuffer desktop** (`kernel/arch/x86_64/gui.c`) over the UEFI linear framebuffer: a desktop, a panel, a pointer that follows the mouse, and one window that mirrors the console, so the machine shows on screen what it has been saying on the serial line. It composites into a back buffer supplied by the caller and copies only what changed - drawing straight into video memory makes a moving pointer flicker and leave fragments, and framebuffer memory is uncached write-combining, so the read-modify-write that erasing a cursor needs is unusually expensive there.
- What a serial log can say about the picture is reported as `[GUI] GUI_STATS frames=... termchars=...`: pointer repaints happened, so the mouse produced packets, and characters reached the on-screen terminal, so the console really is mirrored. The boot gate fails on `gui_terminal_empty` if a desktop came up and the terminal stayed empty. **The pixels themselves are not gated** - `scripts/dev/screenshot.py` looks at them and a human runs it.

## Pending
- A gate that looks at pixels. A check somebody has to remember to run is not a check.
- Input routing. The mouse moves a pointer and nothing else; keystrokes reach the shell rather than a focused window, so the GUI is a drawing rather than a window system.
- The GUI is kernel code. A ring-3 program cannot query the screen, present a buffer or read input.
- Driver isolation contract hardening (capabilities, fault containment).
- Dynamic loading/version negotiation for driver binaries (loader + signature path not yet implemented).
- Hardware probe lifecycle and hotplug policy.
- DMA/IOMMU policy, device reset/recovery and driver fault containment for runtime drivers.

## Next checkpoint
- Define a common runtime driver contract and isolate the existing hardware drivers behind it before adding hotplug or additional buses.
