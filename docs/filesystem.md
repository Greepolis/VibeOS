# Filesystem

## Goals

- modern reliability and integrity model
- efficient metadata operations
- support for snapshots and crash recovery in future phases
- clean separation between VFS and concrete filesystem implementations

## Architecture

### Virtual filesystem layer

The VFS provides:

- path resolution
- mount management
- file handle abstraction
- permission and policy hooks
- filesystem-neutral caching interfaces

### Filesystem hosting model

The preferred architecture is:

- VFS and page-cache integration in privileged space or tightly controlled kernel services
- concrete filesystems hosted as isolated services when practical

This allows experimentation without placing all on-disk parsing logic in the kernel.

## Native filesystem direction

The native filesystem, tentatively named `AuroraFS`, should target:

- copy-on-write metadata updates
- checksummed metadata
- optional checksummed data
- extent-based allocation
- snapshot support
- efficient small-file performance

## Compatibility strategy

### Where this now stands

Five filesystems are implemented, each behind the same `vibeos_fs_ops_t`, so
nothing above the mount layer names any of them:

| Filesystem | Access |
| --- | --- |
| FAT16/FAT32 | read/write - the boot volume |
| ext2 | read, including triple-indirect blocks and holes |
| ISO9660 | read, including Joliet names |
| exFAT | read |
| NTFS | read - fixups, resident and non-resident attributes, run lists |

MBR and GPT partition tables are parsed. ext4 and APFS remain unimplemented;
ext2 was built first because it is the same shape of problem with a fraction of
the surface, and it is what proved the interface could hold a second
filesystem at all.

Full write support for foreign filesystems stays gated behind test coverage,
and the bar here is a sabotage case per guard: removing any single check must
turn a named test red.

## Caching model

- unified page cache where feasible
- write-back with journalling or transaction semantics for supported
  filesystems. The block cache is write-back today and its flush reaches the
  device's own cache, which is what makes a barrier real rather than nominal.
- explicit invalidation contracts for compatibility runtimes and VM-backed file access

## Security considerations

- namespace-aware mount permissions
- immutable and verified system partitions in hardened modes
- per-filesystem policy hooks
- brokered access for sandboxed applications

## Current implementation snapshot

- VFS runtime supports mount, unmount, open, close, and secure-open policy checks.
- mount observability is available through active-mount counters.
- unmounted filesystems are rejected by open paths, improving lifecycle correctness.
- the kernel path is block device -> block cache -> filesystem driver -> mount,
  with the syscall layer naming none of the five drivers.
- a write-ahead journal recovers at attach and is swept against power loss, but
  no driver stages its metadata through it yet: the mechanism is verified, the
  volume is not. See `docs/implementation_progress/filesystem_layer.md`.
