# F0 inventory: every path in use, against the hierarchy

Phase F0 of [implementation-phases.md](implementation-phases.md): what the
system names today, read off the code on 2026-11-17, each against its place in
[hierarchy.md](hierarchy.md) and a rule - **keep**, **move** (where to, and in
which phase) or **retire**. A row's "today" is evidence in the tree, cited by
file; nothing here is a claim about what runs that the code does not show.

## Mounts

Every mount is made by the kernel, in `kernel/arch/x86_64/io_bringup.c`, at a
path written there. There is no manifest: that is F2's.

| Path | Today | Target | Rule |
| --- | --- | --- | --- |
| `/` | the ext4 partition labelled `vibeos-root`, found by label; without one, the first volume mounted (the FAT ESP) | the system root, chosen by a stable volume id | keep; F2 replaces "by label, else the first volume" with the manifest's id, and "no root" with a policy answer instead of a silent fallback to the ESP |
| `/boot/efi` | the ESP (FAT), mounted read-write; the boot self-test writes files there | the ESP, read-only in normal operation | keep the place; F2 mounts it read-only outside updates. The self-test's writes are test traffic and move with the fixtures |
| `/tmp` | tmpfs, mode 01777 (`kernel/fs/tmpfs.c`) | tmpfs, sticky, `nosuid,nodev` | keep; mount options do not exist yet (no `nosuid`, `nodev`, `noexec` anywhere) - F2 |
| `/dev` | devfs | devfs | keep |
| `/proc` | procfs | procfs | keep |
| `/run` | a directory on the root, persistent | tmpfs, cleared at boot | **move to tmpfs - F1** |
| `/sys` | a directory, nothing mounted | sysfs or a control service | reserved; no phase owns it yet |
| `/vol1`, `/vol2`, ... | every other volume the scan mounts, by its index; `/vol1` also a scratch volume of the I/O self-test | `/mnt/<name>` for an administrator's volume, `/run/media/<user>/<label>` for removable media | **move - F2**: a volume gets its place from the manifest, not from the order the scan found it in |
| `/ext2`, `/ext4`, `/iso`, `/ntfs`, `/exfat`, `/fatlong` | test images from `EFI/BOOT/*.IMG`, mounted at the top of the tree when present (`hw_fsimage` table) | out of the production namespace | **move - F1**: under one test-only place (proposed `/run/test/<fs>`), mounted only on the gate's media. A production ESP carries no `.IMG` (`make-root-tree.py`), so these mounts already fail there - quietly, which is the part to change |

## The root's tree

Made by `scripts/make-root-tree.py` (`ROOT_TREE_DIRS`, `ALIASES`) and
`scripts/make-boot-disk.py`, which formats it, gives every file to root and
casefolds three directories (E4-030).

| Path | Today | Rule |
| --- | --- | --- |
| `/usr/bin` | every program, under its ESP name (`BUSYBOX.ELF`) | keep; the names wait for the shell's design (E4-021) |
| `/bin`, `/sbin`, `/lib`, `/lib64`, `/usr/sbin` | symbolic links to `/usr/bin` and `/usr/lib` | keep (merged `/usr`, decided 2026-11-03) |
| `/usr/lib/windows` | the NT personality's system files; casefolded | keep (done 2026-11-10) |
| `/var/lib/windows` | the registry's hives (`USER.HIV`, `MACHINE.HIV`); casefolded | keep |
| `/etc/vibeos` | `services` (init's manifest on a production root), `root` (the build's marker) | keep |
| `/home` | empty, casefolded, owned by root | keep; per-user directories and their owners are the user model's |
| `/root`, `/mnt`, `/opt`, `/usr/share`, `/var/cache`, `/var/log`, `/var/tmp` | empty directories, owned by root, modes as mkfs copied them from the host | keep; **F1** checks each against the directory contract's mode (`/root` 0700, `/var/tmp` sticky) - nothing sets them today |
| `/corpus` | the corpus workloads and LTP (test-only) | keep test-only: left out of a production root (`--production`) |
| `/win` | the Windows test programs (test-only) | keep test-only: left out of a production root |
| `/DOCS` | made by the boot self-test's shell at the root of the volume | test traffic; moves with the fixtures - **F1** |

## Paths the code names

| Name | Where | Rule |
| --- | --- | --- |
| `/sbin/init`, `/sbin/logd` | init's and the log service's image paths (`userland.c`) | keep while `/sbin` is an alias; name `/usr/bin/...` when the programs get their real names (E4-021) |
| `/usr/bin/*.ELF` | the boot self-test's script (`userland.c`) | test-only, as the script is |
| `C:\Windows`, `/windows` | the NT personality maps both onto `/usr/lib/windows` (`kernel/abi/nt/file.c`) | keep: a translation, not a directory |
| `/tmp/.shm-*`, `/tmp/.memfd-*`, `/tmp/.ntsec-*` | names the kernel gives shared memory, memfds and NT sections in tmpfs | review in **F1**: Linux puts POSIX shared memory in `/dev/shm`, which does not exist here; a program that opens `/dev/shm/x` itself finds nothing |
| `/proc/self/fd/*`, `/dev/console` | standard names | keep |

## Writable state on the ESP

The directory contract says the ESP holds "UEFI executables, signed manifests
and recovery assets only; no user data".

| What | Today | Target |
| --- | --- | --- |
| Swap | `EFI/BOOT/SWAPFILE.BIN`, on the ESP | a swap file on the root (`/var/lib/swap` or similar) or a partition - the order is already decided: file first, partition second |
| The disk log sink | sectors the log sink reserves on the boot disk | decided with F2's volume ids: the sink must name the volume it writes |
| The self-test's files | written to `/boot/efi` | test traffic; moves with the fixtures - **F1** |

## What this says about the phases

**F1 - the namespace on the current root.** Mostly built by ext4's step 4:
the root is the hierarchy, `/tmp`, `/dev`, `/proc` and `/boot/efi` are mounted
where they belong, the aliases exist, and a production root leaves the test
payloads out. What is left, from the rows above:

1. `/run` on tmpfs.
2. The test images and the self-test's own files out of the production
   namespace, under one test-only place.
3. The directory contract's modes set at build and checked (`/root`, `/var/tmp`).
4. `/dev/shm`, or a decision that it stays absent.
5. Its definition of done: boot-to-CLI and the Linux and NT regressions green,
   and tests for mount-boundary traversal, permissions, aliases and fixture
   exclusion from release media.

**F2 - the mount and policy service.** Not started: every mount above is a
line of kernel code. A versioned manifest, stable volume ids instead of a label
and an index, required against optional mounts, mount options (`ro`,
`nosuid`, `nodev`, `noexec`), and mount and unmount with descriptors open.

Each item becomes a row in this folder's work as it is taken, and the table is
re-read against the code when a phase closes.
