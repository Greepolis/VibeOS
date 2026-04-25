# Filesystem Layer Progress

Status: In Progress
Last review: 2026-04-25

## Implemented
- VFS runtime/service scaffold in `user/fs/vfs_service.c` and `user/fs/vfs_ops.c`.
- Mount/open/close primitives for host/runtime simulation path.
- Policy-aware secure open path integrated with security policy model.
- Service lifecycle hooks for start/stop and supervision interactions.

## Pending
- Persistent on-disk filesystem backend.
- Journal/integrity strategy and crash consistency validation.
- Cross-filesystem compatibility adapters (ext4/NTFS/APFS roadmap items).

## Next checkpoint
- Add a concrete minimal persistent backend and fault-injection consistency tests.
