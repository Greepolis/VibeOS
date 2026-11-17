# Filesystems

Everything about filesystems in one place: the design, what is built and how it
was verified, one folder per filesystem. The storage underneath - block
devices, the block cache, partition tables, the boot disk - is
[io/](../io/README.md) and its progress files in
[implementation_progress/](../implementation_progress/); the row in
[implementation_progress.md](../implementation_progress.md) ("Filesystem
Layer") links both.

| What | Where |
| --- | --- |
| Design intent, written before the code | [overview.md](overview.md) |
| The layer: block device, cache, VFS, the five filesystems behind it, FAT among them | [layer.md](layer.md) |
| Mounts and volumes: the probe, the mount table, the storage scan | [vfs/mounts.md](vfs/mounts.md), [vfs/volumes.md](vfs/volumes.md) |
| The hierarchy: what lives where, its phases F0-F6, how it meets the rest | [hierarchy/](hierarchy/README.md) |
| ext4, the native root: design, progress and its open-items register (E4-*), the ext2 reader it grew from | [ext4/design.md](ext4/design.md), [ext4/progress.md](ext4/progress.md), [ext4/ext2-reader.md](ext4/ext2-reader.md) |
| NTFS and exFAT | [ntfs-exfat/progress.md](ntfs-exfat/progress.md) |
| ISO9660 | [iso9660/progress.md](iso9660/progress.md) |

FAT, tmpfs, procfs and devfs have no folder of their own yet: what they are is
in [layer.md](layer.md) and in the code's own comments (`kernel/fs/`).

Gathered here on 2026-11-17 from `docs/filesystem-design/`,
`docs/filesystem.md` and `docs/implementation_progress/`; a review of the whole
of the documentation, and whether the filesystem layer and the VFS want a
refactor, is planned for later.
