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
| `/run` | tmpfs, 0755 (since 2026-11-17) | tmpfs, cleared at boot | done in F1: the gate writes a file there and checks the disk has none |
| `/sys` | a directory, nothing mounted | sysfs or a control service | reserved; no phase owns it yet |
| `/vol1`, `/vol2`, ... | every other volume the scan mounts, by its index; `/vol1` also a scratch volume of the I/O self-test | `/mnt/<name>` for an administrator's volume, `/run/media/<user>/<label>` for removable media | **move - F2**: a volume gets its place from the manifest, not from the order the scan found it in |
| `/run/test/ext2`, `ext4`, `iso`, `ntfs`, `exfat`, `fatlong` | test images from `EFI/BOOT/*.IMG`, mounted under `/run/test` since F1 (they were at the top of the tree) | out of the production namespace | done in F1: volatile and under one test-only place; a production ESP carries no `.IMG` (`make-root-tree.py`), so nothing is mounted there |

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
| `/root`, `/mnt`, `/opt`, `/usr/share`, `/var/cache`, `/var/log`, `/var/tmp` | empty directories, owned by root | keep; the build already set the contract's modes (`make-root-tree.py`, `SPECIAL_DIR_MODES`, since E4-028 - this inventory first said nothing did, and the sabotage that should have proved it went NOT RED); since F1 the gate reads them back off the image |
| `/corpus` | the corpus workloads and LTP (test-only) | keep test-only: left out of a production root (`--production`) |
| `/win` | the Windows test programs (test-only) | keep test-only: left out of a production root |
| `/DOCS` | made by the boot self-test's shell at the root of the volume | test traffic, and it stays: it is how the root's write path is tested, and the self-test's input is injected only when the root is not a production one (E4-023) |

## Paths the code names

| Name | Where | Rule |
| --- | --- | --- |
| `/sbin/init`, `/sbin/logd` | init's and the log service's image paths (`userland.c`) | keep while `/sbin` is an alias; name `/usr/bin/...` when the programs get their real names (E4-021) |
| `/usr/bin/*.ELF` | the boot self-test's script (`userland.c`) | test-only, as the script is |
| `C:\Windows`, `/windows` | the NT personality maps both onto `/usr/lib/windows` (`kernel/abi/nt/file.c`) | keep: a translation, not a directory |
| `/tmp/.shm-*`, `/tmp/.memfd-*`, `/tmp/.ntsec-*` | names the kernel gives System V shared memory, memfds and NT sections in tmpfs | keep: internal names, never opened by a program. POSIX shared memory - a C library's shm_open - is `/dev/shm`, a tmpfs since F1 |
| `/proc/self/fd/*`, `/dev/console` | standard names | keep |

## Writable state on the ESP

The directory contract says the ESP holds "UEFI executables, signed manifests
and recovery assets only; no user data".

| What | Today | Target |
| --- | --- | --- |
| Swap | `EFI/BOOT/SWAPFILE.BIN`, on the ESP | a swap file on the root (`/var/lib/swap` or similar) or a partition - the order is already decided: file first, partition second |
| The disk log sink | sectors the log sink reserves on the boot disk | decided with F2's volume ids: the sink must name the volume it writes |
| The self-test's files | written to `/boot/efi` | test traffic, and it stays: it is how the FAT write path is tested on the gate's media; a production root runs no self-test |

## What this says about the phases

**F1 - the namespace on the current root.** Done 2026-11-17. ext4's step 4 had
put the root's tree in place - the hierarchy's directories, `/tmp`, `/dev`,
`/proc` and `/boot/efi` mounted where they belong, the merged-`/usr` aliases,
a production root without the test payloads. F1 added what the inventory
found missing: `/run` and `/dev/shm` as tmpfs, the test images under
`/run/test`, and a check of the directory contract's modes on the image the
machine ran from. The boot gate
asserts each (`run_round_trip_failed`, `run_is_not_volatile`,
`dev_shm_round_trip_failed`, `root_tree_mode_wrong`), and
`cases/hier-f1-boot.txt` and `cases/hier-f1-modes.txt` go red with exactly
those reasons. The self-test's own writes to the root and the ESP stay: they
are how those write paths are tested, and only the gate's media run them.

What F1's definition of done also names is already covered elsewhere and
was not rebuilt: mount-boundary traversal and the aliases by the path walk's
host tests and the corpus, permissions by L2's credential tests
(`abi-cred*.txt`), fixture exclusion by `make-root-tree.py --production`.

**F2 - the mount and policy service.** Not started: every mount above is a
line of kernel code. A versioned manifest, stable volume ids instead of a label
and an index, required against optional mounts, mount options (`ro`,
`nosuid`, `nodev`, `noexec`), and mount and unmount with descriptors open.
