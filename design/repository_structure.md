# Repository Structure

## Proposed top-level layout

```text
/
|-- README.md
|-- docs/
|-- design/
|-- roadmap/
|-- research/
|-- kernel/
|   |-- arch/
|   |-- core/
|   |-- ipc/
|   |-- mm/
|   |-- object/
|   |-- platform/
|   |-- proc/
|   |-- sched/
|   |-- time/
|-- boot/
|-- user/
|   |-- compat/
|   |   |-- linux/
|   |   |-- macos/
|   |   |-- windows/
|   |-- devmgr/
|   |-- drivers/
|   |-- fs/
|   |-- init/
|   |-- lib/
|   |-- net/
|   |-- sec/
|   |-- servicemgr/
|   |-- shell/
|-- tools/
|-- tests/
|-- scripts/
|-- third_party/
```

## Directory intent

- `boot/` bootloader and boot protocol code
- `kernel/` privileged kernel implementation
- `user/` system services and native userland
- `tools/` build helpers, image tooling, diagnostics
- `tests/` unit, integration, emulator, and conformance tests
- `scripts/` CI-oriented automation and local development helpers
- `third_party/` strictly controlled external dependencies

## Phase 1 note

Phase 1 already materializes the main implementation roots with placeholder files so the intended structure is tracked in Git before substantive code lands.
