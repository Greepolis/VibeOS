# Filesystem Integration and Trust Boundaries

## 1. Layering contract

The public route is:

```text
application / compatibility runtime
       | versioned syscalls, descriptors, capabilities
       v
process root + cwd + mount namespace -> VFS path walk / policy
       | vnode/file operations
       v
filesystem implementation -> block cache -> block request -> device host
```

The current VFS and filesystem operations are the compatibility boundary. New
code MUST NOT call FAT helpers or name a concrete filesystem above its driver.
The future user-space filesystem host described by ADR-0006 is a target, not a
reason to move parsers across a trust boundary before the service protocol,
shared-memory ownership and recovery behavior are tested. Begin with the
current in-kernel VFS semantics, keep filesystem operations replaceable through
versioned contracts, and move concrete parsers to isolated hosts only behind a
measured and tested transition.

Required internal concepts:

- **Volume identity:** stable UUID/partition identity, capacity and device
  identity. Enumeration order or PCI adapter number is never a persistent name.
- **Mount object:** filesystem instance, mountpoint, root node, flags, owning
  namespace, lifecycle/refcount and health state.
- **Open description:** node identity, access mode, offset/lock state and mount
  reference. Closing the final descriptor releases it; unmount cannot invalidate
  a live descriptor silently.
- **Path context:** process root, cwd and mount namespace, copied/inherited at
  fork according to ABI rules and preserved across exec as specified.
- **Policy hook:** credentials, capabilities, mount flags and object labels are
  checked at open/create/rename/unlink/execute and device operations, not only
  when a mount is attached.

## 2. Mount policy and boot order

Production boot should proceed in bounded, observable steps:

1. Firmware validates and starts the EFI loader; the loader verifies the
   release manifest and loads the kernel plus a small signed bootstrap image.
2. Kernel initializes the minimum block/device stack and mounts the bootstrap
   environment read-only. Serial/TTY rescue remains available if any later step
   fails.
3. `init` discovers volumes by stable identity, validates filesystem and slot
   metadata, and mounts the selected verified system root.
4. Before switching roots, it mounts `/dev`, `/proc`, `/sys`, `/run` and `/tmp`
   in the intended order and validates the required service binaries/config.
5. `init` performs an atomic root switch (or equivalent process-root transition)
   only when the new root is complete; on failure it enters recovery/rescue and
   preserves the previous slot.
6. Persistent `/var` and `/home` are mounted only after integrity and ownership
   checks. Optional/removable volumes are mounted later and cannot delay boot
   indefinitely.

Target default mounts:

| Mountpoint | Purpose | Required policy |
| --- | --- | --- |
| `/` | selected system root | verified slot, read-only in normal mode |
| `/boot/efi` | ESP | read-only except explicit signed update transaction |
| `/dev` | device namespace | node/capability access control |
| `/proc` | process/kernel introspection | per-process visibility and read-only by default |
| `/sys` | device/control namespace | writes require explicit privilege/capability |
| `/run` | runtime state | tmpfs, clear on boot, per-service ownership |
| `/tmp` | scratch | tmpfs, `01777`, quotas, `nosuid,nodev` |
| `/var` | persistent service state/logs | separate data area or verified subvolume, quota/rotation |
| `/home` | user data | separate data area or verified subvolume, per-user policy |
| `/media/*` | removable devices | explicit/managed mount, `nodev,nosuid,noexec` by default |
| `/mnt/*` | administrator mounts | explicit only, no implicit fallback |

All mount attempts have bounded timeouts and stable error codes. Failure to
mount a required root is fatal to normal startup but MUST preserve a usable
diagnostic/recovery path. Failure of optional `/media`, network filesystems or
noncritical services is logged and isolated.

## 3. Storage format and durability decision

The hierarchy is independent of the filesystem format. The production root
format is **not yet selected**. The existing AuroraFS concept is a candidate,
not an accepted disk format. Before selection, compare at least:

- whether an existing driver can be made safely writable and maintained;
- metadata transaction/replay guarantees, checksums and power-loss behavior;
- host-side formatter/fsck/inspection tools and recovery from another OS;
- encryption, snapshots/rollback and space accounting requirements;
- fuzzing maturity, format versioning and migration strategy;
- operational complexity and the project's capacity to maintain the format.

