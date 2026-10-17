# Interaction and Visual System

See [visual-mockups.md](visual-mockups.md) for drawn screen compositions and
component states. This document defines the intended user-visible desktop. It
is a specification for the implementing agent and design review, not a
screenshot of a finished product. Preserve the existing VibeOS blue/graphite
identity while replacing the one-window bring-up layout with a cohesive
workspace.

## 1. Product feel

The desktop should feel like a calm, precise **systems workbench**: clear
hierarchy, restrained color, strong readable surfaces and direct manipulation.
It must not imitate Windows or macOS window chrome closely, and must not use
decoration to hide missing behavior. Prefer a compact, information-dense
workspace that makes system status and the active task legible without looking
like a debug console.

Keep the existing recognizable palette as a bridge:

| Token | Proposed value | Use |
| --- | --- | --- |
| `canvas` | `#202A38` | desktop/workspace background |
| `chrome` | `#121820` | top panel, menus and dark chrome |
| `surface` | `#F2F4F5` | standard application window content |
| `surface-raised` | `#FFFFFF` | dialogs and elevated cards |
| `text-primary` | `#101820` | text on light surfaces |
| `text-inverse` | `#FFFFFF` | text on dark chrome/accent |
| `accent` | `#3A6EA5` | active window, selection, primary action |
| `focus` | `#F3B33D` | keyboard focus ring and attention highlight |
| `danger` | `#A42932` | destructive/error state |
| `success` | `#16794B` | confirmed healthy/success state |

These are design tokens, not hard-coded per-widget colors. Contrast must be
measured for every foreground/background pair; target WCAG 2.2 AA for text and
controls, and never communicate state by color alone. High-contrast mode may
replace tokens without changing layout or semantics. A dark application theme
is a later theme, not a second independent design system.

Use flat, low-noise surfaces with a subtle geometric wallpaper constructed from
large clipped shapes rather than expensive blur or animated gradients. Keep
shadows shallow and deterministic. The terminal window remains a light text
surface for readability, with a clearly differentiated titlebar. Avoid
transparent glass effects on the software framebuffer path.

## 2. Desktop layout

### Persistent top panel

- Height: 40 logical pixels at 1x; content is vertically centered and never
  overlaps notches/edge constraints if those appear on future hardware.
- Left: VibeOS identity and active application/workspace context. The launcher
  is available from the bottom taskbar and the `Super` shortcut.
- Center: workspace switcher; application launch/running state belongs to the
  taskbar, not this panel.
- Right: clock, network/power/audio status and session menu. Each item has a
  text-accessible name, tooltip/accessible label and explicit failure state.
- The panel stays above ordinary windows. Full-screen windows may hide it only
  through an explicit user-approved mode; keyboard shortcuts restore it.

### Persistent bottom taskbar

- Height: 56 logical pixels at 1x, anchored to the bottom edge and kept
  separate from the 40 px system panel. Reserve its geometry; ordinary windows
  do not extend underneath it unless explicitly maximized/full-screen.
- Left-to-right order: application-grid button, pinned launchers, separator,
  running application/window buttons, flexible space, and a `Show desktop`
  control at the trailing edge. Keep the visual weight compact and avoid a
  second clock/network tray here.
- Pinned and running states are distinct: pinned apps remain when closed;
  running apps show a dot and accessible `Running` state; the active app has a
  filled surface plus a focus marker, not color alone. Multiple windows group
  by app and open a keyboard-navigable window list.
- Clicking a pinned app launches or activates it. Clicking the active running
  app minimizes it; clicking an inactive app activates its last focused window.
  Context actions include pin/unpin and close only when supported by the app.
- The taskbar never steals keyboard focus merely because a window is focused
  or a pointer crosses it. Keyboard traversal, tooltips and accessible names
  expose every action; minimum hit targets follow the accessibility scale.

### Workspace and launcher

- One workspace by default. Additional workspaces are created on demand and
  shown by name/number; switching preserves per-workspace window geometry and
  focus history.
- The launcher opens a searchable application grid with keyboard focus placed
  in search, recently used trusted apps and a visible system/settings section.
- Activating `Settings` from the launcher or taskbar opens the full Settings
  application. Quick settings remains a small status-panel popover and links to
  the relevant full Settings page; it is not a substitute for that application.
- First boot opens a Welcome/Help view and one terminal window, not a wall of
  diagnostic windows. QEMU tests may select a deterministic test scene.

### Full Settings application

