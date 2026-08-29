# Driver Framework Progress

Status: In Progress (virtio block/network, AHCI/SATA and PS/2 runtime drivers verified; both disk controllers gated in CI)
Last review: 2026-08-29

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


### Storage: two controllers, one filesystem

`kernel/arch/x86_64/blk.c` is the indirection the filesystem goes through;
whichever driver comes up binds itself to it, and the order is decided in one
readable place in `arch_hw.c`. `fat.c` used to call
`vibeos_x86_64_virtio_blk_read` by name from nine places, which was honest
while virtio was the only driver and is exactly why nobody noticed the gap.

| Driver | Provided by | Gated |
| --- | --- | --- |
| virtio-blk | QEMU | yes, the default smoke run |
| AHCI/SATA | VirtualBox, VMware, real machines | yes, `VIBEOS_SMOKE_DISK=ahci` |

**Why AHCI was needed.** The kernel could only talk to virtio-blk. The
appliances imported into VirtualBox, booted, and then every `exec` failed: the
files were on the disk and there was no way to read them. The bootloader hid
it, because UEFI does the reading up to `ExitBootServices`, and after that
there was no block device at all - an absence rather than a failure, which is
quieter. CI never caught it because CI only ever ran the controller QEMU
defaults to.

The controller is matched by PCI class (01/06/01) rather than by device id:
VirtualBox emulates an ICH8, QEMU an ICH9, VMware something else, and a table
of ids would be wrong on the next hypervisor. The driver is polled rather than
interrupt-driven on purpose - the filesystem calls it from under
`g_exec_lock`, with interrupts masked, so a completion interrupt could never
arrive.

The register window is marked uncacheable (`vibeos_x86_64_mark_uncacheable`).
The identity map is built with write-back 2 MiB pages, which is right for
memory and wrong for registers whose reads have side effects.

**What is verified, and what is only correct.** Five sabotage cases were
written for this driver; two turn the AHCI boot red (bus mastering left
disabled, the port's command engine never started). Three came back NOT RED and
are kept in `scripts/dev/cases/ahci.txt` as a comment: QEMU's ich9-ahci does
not enforce the AHCI mode bit, ignores a PRDT byte count that is one too large,
and never serves MMIO from a cache line. All three are requirements on real
hardware. The only evidence beyond the specification is that the same code also
drives VirtualBox's ICH8, which is a second implementation and not a test.

Verified end to end on 2026-08-29: an appliance built by the release pipeline
imports into VirtualBox 7.2.16 and boots to the full userland - AHCI port up,
FAT16 mounted, `INIT.ELF` read from disk, all four supervised services exec'd,
and the deliberate ring-3 fault killing its task while the machine keeps going.

## Pending
- No NVMe driver. VirtualBox and VMware both offer one, and some machines
  have nothing else; AHCI covers both hypervisors today.
- `vibeos_storage_scan` (partition table then filesystem probe) is built and
  host-tested but still not wired into boot: the runtime path goes straight
  to FAT.
- A gate that looks at pixels. A check somebody has to remember to run is not a check.
- Input routing. The mouse moves a pointer and nothing else; keystrokes reach the shell rather than a focused window, so the GUI is a drawing rather than a window system.
- The GUI is kernel code. A ring-3 program cannot query the screen, present a buffer or read input.
- Driver isolation contract hardening (capabilities, fault containment).
- Dynamic loading/version negotiation for driver binaries (loader + signature path not yet implemented).
- Hardware probe lifecycle and hotplug policy.
- DMA/IOMMU policy, device reset/recovery and driver fault containment for runtime drivers.

## Next checkpoint
- Define a common runtime driver contract and isolate the existing hardware drivers behind it before adding hotplug or additional buses.
