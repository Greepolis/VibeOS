# Desktop and GUI Implementation Phases

The desktop is not one feature-sized change. These phases are ordered by
trust-boundary and runtime dependency. Every phase keeps serial/text recovery
working and records whether behavior is host-modelled, QEMU-enforced or
hardware-verified.

| Phase | Goal and principal work | Dependencies | Definition of done |
| --- | --- | --- | --- |
| U0 - baseline and contract | Inventory `kernel/io/gui.c`, GOP mapping, PS/2 input, terminal/TTY, QEMU screenshots and service lifecycle. Specify Desktop and CLI-only session policy, GUI failure fallback, display metadata, normalized input, ownership and protocol threat model. | Existing graphics, input, IPC, VFS and init documentation | Current behavior and unsupported claims are separated; deterministic reference scenes and metrics cover desktop, taskbar, Settings and CLI-only mode; no app ABI is frozen prematurely. |
| U1 - display/input boundary | Extract tested pixel/layout/composition primitives. Define a versioned display backend and normalized seat event contract. Add USB HID planning/driver path; keep PS/2 and serial usable. | U0, device framework, capability/IPC model | Host tests cover clipping, pitch/pixel formats, event order and hot-unplug; QEMU still shows the existing terminal and text rescue still works. |
| U2 - user-space compositor skeleton | Start `displayd` under init supervision. Move the existing terminal surface and panel rendering to a user-space process with restricted framebuffer-present capability; restart it after injected crash. | U1, native init/service supervisor, shared memory, process cleanup | `displayd` crash does not crash kernel or lose shell session; recovery/restart is bounded; serial fallback remains available; no normal app writes framebuffer memory. |
| U3 - window and client protocol | Implement versioned client/session/output/surface objects, buffer attach/commit, damage, configure, focus and input routing. Add `libvibeui` minimal client API and protocol fuzzing. | U2, capability handles, VM shared pages, user pointer validation | Two independent ring-3 clients display isolated buffers; one cannot read, forge input for or destroy the other; malformed clients are rejected without wedge. |
| U4 - desktop shell and native apps | Add launcher, persistent bottom taskbar, task/workspace model, terminal client, file manager, full Settings control panel and system status app. Apply the visual tokens, keyboard shortcuts and filesystem paths from the companion specs. | U3, filesystem hierarchy F1/F2, native userland, fonts/text | End-to-end scripted keyboard/pointer scenario opens, focuses, moves, resizes, closes, switches workspace, pins/activates apps, changes a setting with safe revert and uses files; errors are visible and logged. |
| U5 - security and accessibility | Integrate authenticated sessions/lock, per-app policy, clipboard/screenshot permissions, accessibility tree, scaling, high contrast, reduced motion and localization plumbing. | U4, identity/authentication, security policy, accessibility review | Negative tests prove isolation and lock privacy; keyboard-only path completes all core tasks; automated contrast/focus/tree checks pass. |
| U6 - performance and resilience | Optimize damage tracking, buffer memory, frame scheduling and input latency; add watchdog, restart limits, logs, low-memory mode and hardware display/input matrix. Verify CLI-only boot and automatic text recovery after display/compositor failure. Add GPU backend only if profiling justifies it. | U5, observability, stable device hosts | Stress and soak tests show bounded memory/no unhandled crash; text recovery remains usable without graphics; published benchmark meets approved reference-device targets; GPU fallback preserves software correctness. |
| U7 - release qualification | Threat review, protocol fuzzing, visual/accessibility review, compatibility docs, upgrade/session recovery and support diagnostics. | U6, release engineering | Required QEMU and physical-device gates pass; every shipped app has keyboard/accessibility audit; failure recovery and security review signed off. |

## Test strategy and quality gates

### Unit and host tests

- pixel layout for every supported firmware pixel format, pitch, odd width,
  minimum/maximum size and invalid framebuffer range;
- geometry, clipping, z-order, damage-region union and resize boundary tests;
- protocol version/length/overflow/handle ownership and stale-generation cases;
- input normalization, key repeat, dropped/coalesced event rules, hot-unplug
  releases and multi-device ordering;
- theme contrast, focus order, accessibility names/roles and scale clipping.

### QEMU integration

- OVMF/GOP boot at deterministic modes, screenshot and serial artifacts;
- screenshot assertions over semantic regions (panel, window, text surface,
  cursor/background), not only file existence or color count;
- scripted keyboard and mouse interactions through QEMU input injection;
- two-client surface isolation, compositor/client crash, restart, missing
  framebuffer, missing mouse and terminal fallback;
- bounded input-to-visible latency, frame duration, allocation and dropped-event
  counters included in summary JSON.

### Security, fuzz and soak

- fuzz display/input IPC, dimensions/stride, damage rectangles, event ordering,
  clipboard requests and app metadata under sanitizers on host;
- fault injection for process exit during buffer attach/commit, memory pressure,
  device removal and service restart;
- soak window open/close, workspace changes, pointer motion and client churn;
- test that a client cannot capture global keystrokes, screenshot locked content,
  map another client's buffer or persist after its capability is revoked.

### Visual review

Automated screenshot checks guard gross regressions but do not replace human
visual review. Keep deterministic fixtures and record QEMU mode, renderer and
font version with each image. Review at supported scale/mode and with keyboard,
mouse-only and reduced-motion scenarios. A manual review is not reported as an
automated gate.

## Release policy

Do not mark the GUI production-ready when only the boot-time terminal window
renders. Production readiness requires U5 security/accessibility and U7 release
criteria, including at least one physical reference display/input combination,
verified crash recovery, supported session-authentication policy, protocol
fuzzing and a measured software or hardware renderer baseline. Features that
remain unsupported must be discoverable and documented rather than silently
degrading.
