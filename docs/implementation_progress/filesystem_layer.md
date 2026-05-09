# Filesystem Layer Progress

Status: In Progress
Last review: 2026-05-09

## Implemented
- VFS runtime/service scaffold in `user/fs/vfs_service.c` and `user/fs/vfs_ops.c`.
- Mount/open/close primitives for host/runtime simulation path.
- Policy-aware secure open path integrated with security policy model.
- Service lifecycle hooks for start/stop and supervision interactions.
- Minimal persistent backend in runtime (`persist_write/read/delete/count`) with mount-scoped retention and unmount cleanup.

## Pending
- Persistent on-disk filesystem backend (current persistence layer is in-memory only).
- Journal/integrity strategy and crash consistency validation.
- Cross-filesystem compatibility adapters (ext4/NTFS/APFS roadmap items).

## Next checkpoint
- Add a concrete minimal persistent backend and fault-injection consistency tests.
