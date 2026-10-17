# Desktop Architecture and Protocol Contracts

## 1. Component model

```text
Applications
  | libvibeui: widgets, accessibility tree, text/input, app lifecycle
  | versioned UI IPC + capability-scoped shared-memory buffers
  v
displayd: compositor + window manager + desktop shell
  | normalized input events / display present
  v
device host or kernel display/input boundary
  | GOP framebuffer initially; PS/2 today, USB HID required for broad hardware
  v
platform hardware

session manager -> starts displayd and user apps, owns session policy
init/service manager -> supervises session manager and displayd
serial/TTY path --------------------------> independent recovery console
```

The current `kernel/io/gui.c` renderer is a bring-up implementation. First
extract and test its rendering semantics; then move desktop policy and
composition into `displayd`. Do not expose that kernel UI structure as a stable
application ABI. The kernel may retain a minimal panic display, but ordinary
windowing must not call kernel drawing functions from applications.

### Responsibilities

| Component | Owns | Must not own |
| --- | --- | --- |
| Kernel/display backend | framebuffer mapping or present operation, mode metadata, device lifetime, access checks | Window policy, application menus, widget toolkit, user identity UI |
| Input backend/host | hardware probing, raw scan/HID decoding, hotplug, normalized events, device capabilities | Window focus, application shortcuts, text field behavior |
| `displayd` | surfaces, composition, windows, focus, workspaces, pointer routing, desktop shell | Filesystem authority, arbitrary syscall policy, application credentials |
| Session manager | login/session lifecycle, lock/idle/power policy, session-owned services | Pixel composition and per-widget rendering |
| `libvibeui` | client protocol, widgets, accessibility semantics, drawing helpers | Direct framebuffer or global input access |
| Applications | their own content, declared actions, preferences/data in allowed paths | Another client's buffers, global keyboard capture, privileged device access |

The first runtime may keep a narrow display driver in kernel space while the
user-space boundary is built. This is a staging exception under
ADR-0006, not a decision to make the desktop part of the kernel permanently.

## 2. Client and surface model

Each connection is bound by the kernel to a process identity, session and
capability set. The compositor MUST derive identity from the connection; it
MUST NOT trust a client-provided PID, privilege label, app ID or ownership
claim.

A client can request a bounded set of operations:

- create/destroy a top-level surface;
- attach a shared-memory buffer handle and a generation number;
- commit a frame with one or more damage rectangles;
- set a bounded title, application identity and declared window actions;
- request a geometry/state transition and receive a compositor `configure`;
- receive focus, pointer, keyboard, close, scale and lifecycle events.

The compositor decides final geometry, stacking, focus, visibility, resize and
input routing. A client never chooses global z-order or absolute input capture.
Popup/grab behavior is time-limited, tied to a user gesture and revocable.

Protocol requirements:

- fixed-width, versioned messages; length/version validation before reading
  payloads; deterministic errors for unsupported versions and operations;
- explicit maximum clients, surfaces, dimensions, buffer bytes, queued events,
  damage rectangles and title/app-ID lengths;
- handles are capability-bearing objects with transfer rights and lifecycle;
  buffer revocation/unmap occurs when a client dies;
- a buffer may be read only by its owner and compositor; stale commits are
  rejected by generation/sequence number;
- every rectangle is overflow-checked and clipped before address calculation;
- queue overflow is counted and has a specified coalescing policy (pointer
  motion may coalesce; key transitions and button up/down must not be lost);
- protocol parsing is fuzzed as an untrusted boundary.

The wire format should be inspired by small object/event protocols, not be a
source-compatible clone of Wayland. Avoid a giant global opcode namespace;
separate display, surface, seat/input and session objects and version them
independently. Keep application calls behind `libvibeui` so protocol revisions
do not force every app to change.

## 3. Composition and display backend

The initial backend consumes the UEFI GOP framebuffer metadata already passed
at boot. It must validate pixel format, pitch, dimensions, address range and
integer arithmetic. The kernel/device host owns mapping and lifetime; only the
compositor receives a read/write mapping capability. Applications receive
ordinary shared-memory buffers, never the hardware framebuffer.

Rendering pipeline:

