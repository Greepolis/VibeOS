# VibeOS Desktop and User Interface Design

**Status:** Proposed product/architecture specification for implementation
review; it does not claim the target desktop exists.
**Audience:** desktop/compositor, input, userland, session, accessibility,
security and application developers.
**Target:** x86_64 UEFI, software-composited framebuffer first, with a reliable
text/serial recovery path.

## Executive decision

Evolve the existing framebuffer desktop into a **single-seat, user-space
compositor and window manager**. Applications own client surfaces, not display
hardware. A versioned, capability-protected IPC protocol carries surface
buffers, damage, configure events, keyboard/pointer input and lifecycle. The
kernel and device hosts provide bounded display/input primitives; desktop policy,
window state, rendering and shell UX live outside the kernel.

The initial graphical release is intentionally narrow: one local session, one
GOP framebuffer, software composition, mouse and keyboard, a small set of
first-party apps, and serial/text rescue. GPU acceleration, multi-seat, remote
desktop, HDR and broad third-party compatibility are later capabilities, not
prerequisites for a coherent first desktop.

## Current state: do not overclaim

VibeOS already has a real but minimal kernel-side desktop in `kernel/io/gui.c`:
it draws a panel, a single terminal window and a PS/2 pointer into a back buffer
using the firmware framebuffer. The terminal shares the shell session with the
serial console. `scripts/dev/screenshot.py` can inspect QEMU pixels, but the GUI
is not currently a required automated visual gate. The implementation has no
general window protocol, independent application surfaces, desktop session,
GPU acceleration, broad accessibility support or production input-device
coverage. See [../graphics.md](../graphics.md) for the observed path and its
limitations.

## Documents

| Document | Purpose |
| --- | --- |
| [architecture.md](architecture.md) | Components, IPC, surfaces, ownership, lifecycle and security boundaries |
| [interaction-and-visual-system.md](interaction-and-visual-system.md) | Product behavior, visual language, keyboard/pointer UX and accessibility |
| [implementation-phases.md](implementation-phases.md) | Sequenced work, dependencies, tests and release acceptance |

## Product requirements

1. **Safe failure.** A crashed client cannot crash the compositor or kernel; a
   compositor restart returns to a usable login/recovery view, and serial/text
   access remains available independently.
2. **Keyboard-first, pointer-complete.** All desktop and window actions have
   visible focus and keyboard equivalents. Pointer users can move, resize,
   focus and close windows without precision-only gestures.
3. **Predictable state.** Focus, z-order, workspace, window geometry, display
   scale and session transitions have explicit ownership and deterministic
   recovery behavior.
4. **Least privilege.** Applications cannot read another surface, capture
   global input, forge identity or address the framebuffer without explicit
   grants.
5. **Observable and testable.** The compositor reports frame timing, dropped
   events, surface memory, client failures and display mode. Automated tests
   validate both protocol semantics and rendered output.
6. **Honest capability tiers.** The first software path is benchmarked and
   bounded. Features not supported by its backend are hidden or reported as
   unavailable rather than simulated as success.

## Decisions that need approval before ABI freeze

The architecture and interaction defaults below are proposals. Before freezing
the public window protocol or shipping a user session, the maintainer should
approve: session authentication/lock integration; the permanent shortcut
modifier; whether the system launches directly to a desktop or a login/session
chooser; and the minimum supported physical display/input hardware. The first
implementation may use a documented development-session policy, but it must
not present that policy as production authentication.