- Provide a dedicated, searchable Settings window with a persistent category
  navigation rail and one detail page at a time. Initial categories: Display,
  Appearance, Input, Accessibility, Network, Storage, Users & Sessions,
  Updates, and About. Hide unavailable controls or mark them clearly as
  unavailable; never imply a change succeeded when its service is absent.
- Each setting has a visible label, current value, reset/default behavior and
  keyboard semantics. Changes with safe live preview apply immediately; risky
  display-mode changes use a bounded confirmation/revert timer. Destructive or
  security-sensitive changes require an explicit confirmation and authorization.
- Settings are per-user unless the control is explicitly system-wide. Show
  scope and required privilege before mutation, persist atomically, and report
  service errors with a retry/recovery path. Avoid an Apply button for routine
  reversible settings; use explicit Save only for grouped transactions that
  cannot be safely applied incrementally.
- The display page exposes detected mode, scale, orientation only when
  supported, and a safe revert path. Accessibility settings are reachable
  before login/session start where the platform permits.

| Category | Initial page contract |
| --- | --- |
| Display | Detected outputs/modes, scale, supported orientation, temporary test and automatic revert; never offer an unreported mode. |
| Appearance | Wallpaper, accent and supported theme tokens; preview is reversible and respects reduced-motion/high-contrast overrides. |
| Input | Keyboard layout/repeat and pointer speed/handedness only when the backend reports support; provide a recovery path if input becomes unusable. |
| Accessibility | UI/text scale, high contrast, reduced motion and pointer visibility/size; screen-reader controls appear only when a compatible service exists. |
| Network | Interface/link/address and DNS status; configuration is capability-gated and reports disconnected, applying, failed and connected states. |
| Storage | Volumes, capacity, read-only/error state and safe eject where supported; destructive format/repair actions require elevated confirmation. |
| Users & Sessions | User/session overview, authentication and startup mode; privileged changes require re-authentication and audit records. |
| Updates | Installed version, signature/verification state, available update and restart/rollback status; hide update actions until a trusted updater exists. |
| About | OS/build identity, architecture, licenses, hardware summary and exportable diagnostics with secrets removed. |

Search indexes category names and setting labels, respects the caller's
permissions, and opens the owning category with focus on the matched control.
Links from quick settings deep-link to the corresponding full page without
creating a second source of truth.

### Graphical and CLI-only session modes

- Offer two explicit session modes: `Desktop` and `CLI only`. The choice is a
  session/boot policy, not a shell command or a claim that both modes already
  exist. Keep Desktop as the normal proposed default once the graphical session
  is qualified; allow a boot-menu/configuration choice for CLI-only recovery.
- CLI-only starts the native shell on a text TTY and does not start the
  compositor, desktop shell, taskbar, or graphical apps. Kernel, storage,
  network and service-management components remain governed by their normal
  policies. Serial console mirroring remains available when configured.
- If framebuffer initialization or the compositor repeatedly fails, enter a
  readable text recovery session with a reason code and log correlation ID.
  Never show a blank screen or repeatedly restart the failed GUI. Do not claim
  automatic switching back to Desktop until the session manager can probe and
  restart it safely.
- Proposed restart guard for review: at most three `displayd` starts within
  60 seconds, with bounded backoff; then stop graphical retries and enter CLI
  recovery. A user-requested retry or an approved reboot policy may reattempt
  Desktop. The final threshold belongs in service policy, not UI code.
- When supported, `Ctrl+Alt+F1` selects the primary text TTY and a separate
  approved key chord can return to the graphical session. The exact chord,
  authentication behavior and whether switching is enabled are implementation
  gates; serial access is independent of this GUI key path.
- The CLI-only screen uses the same typography/contrast tokens for readable
  status, but has no simulated windows, pointer, taskbar or graphical chrome.
  It must support keyboard-only startup, login/session policy, shell help,
  status, logs, service recovery and a documented shutdown path.

### Window frame and states

- Standard window: 36 px titlebar, readable title and app identity, explicit
  minimize/maximize/close controls at the trailing edge, visible resize affordance
  on hover/focus. Close is visually dangerous only on hover/press, not a large
  red billboard at rest.
- Minimum interactive target is 40x40 logical pixels for primary touchable
  controls; dense titlebar controls may use a 32x32 hit region only if keyboard
  actions and tooltips provide an equivalent.
- Drag the titlebar to move; drag edges/corners to resize; double-click titlebar
  to maximize; snap preview appears only after a short edge dwell and can be
  canceled by moving away. No gesture is the only way to perform an action.