1. Receive client commits and normalized input without holding a device lock.
2. Snapshot window/surface state under a short compositor-state lock.
3. Compute damage from old/new bounds, client damage and cursor movement.
4. Clip damage to output and window bounds; redraw intersecting layers into a
   software back buffer.
5. Present only dirty regions when the backend supports it; otherwise copy a
   full frame at a bounded cadence. Never draw from a timer interrupt.
6. Record render/present duration, dirty pixels, missed frame budget and backend
   errors.

Use at least one back buffer for tear-free software composition. Buffer count
and memory limits are selected by measured resolution and memory budget, not an
unbounded per-window allocation. The first release supports one output and a
single 1x scale factor. The protocol includes output IDs and scale metadata so
multi-output and high-DPI can be added without changing surface identity.

Do not promise vsync until the backend can signal it. A software GOP backend
may have tearing and a lower frame rate; report the capability and meet the
measured baseline. A future GPU backend is introduced behind the same present
contract and justified by profiling.

## 4. Input, sessions and focus

The input contract is normalized events: device/seat ID, monotonic timestamp,
event sequence, key code, text value where appropriate, pointer position/delta,
buttons, scroll and capability flags. Hardware scan codes do not cross into
application APIs. The existing PS/2 keyboard/mouse can be the first backend;
USB HID keyboard and pointer support, hotplug and device removal are required
before broad physical-hardware production claims.

`displayd` owns per-seat focus, pointer location, key repeat and workspace
routing. The session manager owns session creation, lock, idle and user switch.
Applications receive keyboard events only when focused, unless an explicit
system-granted accessibility/shortcut capability applies. Secure attention and
screen lock cannot be intercepted by an ordinary app.

Required lifecycle rules:

- hot-unplug cancels in-flight device reads and releases pressed keys/buttons;
- client exit revokes surfaces, pending input and buffer references before PID
  reuse;
- compositor restart invalidates stale connection handles and restores a safe
  default desktop from session state, not from client memory;
- session lock obscures all application surfaces, revokes clipboard/screenshot
  access and requires authenticated unlock; until authentication exists, lock is
  not represented as secure;
- serial console and recovery TTY remain usable when graphics initialization,
  input devices or `displayd` fail.

## 5. Security and privacy

- Use kernel-authenticated IPC identity and capability checks for display,
  input, screenshot, clipboard and session-control objects.
- Isolate clients' buffers; validate dimensions, stride, pixel format and
  mapped length at attach and every commit generation.
- Do not allow a client to observe another process's window contents, title,
  keystrokes or clipboard without a user-mediated grant.
- Clipboard is an explicit transfer between focused clients. No background
  polling, silent clipboard read or persistence by default.
- Screenshots and screen recording require a visible user action/permission;
  protected/lock surfaces are excluded unless a privileged diagnostic mode is
  explicitly enabled.
- Denial, protocol abuse, client crash, allocation exhaustion and backend fault
  are rate-limited and observable; malformed client traffic cannot wedge the
  system service or block input indefinitely.
- The compositor runs with the minimum file/device permissions needed; apps
  cannot access raw `/dev` display/input resources directly.

## 6. Native application contract

`libvibeui` should provide versioned app startup metadata, top-level window,
layout primitives, buttons/lists/text input, menus/dialogs, clipboard requests,
accessibility nodes, theme tokens, input dispatch and orderly shutdown. Keep
the UI toolkit outside the kernel and avoid application dependence on private
compositor structs. Filesystem selection dialogs use the VFS and capability
broker; they do not receive ambient read access to every user's files.

Core first-party apps for the first useful desktop:

- **Terminal:** existing shell session shown as a graphical client; serial and
  graphical views may attach to one session only with explicit session policy.
- **Files:** navigate, list, copy/move/delete with confirmation for destructive
  actions, mount visibility, and clear read-only/error states.
- **Settings:** display scale/resolution (only available modes), input repeat,
  theme/accessibility options, network status and session controls.
- **Launcher/task switcher:** discover installed applications from trusted
  manifests, not by executing every file found in a writable directory.
- **System monitor/log viewer:** read-only, permission-aware service/process
  status and diagnostics.

Applications and user data follow [the filesystem hierarchy design](../filesystem-design/README.md).
Files and settings use stable per-user locations; executable discovery comes
from signed/system manifests or managed package metadata, not desktop icon
paths.
