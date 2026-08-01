# Filesystem Layer Progress

Status: In Progress (runtime FAT read/write verified)
Last review: 2026-08-01

## Implemented
- VFS runtime/service scaffold in `user/fs/vfs_service.c` and `user/fs/vfs_ops.c`.
- Mount/open/close primitives for host/runtime simulation path.
- Policy-aware secure open path integrated with security policy model.
- Service lifecycle hooks for start/stop and supervision interactions.
- Minimal persistent backend in runtime (`persist_write/read/delete/count`) with mount-scoped retention and unmount cleanup.
- Runtime FAT support in `kernel/arch/x86_64/fat.c` reads the boot volume and supports file creation, overwrite, directory creation and removal through the shell/syscall path.
- The virtio-blk driver supplies the first real persistent block-device path in QEMU; init and shell payloads are loaded from FAT.

## Pending
- General VFS integration between the portable service model and the runtime FAT backend.
- Journal/integrity strategy and crash-consistency validation; FAT updates are not crash-safe.
- Cross-filesystem compatibility adapters (ext4/NTFS/APFS roadmap items).

## Next checkpoint
- Add fault-injection and reboot persistence tests for the FAT write path, then define the journaled native filesystem target.
