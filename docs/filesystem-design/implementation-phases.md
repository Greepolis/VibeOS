# Filesystem Hierarchy Implementation Phases

These phases implement the namespace design; they do not mark existing storage
work complete. **Do not begin destructive install/format work before F4's
format and recovery decision is approved.** Each phase is independently
reviewable and leaves a bootable rescue path.

| Phase | Goal and work | Dependencies | Definition of done |
| --- | --- | --- | --- |
| F0 - inventory and contract | Reconcile current mount table, boot paths, Linux ABI, NT `/win` use and test-image paths. Define versioned directory, mount, volume and transition contracts; write migration inventory. | Existing VFS, test harness, docs/io | Every path has canonical target, owner, migration/retirement rule; no user-facing claim exceeds runtime evidence. |
| F1 - namespace on current root | Create target directories on disposable FAT image; mount `/run`, `/tmp`, `/dev`, `/proc` and test volumes consistently; move QEMU fixtures out of the production path. Add `/EFI` and `/DOCS` aliases only where required by regression. | F0, FAT/VFS path tests | Boot-to-CLI and Linux/NT regressions pass; tests prove mount-boundary traversal, permissions, alias behavior and fixture exclusion from release media. |
| F2 - mount and policy service | Give init/filesystem manager a versioned mount manifest, bounded retries, stable volume IDs, health/error reporting, per-service credentials and capability checks. Implement root/cwd/mount namespace lifecycle tests. | F1, native init, credentials/capabilities, VFS | Required/optional mount failures behave differently by policy; no wrong-volume fallback; mount/unmount with open descriptors is tested. |
| F3 - production root selection | Evaluate and approve a writable native root format. Build host formatter/inspector/recovery tools; implement persistent `/usr`, `/etc`, `/var`, `/home` volumes or subvolumes, metadata ownership and journal/recovery integration. | F2, block layer, selected format, tools | Real driver creates, mounts, writes, reboots, replays/repairs and reports faults; power-cut sweep covers actual filesystem metadata. No installer yet. |
| F4 - verified boot, install and rollback | Define signed volume/slot manifest, A/B or equivalent atomic system updates, ESP update protocol, root selection, anti-brick fallback and recovery environment. | F3, secure/measured boot decision, signing/release pipeline | Interrupted update at each write boundary boots old or new valid slot; rollback and recovery are end-to-end tested on QEMU disk images. |
| F5 - production hierarchy migration | Move native binaries/libraries to `/usr`, configuration to `/etc`, service state/logs to `/var`, per-user state to `/home`; migrate NT paths and Linux aliases. Remove obsolete bring-up paths only after coverage. | F4, init/service manager, runtime ABIs | Clean install and upgrade produce canonical tree; every compatibility alias has passing tests or is removed; no test fixtures in release image. |
| F6 - release qualification | Hardware/storage matrix, malformed-volume fuzzing, capacity/error injection, performance/latency baseline, recovery runbooks and data migration tools. | F3-F5 | Release gate covers supported devices and formats; no silent data-loss case; offline recovery and supported upgrade/rollback documented. |

## Required gates per phase

- Host tests for path normalization, mount resolution, access control, volume
  identity, open-handle lifetime and error mapping.
- QEMU tests on disposable disk images for boot, read/write, reboot
  persistence, interrupted writes and missing/corrupt volume behavior.
- Linux ABI regression for `openat`, `stat`, `getdents64`, links, permissions,
  `/proc/mounts` and `/dev`; Windows regression for DOS-device and DLL-path
  translation as that path moves.
- Fuzz/sanitizer coverage for partition tables, superblocks, directory entries,
  path components, journal replay and mount manifests.
- Artifact inspection proving production images contain no test-only payloads
  and the ESP is not used as general writable state.

## Critical ordering

F0-F2 can progress without choosing a native filesystem. F3 cannot. F4 depends
on F3 and a verified boot decision. Do not conflate “the hierarchy exists” with
“the storage is durable”, or “the journal recovers in a host model” with
“production root is crash-safe”. A phase is complete only with its runtime
evidence, sabotage/regression cases and updated implementation status.