- Default stacking is click-to-focus. Focus changes are visible in the titlebar
  and focus ring; inactive windows remain readable. Modal dialogs are scoped to
  their parent and cannot block unrelated system recovery.
- Close asks only when the application reports unsaved work. The compositor
  never guesses whether a document is saved.

## 3. Input behavior

Keyboard is the reliable primary navigation path:

| Action | Proposed shortcut | Behavior |
| --- | --- | --- |
| Open launcher | `Super` | Search/apps menu; focus starts in search |
| Focus taskbar | `Super+T` | Move focus to taskbar; exact binding is an approval gate |
| Show desktop | `Super+D` | Minimize/restore ordinary windows; taskbar button equivalent |
| Switch apps | `Alt+Tab` | Cycle visible windows, release Alt to activate |
| Close active window | `Alt+F4` | Request close; app owns unsaved prompt |
| Maximize/restore | `Super+Up` | Toggle active window maximize |
| Snap left/right | `Super+Left/Right` | Tile active window in half-screen region |
| Move focus | `Tab` / `Shift+Tab` | Logical order; no focus trap except modal dialog |
| Switch workspace | `Super+1..4` | Switch/create configured workspace |
| Text recovery console | `Ctrl+Alt+F1` | Switch to text TTY where platform supports it |
| Lock session | `Super+L` | Secure only after authenticated lock exists; otherwise unavailable |

Shortcut modifier and exact workspace count are approval gates before ABI freeze.
System shortcuts are reserved by the session/compositor and cannot be captured
by applications. A visible keyboard shortcut appears in menus and help.

Pointer behavior: single click focuses; activation is consistent with the
focused control; context menus use the secondary button and keyboard menu key;
wheel/scroll is delivered to the hovered scrollable region without accidental
window focus changes. Pointer acceleration and handedness are user settings
when the input backend supports them. The pointer has a high-contrast outline
and a configurable size.

Text entry must support selection, copy/paste permission prompts, caret
visibility and IME-ready text events even if the first release only ships a
basic keyboard layout. Never expose raw scan codes as the app's text API.

## 4. Accessibility and localization

- Full keyboard operation, stable focus order, visible focus indicator and
  non-color state cues are required for every shipped control.
- Support 100%, 125%, 150% and 200% UI scaling where framebuffer dimensions
  permit; clipping and minimum window sizes are tested at each scale.
- Honor reduced-motion preference; all transitions have a no-motion mode and
  remain understandable without animation.
- Provide high-contrast palette and adjustable pointer/text size before general
  availability. Contrast is validated by automated token-pair checks.
- Every control exposes role, name, state, value and relationships through an
  accessibility tree protocol. The first-party screen reader can be staged, but
  the tree contract and keyboard focus semantics must exist before third-party
  apps depend on widgets.
- UI strings are externalized; text layout handles UTF-8, fallback fonts and
  right-to-left shaping as a later localization gate. Do not clip translated
  strings based on English-only fixed widths.
- The bundled 8x8 font remains a boot/recovery fallback, not the only font for a
  production desktop.

## 5. Motion, responsiveness and feedback

- Use short transitions (roughly 120-180 ms) for open/close/focus only when the
  frame budget allows. Respect reduced motion and never delay input while an
  animation runs.
- Menus and controls acknowledge activation immediately. Long operations show
  progress, allow cancellation when safe, and report a recoverable error with
  next action; no indefinite spinner without status.
- The compositor coalesces pointer movement but preserves key/button edges.
  Pointer movement must not repaint the full screen when only old/new cursor
  bounds changed.
- The software baseline is measured at the supported framebuffer modes. Initial
  target: responsive 1280x720 at 30 fps under normal desktop load, with p95
  input-to-visible response under 100 ms; higher modes must degrade gracefully
  and publish measured frame-time/memory data. These are acceptance targets to
  validate on QEMU and at least one physical reference device, not claimed
  current performance.

## 6. Essential states and recovery UX

Design and test these states, not only the happy-path desktop:

- first boot/no user configured;
- session start and service readiness;
- display unavailable or unsupported mode;
- keyboard available but mouse absent, and vice versa;
- compositor unavailable/restarting;
- application not responding or crashed;
- storage full/read-only and removable device failure;
- network disconnected and reconnecting;
- permission denied, authentication required and locked session;
- low-memory mode with optional effects disabled.

An error dialog must not be the only route out of a failed service. Provide a
keyboard action to dismiss/recover, preserve work where possible, and log a
correlation ID that a user can show to support. Kernel panic and secure-lock
views are separate trusted paths with explicit policy.
