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

### Planned read or integration priorities

- FAT32 or exFAT for boot and removable media interoperability
- ext4 read support as an early interoperability milestone
- NTFS read support later through service-level implementation
- APFS support deferred due to complexity and ecosystem constraints

Full write support for foreign filesystems should be gated behind strong test coverage.

## Caching model

- unified page cache where feasible
- write-back with journaling or transaction semantics for supported filesystems
- explicit invalidation contracts for compatibility runtimes and VM-backed file access

## Security considerations

- namespace-aware mount permissions
- immutable and verified system partitions in hardened modes
- per-filesystem policy hooks
- brokered access for sandboxed applications
