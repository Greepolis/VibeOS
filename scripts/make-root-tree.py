#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The native root's tree, from a build's EFI tree (docs/filesystem-design/
hierarchy.md, ext4.md step 4).

    make-root-tree.py <efi_root> <out_dir> [--marker] [--production]

The root is "/": the programs go to /usr/bin, and the rest of the EFI tree -
/corpus, /lib, /lib64, /win, /etc - comes along as it is. What stays on the
ESP alone is what the firmware and the kernel read from it themselves: the
loader, the kernel, the loop images and the swap file. Every directory the
hierarchy names is made, empty where nothing goes yet.

--marker writes /etc/vibeos/root, which the boot gate reads back to prove the
guest is reading this volume and not another one.

--production is the tree an image for people carries (E4-023): no test
programs, no /corpus fixtures, no test executables under /win, and an
/etc/vibeos/services that has init start the shell and nothing else. Without
that file init runs its built-in table, which is the boot gate's self-test -
two minutes of stress before a prompt, on every boot of every VM image, until
this flag existed. hierarchy.md: "A production build MUST omit them."

/lib, /lib64, /bin, /sbin and /usr/sbin are symbolic links into /usr, as the
hierarchy requires for the Linux personality's merged /usr (hierarchy.md;
E4-023): the loaders and libraries live in /usr/lib, once, and a program that
names /lib/ld-musl-x86_64.so.1 or /lib64/ld-linux-x86-64.so.2 reaches them
through the link. The kernel follows it when it opens an interpreter.

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

# The merged /usr: each of these is a symbolic link, never a directory.
ALIASES = (("lib", "usr/lib"), ("lib64", "usr/lib"), ("bin", "usr/bin"),
           ("sbin", "usr/bin"), ("usr/sbin", "bin"))

# What the firmware and the kernel read from the ESP themselves, so not a
# program for /usr/bin.
ESP_ONLY = ("VIBEOSKR.ELF", "SWAPFILE.BIN")

# The programs a production root keeps: init, the shells, and the small
# native utilities. Everything else in EFI/BOOT is a test program the boot
# gate runs. A list of what to keep rather than of what to drop, so that a new
# test program is left out by default instead of shipped by default.
PRODUCTION_PROGRAMS = ("INIT.ELF", "NATIVE_INIT.ELF", "BUSYBOX.ELF", "SH.ELF",
                       "CAT", "ECHO", "LS", "WC")

# Test executables that sit beside the Windows DLLs.
WIN_TESTS = ("hello.exe", "stubcall.exe")

# What init starts on a production root: BusyBox's shell on the console, as a
# session - when it ends, so does the machine's userland.
PRODUCTION_SERVICES = """\
# Services init starts (user/prog/init.c): <name> <path> <restart_limit> <oneshot> [<arg>]
# BusyBox becomes the command it is invoked as, so the name is the applet.
sh /usr/bin/BUSYBOX.ELF 0 1
"""


def make(efi_root, out, marker, production):
    ignore = ["*.tmp", "EFI", "startup.nsh"]
    if production:
        ignore.append("corpus")
        ignore.extend(WIN_TESTS)
    shutil.copytree(efi_root, out, dirs_exist_ok=True, ignore=shutil.ignore_patterns(*ignore))
    os.makedirs(os.path.join(out, "usr", "bin"), exist_ok=True)
    boot_dir = os.path.join(efi_root, "EFI", "BOOT")
    for n in sorted(os.listdir(boot_dir)):
        if n.endswith((".IMG", ".EFI", ".tmp")) or n in ESP_ONLY:
            continue
        if production and n not in PRODUCTION_PROGRAMS:
            continue
        shutil.copy2(os.path.join(boot_dir, n), os.path.join(out, "usr", "bin", n))
    for d in ROOT_TREE_DIRS:
        os.makedirs(os.path.join(out, d), exist_ok=True)
    # The EFI tree's /lib and /lib64 move into /usr/lib, and each becomes a
    # link to it. Nothing is in both: the musl loader, glibc's loader and the
    # x86_64-linux-gnu directory have different names.
    for old in ("lib", "lib64"):
        src = os.path.join(out, old)
        if os.path.isdir(src) and not os.path.islink(src):
            for name in os.listdir(src):
                dst = os.path.join(out, "usr", "lib", name)
                if os.path.exists(dst):
                    raise SystemExit("make-root-tree: %s is in /lib and /lib64 both" % name)
                shutil.move(os.path.join(src, name), dst)
            os.rmdir(src)
    for link, target in ALIASES:
        path = os.path.join(out, link)
        if not os.path.lexists(path):
            os.symlink(target, path)
    if marker:
        with open(os.path.join(out, "etc", "vibeos", "root"), "w") as f:
            f.write("VIBEOS_ROOT_MARKER\n")
    if production:
        with open(os.path.join(out, "etc", "vibeos", "services"), "w") as f:
            f.write(PRODUCTION_SERVICES)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if len(args) != 2:
        print(__doc__.strip().splitlines()[3].strip())
        return 2
    make(args[0], args[1], "--marker" in sys.argv[1:], "--production" in sys.argv[1:])
    return 0


if __name__ == "__main__":
    sys.exit(main())
