# Selectable desktop environments

**Status:** proposed, for the maintainer's review. Nothing here is implemented.
**Origin:** [discussion #153](https://github.com/Greepolis/VibeOS/discussions/153).
**Place in the order:** after the GUI, item 4 of
[next-steps.md](../next-steps.md). This document does not move it earlier.

## The proposal in short

- **Wayland first.** X11 programs arrive through Xwayland. VibeOS does not ship
  a bare X server as a first target.
- **One display path for every session.** Minimal DRM/KMS with dumb buffers and
  evdev, the interface item 4 already decided. No desktop gets a private route
  to the hardware.
- **The order follows the cost.** A wlroots compositor first, then Xfce, then
  KDE Plasma, then GNOME. Each step has its own exit criteria and can be the
  place the work stops.
- **A session is a file.** The standard `wayland-sessions` entries list what is
  installed. VibeOS's own desktop stays the default.
- **The text console is never removed.** It is where a failed session lands,
  and serial stays independent of all of it.

## What exists today

Checked against the tree at `7cef7466`. `kernel/abi/linux_syscalls.def` is the
source for each system call's state.

| Need | State | Evidence or gap |
|---|---|---|
| Dynamic programs, threads, futexes | present | musl and glibc run; `futex` lacks only priority inheritance |
| `epoll`, `poll`, `eventfd`, `timerfd`, `inotify` | present | marked DONE |
| Unix sockets, `SCM_RIGHTS`, `SO_PEERCRED` | present | `kernel/abi/files/unixsock.c`, `kernel/abi/linux/net.c` |
| `SCM_CREDENTIALS` | partial | read past on send, never delivered |
| `memfd_create` | partial | `F_ADD_SEALS` refused |
| Shared file mappings | partial | refused on FAT with `ENODEV`, so a desktop cannot live on today's boot root |
| `/dev/dri/card*`, DRM ioctls | **absent** | `kernel/fs/devfs.c` has no display node |
| `/dev/input/event*`, evdev ioctls | **absent** | PS/2 input goes to the console and the kernel desktop |
| `/dev/fb0` | **absent** | the framebuffer is reached only by `kernel/io/gui.c` |
| VT and KD ioctls (`VT_SETMODE`, `KDSETMODE`) | **absent** | `ioctl` covers the console's modes, size, process group and descriptor requests |
| `/sys` | **absent** | a directory with nothing mounted ([inventory](../filesystems/hierarchy/inventory.md)) |
| `AF_NETLINK` | **absent** | `socket` accepts only `AF_UNIX` and `AF_INET` |
| Capabilities, namespaces, `seccomp` | **absent** | `capget`, `capset`, `seccomp` MISSING (L9); `unshare`, `setns` DEFERRED |
| A persistent root with a real `/usr` | **absent** | item 2 (F3) and item 3 (the installer) |

The list in #153 was close, and most of its system calls are already there. The
larger gaps are the ones it did not name: `/sys`, device discovery, VT
switching and a root that can hold a desktop.

## The stack

```text
 session chooser: /usr/share/wayland-sessions/*.desktop
 ------------------------------------------------------
 VibeOS desktop | labwc | Xfce | KDE Plasma | GNOME
 ------------------------------------------------------
 Xwayland (X11 programs)       D-Bus, session services
 libwayland, libinput, libseat, libdrm, libudev
 ------------------------------------------------------
 Linux ABI: DRM/KMS, evdev, minimal /sys, seat support
 ------------------------------------------------------
 GOP framebuffer, PS/2 (later virtio-gpu, virtio-input)
```

Everything above the Linux ABI line is existing software. The kernel gains only
the interfaces below that line, each one used by more than one desktop.

## Choices

### Wayland or X11

**Wayland first.** Item 4 has already chosen it, Windows' window system (item 5)
is planned as a client of the same compositor, and KDE and GNOME both treat it
as their main path. Xwayland covers X11 applications without a second display
stack in the system.

The cost falls on Xfce, whose Wayland support is the youngest of the three. If
it proves insufficient, its fallback is an X11 session hosted by Xwayland, not
a native X server on KMS.

### The compositor for item 4

This remains the maintainer's decision, listed under "Decisions waiting" in
[next-steps.md](../next-steps.md). The recommendation is **labwc on wlroots**.

| Candidate | For | Against |
|---|---|---|
| **labwc** (wlroots) | stacking windows, as VibeOS's desktop is designed; layer-shell lets our panel stay a separate client; software rendering through pixman | follows wlroots' API changes |
| Weston | reference compositor, pixman renderer | a less natural host for a desktop of our own |
| Sway (wlroots) | mature, same base as labwc | tiling is not the designed interaction |
| cage (wlroots) | very small, one full-screen client | not a desktop; useful as the first bring-up test |

The dependencies, their versions and the calls they really make must be
measured when item 4 is planned. The table is a starting position, not a
result.

### Device discovery

libinput and wlroots discover devices through libudev. A full udev daemon would
need netlink and a faithful sysfs. The proposal is **libudev-zero** with a
small read-only `/sys` for `drm` and `input` devices, and no `AF_NETLINK` until
a program in the plan proves it cannot run without it. Hotplug is out of scope
for the first output and the first input devices.

### Seat

**seatd**, with `libseat`, rather than a logind replacement. Bring-up may use
libseat's no-op backend with the compositor holding the devices directly, as a
development configuration only: it does not meet the least-privilege
requirement of the [design](README.md). KDE and GNOME expect logind, so
**elogind** is evaluated at those phases and not before.

### The built-in GUI

`kernel/io/gui.c` stays until the Wayland desktop has passed its own gate, then
shrinks to a boot and panic console. The kernel does not keep a second desktop.
A drawn console is still needed for a machine with no serial port.

### Choosing a session

- Installed sessions are the `.desktop` files in
  `/usr/share/wayland-sessions`. A desktop's normal packaging already provides
  them.
- The system default is in `/etc/vibeos/session`; a user's choice is in
  `~/.config/vibeos/session` and wins over it.
- `login` stays as [the shell's design](../shell/design.md) has it. A graphical
  greeter is a later, separate decision.
- `text` is always a valid session and needs nothing from the graphics stack.
- A session that exits with failure twice within 30 seconds is not restarted:
  the user lands on the text console with the reason printed. The numbers are
  initial values, from the "bounded restart" in [architecture.md](architecture.md).

### Shipping them

No image should carry every desktop. The base image holds the VibeOS desktop
and the compositor stack, and each further desktop is an optional tree on the
F3 root.

- **The compositor stack** is built from pinned sources against musl, as
  `scripts/dev/corpus-build.sh` does for bash, SQLite and Lua.
- **Xfce, KDE and GNOME** are first evaluated from a distribution's binary
  packages, because building them from source in CI is a project of its own.
  Alpine is the natural candidate since it is musl-based. A glibc distribution
  is the comparison when a failure may be a musl difference.

Whether users get them through a package manager is not decided here.

## Phases

Each phase depends on the one before it. DE0 and DE1 are also the delivery of
item 4, and belong to its plan once that is written.

| Phase | What | Exit criteria |
|---|---|---|
| **DE0** Display and input | `/dev/dri/card0` with one connector, one CRTC and dumb buffers over the GOP framebuffer; evdev for PS/2 keyboard and mouse | A test program in the guest sets a mode, flips two dumb buffers and reads injected key and pointer events; the same program passes on Linux |
| **DE1** First compositor | Minimal `/sys`, libudev-zero, libseat, cage and then labwc under pixman | A Wayland client commits a known pattern and it is read back; killing the client leaves the compositor running |
| **DE2** Session selection | Session files, defaults, the failure path, Xwayland | Each of `text` and labwc starts from its setting; a session made to crash reaches the text console; an X11 client displays |
| **DE3** Xfce | D-Bus, session services, the desktop on the F3 root | The session starts, a terminal opens and runs a command, the session logs out and returns cleanly |
| **DE4** KDE Plasma | elogind, Mesa llvmpipe, Qt | The same scenario as DE3 |
| **DE5** GNOME | Whatever DE4 left open, and its service dependencies | The same scenario as DE3, or a written decision not to support it |

**Stopping at DE5 is a legitimate outcome.** GNOME is the most tightly bound to
systemd of the three. If it needs an init system VibeOS does not intend to be,
this document should record that and the discussion should say so. It should not
be pursued through a growing set of stubs.

Software rendering is the expected limit for DE4 and DE5 until there is a GPU
path. Their criteria measure whether the session works, and its frame time is
reported rather than gated.

## How it is gated

The GUI is not gated today, and #153 asks how that changes. The rule from
[graphics.md](../graphics.md) stands: no comparison of whole screens, which
would break on every font or theme change.

1. **State, as the boot gate already does.** The compositor is up, its socket
   accepts a client, the session reports ready, no client crashed.
2. **A known pattern, read back.** A small client draws fixed colours in a
   fixed rectangle. The gate reads that rectangle from QEMU's screen dump and
   compares it within a tolerance. This catches black screens, a wrong pixel
   format and a wrong pitch, the failures `graphics.md` says nothing would
   catch now.
3. **Injected input.** QEMU sends a key and a pointer move; the client reports
   receiving them.
4. **Differential.** The DE0 program prints what the DRM and evdev interfaces
   answered, and the output is compared with the same program on Linux, the way
   the corpus compares workloads.

DE0 to DE2 belong on every push once they exist. DE3 to DE5 are too heavy for
that and belong in the nightly. Each gate is shown red by sabotage before it is
trusted: swap the pixel format, drop an input event, make the session crash.

## Not in scope

- GPU acceleration, multiple outputs, HiDPI, multi-seat, remote desktop.
- Hotplug of displays or input devices.
- Display managers and graphical login.
- A native X server on KMS.
- systemd as VibeOS's init.
- Applications' own sandboxes (Flatpak and similar), which wait for item 6.

## Decisions for the maintainer

1. Accept Wayland first, with X11 only through Xwayland?
2. Accept labwc on wlroots as item 4's compositor?
3. Accept libudev-zero and seatd, so no netlink or logind for the first phases?
4. Should the kernel desktop shrink to a console after DE2, as proposed?
5. Is it acceptable that GNOME may end in a decision not to support it?