No format is production-acceptable until it has versioned superblocks, bounds
checks, checksummed metadata, a tested write ordering/barrier contract, crash
recovery, offline inspection/repair tooling, fuzz corpus, and power-cut tests on
the actual filesystem driver. A host-modelled journal test is necessary but
does not prove a mounted real volume survived power loss.

Recommended rollout: keep FAT as ESP/bring-up media; keep foreign filesystems
read-only until their write paths are independently qualified; select one
read-write system format before installer work; keep the on-disk interface
versioned and avoid promising cross-version mounts before migration tools exist.

## 4. Personality integration

### Native VibeOS and Linux

Both use the POSIX-like VFS hierarchy. Linux ABI translation retains Linux
errno, stat, symlink, directory enumeration, file-lock and mount semantics at
its boundary. Native APIs may use versioned handles/capabilities, but they must
resolve through the same object and permission policy. Compatibility behavior
must not add hidden path aliases that bypass checks.

`/bin`, `/sbin`, `/lib` are merged-`/usr` compatibility aliases. `/proc` and
`/dev` are virtual namespaces, not files copied into the root image. `/run`
contains runtime sockets; persistent service configuration belongs in `/etc`
or `/var/lib`, not in a boot script or the ESP.

### Windows NT personality

An NT process receives a per-process DOS-device map and a Windows path view.
The recommended system DLL location is `C:\Windows\System32`; user data is
`C:\Users\<name>`; application data is under a policy-controlled
`C:\Program Files` or `C:\ProgramData`. The mapping targets VFS objects and
is capability-scoped; it does not require a second on-disk tree. Until the NT
loader and case-insensitive lookup contract are complete, `/win` remains a
compatibility alias to the currently staged DLL directory and must be measured
for use before removal.

The current code uses `/win` as a provisional location and `docs/abi/windows.md`
tracks that decision. A migration must update the NT loader, image staging,
Wine test harness and regression assertions atomically. Do not rename it in a
way that breaks Windows programs between commits.

### macOS-oriented compatibility

Do not claim a macOS filesystem layout or APFS support until its ABI/runtime
target is defined. The VFS can support another per-process namespace later;
its semantics and case behavior must be explicit and must not change the native
root's rules.

## 5. Security, identity and operations

- Every service runs under a dedicated identity and receives only its required
  directory, device and IPC capabilities. A daemon must not need writable `/usr`
  to update its data.
- `/etc` and verified `/usr` are root-owned; `/tmp` is sticky; `/run` service
  subdirectories are private; `/var/log` is append/rotate controlled and
  protected from unprivileged tampering.
- System and data mounts default to `nosuid`; removable and untrusted mounts
  default to `nodev,nosuid,noexec`. Overrides are explicit, auditable and
  privilege-gated.
- `openat`-style operations must validate directory descriptors and use
  race-free beneath/no-follow resolution for privileged services. Path checks
  and file opens must be one VFS operation where possible.
- Disk exhaustion, inode/metadata exhaustion, read-only remount, journal replay
  failure, checksum mismatch and degraded devices each have distinct
  diagnostics. `/var/log` and caches are quota/retention bounded; audit records
  cannot be silently replaced by an ordinary user.
- Mount namespaces and root switching are per-process/service capabilities;
  container-like isolation must not rely on hiding a path in userspace alone.

## 6. Observability and compatibility guarantees

Expose read-only mount and volume status through `/proc/mounts` (Linux-compatible
view), a native status API and diagnostic CLI. Record mount ID, stable volume
identity, format/version, flags, health, attach/replay duration, read/write
errors and free-space estimates. Counters must be consumed by tests or status
tools, not merely incremented.

Maintain a migration table for every transitional path (`EFI/BOOT`, `/EFI`,
`/DOCS`, `/win`, old `/bin` layout). Each alias needs an owner, removal
condition, boot/test coverage and a compatibility deadline; avoid indefinite
aliases that obscure the canonical hierarchy.
