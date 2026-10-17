# VibeOS Filesystem Namespace and Storage Design

**Status:** Proposed design for implementation review; no runtime behavior is
changed by this document.
**Audience:** filesystem/VFS, init, compatibility, security and installer
implementers.
**Scope:** the directory hierarchy and its integration with VibeOS services,
personalities, storage and updates. Concrete on-disk format selection remains a
separate, gated decision.

## Executive decision

Use one predictable, case-sensitive POSIX namespace for native VibeOS and the
Linux personality, with a conventional merged-`/usr` layout. Mount points and
per-process root/current-directory state remain VFS concepts, not filesystem
driver details. Keep the EFI System Partition as boot media mounted at
`/boot/efi`; do not make the ESP the permanent writable system root. Keep
mutable state under `/var`, user data under `/home`, and volatile state under
`/run` and `/tmp`.

Windows path and case semantics are implemented by the NT personality's path
translator and volume mapping, not by making the shared VFS globally
case-insensitive. The existing `/win` path and `/EFI/BOOT` test paths are
transitional compatibility locations, not permanent system layout.

The current implementation is a bring-up state: the running root is FAT, the
boot payload lives under `EFI/BOOT`, `/tmp`, `/dev` and `/proc` are mounted, and
filesystem drivers have materially different runtime evidence. Do not move the
root, format user storage, or remove these paths as part of a hierarchy-only
change.

## Documents

| Document | Purpose |
| --- | --- |
| [hierarchy.md](hierarchy.md) | Canonical directories, ownership, mode and content policy |
| [integration.md](integration.md) | VFS, mounts, boot, security, Linux and Windows path integration |
| [implementation-phases.md](implementation-phases.md) | Sequenced work, dependencies, tests and phase exit criteria |

## Normative language

**MUST**, **MUST NOT**, **SHOULD**, **SHOULD NOT** and **MAY** are requirements
for the target design, not claims about the current implementation. Any
exception affecting ABI, persistent data, trust boundaries or update/recovery
must be recorded as a decision before implementation.

## Current baseline and gaps

- `kernel/fs/path.c` provides normalized paths, a process root/current working
  directory and mount-aware lookup. Linux `openat`-family calls use this path.
- The active boot root remains FAT; `/tmp` is tmpfs, `/dev` devfs and `/proc`
  procfs. Test images can be mounted at paths such as `/ext2`, `/iso`, `/ntfs`,
  `/exfat` and `/fatlong`.
- Runtime Windows files are staged under `/win`; ELF/PE test payloads remain
  under `EFI/BOOT`. These locations are convenient for bring-up and must not be
  mistaken for the production hierarchy.
- ext2, ISO9660, exFAT and NTFS are read-only in the documented baseline. FAT
  metadata is not yet proven to use the journal end-to-end. A tested journal
  mechanism is not the same as a crash-safe mounted root.
- A/B installation, a selected native writable root format, root switching,
  mount namespaces and production account provisioning are not yet complete.

## Target principles

1. **Stable paths, independent storage.** Applications consume paths and file
   descriptors through the VFS. No application depends on a device index, FAT
   cluster, partition number or boot-image staging path.
2. **Small, legible hierarchy.** Use familiar locations; do not invent parallel
   trees such as `/apps`, `/packages` or `/system` when `/usr`, `/opt` and
   `/var` already express the distinction.
3. **Immutable system, explicit state.** Installed OS files are verified and
   updated as a coherent slot; machine state and user data are not overwritten
   by an OS update.
4. **Personality-specific behavior at the boundary.** Case folding, drive
   letters, separators, error mapping and path-length rules belong to ABI
   translators, while VFS enforces object identity and access policy.
5. **Fail closed, recover visibly.** Missing or invalid mounts must not silently
   resolve to the boot volume. Diagnostics identify the mount, volume UUID,
   filesystem and error reason.
6. **No false durability claims.** A successful write is durable only after the
   filesystem, journal/barrier contract and device flush semantics prove it.

## Unresolved approval gates

The directory namespace is proposed as stable design. Before writing a
production installer or persistent root, the maintainer must approve: the native
filesystem format and its recovery policy; whether system and state share a
physical volume; and the supported first-install/rollback policy. The
implementation phases make these gates explicit rather than guessing in code.
