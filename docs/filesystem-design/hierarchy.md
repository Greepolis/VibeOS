# Canonical Filesystem Hierarchy

## 1. Root namespace

The installed system presents this hierarchy. Directories that are not yet
implemented are target mount points or reserved locations; their presence in
this tree is not a claim that their service already exists.

```text
/
|-- boot/efi/             EFI System Partition; firmware and recovery loaders
|-- dev/                  device nodes and pseudo-devices (devfs)
|-- etc/                  host configuration and identity policy
|   `-- vibeos/            VibeOS-owned configuration, versioned by schema
|-- home/<user>/           user-owned documents and per-user state
|-- media/<label>/         automount locations for removable media
|-- mnt/<name>/            administrator-controlled temporary mounts
|-- opt/<vendor>/<app>/    optional third-party application trees
|-- proc/                  process and kernel view (procfs)
|-- root/                  root administrator's home directory
|-- run/                   volatile service/session state (tmpfs)
|-- srv/<service>/         data intentionally served by a local service
|-- sys/                   devices, buses and kernel control view (sysfs)
|-- tmp/                   volatile, shared scratch space (tmpfs, mode 01777)
|-- usr/
|   |-- bin/               user commands and applications
|   |-- lib/               shared libraries and runtime data
|   |-- libexec/           non-interactive helper programs
|   |-- sbin/              system administration executables
|   |-- share/             architecture-independent data, docs, locales
|   `-- local/             administrator-managed software, not OS updates
|-- var/
|   |-- cache/             recreatable caches
|   |-- lib/               persistent service state and databases
|   |-- log/               bounded, rotated system/service logs
|   |-- spool/             queued work
|   `-- tmp/               temporary data that must survive reboot
`-- win/                   transitional alias only; see integration.md
```

`/bin`, `/sbin` and `/lib` MUST resolve to their `/usr` counterparts for the
Linux personality's merged-`/usr` expectation. Implement them as aliases only
after symlink and secure path-walk behavior is proven; do not duplicate mutable
binaries in two locations. `/var/tmp` is persistent temporary storage and MUST
not alias `/tmp`.

## 2. Directory contract

| Path | Backing / mutability | Owner and baseline access | Contract |
| --- | --- | --- | --- |
| `/` | verified system root; normally read-only | `root:root`, `0755` | Contains the hierarchy and mount points; not a user scratch area |
| `/boot/efi` | ESP, FAT, read-only during normal operation | `root:root`, `0755` | UEFI executables, signed manifests and recovery assets only; no user data |
| `/dev` | devfs | per-node policy | Device access is capability/permission checked, not granted by path visibility alone |
| `/etc` | persistent machine configuration | `root:root`, usually `0755`; secrets stricter | Human/admin configuration, not generated package payloads |
| `/home/<user>` | persistent data volume | user-owned, usually `0700` initially | User files and explicit per-user configuration |
| `/media` | removable-volume mounts | policy-dependent | No automatic execution; mount options default restrictive |
| `/mnt` | administrator-managed mounts | root-owned | Not an implicit automount namespace |
| `/opt` | verified application payload or managed app volume | root-owned | Optional vendor trees; mutable app state lives elsewhere |
| `/proc` | procfs | per-object visibility | Process/runtime diagnostics, not a durable filesystem |
| `/root` | persistent or encrypted admin state | `root:root`, `0700` | Never exposed to unprivileged sessions |
| `/run` | tmpfs, cleared at boot | root-owned, service subdirs | PIDs, sockets, locks and ephemeral credentials; no durable configuration |
| `/srv` | persistent service data | dedicated service identity | A service receives only its own subtree |
| `/sys` | sysfs/control service | capability/policy gated | Device and kernel controls are not writable by ordinary users |
| `/tmp` | tmpfs, cleared at boot | `root:root`, `01777` | Sticky bit mandatory; apply per-process quotas and `nosuid,nodev` |
| `/usr` | verified system slot, read-only | `root:root`, `0755` | OS executables/libraries/data; ordinary applications cannot replace files |
| `/var` | persistent state volume | root and service-owned subtrees | Logs, databases, package/update state; quotas and retention policy required |
| `/var/tmp` | persistent state volume | sticky, policy-controlled | Not executable by default for untrusted writes |

Modes above are defaults; ACLs/capabilities and service identities refine them.
There MUST be no general writable path in the verified system tree for a
non-administrator. Ownership and permission metadata must be representable by
every writable production filesystem.

## 3. Path and name semantics

- Native VibeOS and Linux paths use `/`, are case-sensitive, and preserve valid
  UTF-8 byte sequences. Invalid UTF-8 is rejected at user-facing creation and
  import boundaries; the filesystem MUST NOT silently normalize names into a
  collision.
- The VFS MUST preserve the existing bounded component/path checks, make limits
  explicit in the native ABI, and return deterministic `ENAMETOOLONG`, `ELOOP`,
  `ENOTDIR`, `ENOENT` or access errors. Linux personality limits remain Linux
  ABI-compatible even if the internal representation permits more.
- `.` and `..`, repeated separators, process root, working directory, dirfd and
  mount crossings are handled by one path-walk implementation. `..` cannot
  escape a process root or namespace root.
- Security-sensitive opens MUST support no-follow and beneath/in-root
  resolution semantics; a caller cannot validate a string and later follow a
  different symlink target in a second walk.
- Case-insensitive Windows lookup is scoped to the NT volume/path translator.
  If two names collide under its defined Unicode case-folding rule, lookup MUST
  fail deterministically rather than select an arbitrary entry. The on-disk
  namespace is not globally case-folded.

## 4. Boot and test payload separation

The ESP's production tree is intentionally small:

```text
EFI/BOOT/BOOTX64.EFI
EFI/VIBEOS/<release-id>/kernel.elf
EFI/VIBEOS/<release-id>/manifest.json
EFI/VIBEOS/recovery/...
```

Actual paths may retain firmware-required naming, but loader, kernel, hashes,
signatures and version must be described by a verified manifest. The boot
volume MUST NOT become the package repository or general user home.

QEMU fixtures, `TASK.ELF`, `BUSYBOX.ELF`, `DOCS/NOTES.TXT` and synthetic
filesystem images belong to test-only media or a dedicated test mount. A
production build MUST omit them unless a separately signed diagnostic/recovery
image explicitly includes them. Tests should use stable test fixture paths and
never define the installed system hierarchy by accident.
