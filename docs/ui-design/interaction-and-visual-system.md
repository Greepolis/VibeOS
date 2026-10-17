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
- Left: VibeOS launcher button and active application/workspace context.
- Center: workspace switcher and running-app indicators; no tiny status glyphs
  as the only control.
- Right: clock, network/power/audio status and session menu. Each item has a
  text-accessible name, tooltip/accessible label and explicit failure state.
- The panel stays above ordinary windows. Full-screen windows may hide it only
  through an explicit user-approved mode; keyboard shortcuts restore it.

### Workspace and launcher

- One workspace by default. Additional workspaces are created on demand and
  shown by name/number; switching preserves per-workspace window geometry and
  focus history.
- The launcher opens a searchable application grid with keyboard focus placed
  in search, recently used trusted apps and a visible system/settings section.
- A bottom task shelf is transient and appears on launcher/task-switcher use or
  an edge gesture later; do not permanently add a second bar to compete with the
  top panel.
- First boot opens a Welcome/Help view and one terminal window, not a wall of
  diagnostic windows. QEMU tests may select a deterministic test scene.

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
