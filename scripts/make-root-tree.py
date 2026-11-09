#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The native root's tree, from a build's EFI tree (docs/filesystem-design/
hierarchy.md, ext4.md step 4).

    make-root-tree.py <efi_root> <out_dir> [--marker]

The root is "/": the programs go to /usr/bin, and the rest of the EFI tree -
/corpus, /lib, /lib64, /win, /etc - comes along as it is. What stays on the
ESP alone is what the firmware and the kernel read from it themselves: the
loader, the kernel, the loop images and the swap file. Every directory the
hierarchy names is made, empty where nothing goes yet.

--marker writes /etc/vibeos/root, which the boot gate reads back to prove the
guest is reading this volume and not another one.

One script, because two callers build this tree - the boot gate's disk and
the VM images - and two copies of the list would drift.
"""

import os
import shutil
import sys

# The hierarchy's directories (hierarchy.md). /dev, /proc, /run and /tmp are
# mount points.
ROOT_TREE_DIRS = ("boot/efi", "dev", "etc/vibeos", "home", "mnt", "opt", "proc", "root",
                  "run", "sys", "tmp", "usr/bin", "usr/lib/windows", "usr/share",
                  "var/cache", "var/lib/windows", "var/log", "var/tmp")

# What the firmware and the kernel read from the ESP themselves, so not a
# program for /usr/bin.
ESP_ONLY = ("VIBEOSKR.ELF", "SWAPFILE.BIN")


def make(efi_root, out, marker):
    shutil.copytree(efi_root, out, dirs_exist_ok=True,
                    ignore=shutil.ignore_patterns("*.tmp", "EFI", "startup.nsh"))
    os.makedirs(os.path.join(out, "usr", "bin"), exist_ok=True)
    boot_dir = os.path.join(efi_root, "EFI", "BOOT")
    for n in sorted(os.listdir(boot_dir)):
        if n.endswith((".IMG", ".EFI", ".tmp")) or n in ESP_ONLY:
            continue
        shutil.copy2(os.path.join(boot_dir, n), os.path.join(out, "usr", "bin", n))
    for d in ROOT_TREE_DIRS:
        os.makedirs(os.path.join(out, d), exist_ok=True)
    if marker:
        with open(os.path.join(out, "etc", "vibeos", "root"), "w") as f:
            f.write("VIBEOS_ROOT_MARKER\n")


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if len(args) != 2:
        print(__doc__.strip().splitlines()[3].strip())
        return 2
    make(args[0], args[1], "--marker" in sys.argv[1:])
    return 0


if __name__ == "__main__":
    sys.exit(main())
