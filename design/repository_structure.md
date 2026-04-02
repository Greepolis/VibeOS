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
|   |-- drivers/
|   |-- fs/
|   |-- init/
|   |-- lib/
|   |-- net/
|   |-- sec/
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

Only the documentation-oriented directories are required immediately. Implementation directories are listed now to stabilize the future repository shape before code lands.
