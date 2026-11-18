#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The boot disk as a machine has it: an MBR, the EFI system partition, and the
native root on ext4 beside it (docs/filesystems/ext4/design.md step 4).

    make-boot-disk.py make <esp.img> <root-tree> <disk.img> [root_mb]
    make-boot-disk.py extract <disk.img> <esp.img> <root.img>

`make` formats the root with the host's own mkfs.ext4 - labelled vibeos-root,
filled from <root-tree> with -d - and puts the two images 1 MiB apart behind a
DOS partition table: partition 1, type 0xEF, the ESP that make_esp_image.py
built; partition 2, type 0x83, the root. OVMF boots from the 0xEF partition,
the kernel's FAT driver takes the first partition as the boot volume, and the
storage scan finds the second.

`extract` cuts both partitions back out of a disk the guest has written, so
that fat-fsck.py and e2fsck - which want a filesystem, not a disk - can judge
what it left.

Exits 3 without writing anything when mkfs.ext4 is not installed, so a caller
can say the root is absent rather than fail.
"""

import os
import shutil
import struct
import subprocess
import sys

SECTOR = 512
ALIGN = 2048          # 1 MiB, as every partitioning tool aligns today
LABEL = "vibeos-root"


def entry(boot, ptype, start, count):
    # CHS fields are the "use LBA" markers; nothing here reads them.
    return struct.pack("<B3sB3sII", boot, b"\xfe\xff\xff", ptype, b"\xfe\xff\xff", start, count)


def owned_by_root(img, tree):
    """Every file of the system belongs to root (E4-028). mkfs.ext4 -d copies
    the owner from the host, so each one was the uid of whoever built the
    image - 1000 in WSL - and on a machine with users that is somebody's
    account owning /usr/bin. Changing them on the host needs root; debugfs,
    which mkfs's own package ships, sets them in the image instead."""
    debugfs = shutil.which("debugfs")
    if not debugfs:
        raise SystemExit("make-boot-disk: debugfs is needed to give the root's files to root")
    cmds = []
    for d, dirs, files in os.walk(tree):
        rel = os.path.relpath(d, tree)
        base = "/" if rel == "." else "/" + rel.replace(os.sep, "/")
        for name in [""] + dirs + files:
            if name == "" and base != "/":
                continue   # each directory is its parent's entry
            p = base if name == "" else (base.rstrip("/") + "/" + name)
            if '"' in p or "\n" in p:
                raise SystemExit("make-boot-disk: a name debugfs cannot quote: %r" % p)
            cmds.append('sif "%s" uid 0' % p)
            cmds.append('sif "%s" gid 0' % p)
    script = img + ".debugfs"
    with open(script, "w") as f:
        f.write("\n".join(cmds) + "\n")
    r = subprocess.run([debugfs, "-w", "-f", script, img], stdout=subprocess.DEVNULL,
                       stderr=subprocess.PIPE, universal_newlines=True)
    os.unlink(script)
    errors = [l for l in r.stderr.splitlines() if l.strip() and not l.startswith("debugfs ")]
    if r.returncode != 0 or errors:
        raise SystemExit("make-boot-disk: debugfs could not set the owners: %s" % "; ".join(errors[:3]))


# Names compare without case where people and Windows live (hierarchy.md,
# "Case", decided 2026-11-11; E4-030): ext4's casefold, the +F attribute on
# each of these, inherited by whatever is made in them.
CASEFOLD_DIRS = ("/home", "/usr/lib/windows", "/var/lib/windows")


def casefolded(img):
    """Give CASEFOLD_DIRS the casefold flag. Linux sets +F only on an empty
    directory; these may already hold files (C:/Windows's), so after the flag
    e2fsck -D rebuilds every index with the hash of the folded names, and a
    final e2fsck -n is the check that no two names collide by case."""
    debugfs, e2fsck = shutil.which("debugfs"), shutil.which("e2fsck")
    if not debugfs or not e2fsck:
        raise SystemExit("make-boot-disk: debugfs and e2fsck are needed to casefold the root's directories")
    cmds = []
    for d in CASEFOLD_DIRS:
        st = subprocess.run([debugfs, "-R", "stat %s" % d, img], stdout=subprocess.PIPE,
                            stderr=subprocess.DEVNULL, universal_newlines=True).stdout
        if "Flags: " not in st:
            raise SystemExit("make-boot-disk: %s is not on the root" % d)
        flags = int(st.split("Flags: ")[1].split()[0], 16)
        cmds.append("sif %s flags 0x%x" % (d, flags | 0x40000000))
    r = subprocess.run([debugfs, "-w", "-f", "-", img], input="\n".join(cmds) + "\n", stdout=subprocess.DEVNULL,
                       stderr=subprocess.PIPE, universal_newlines=True)
    if r.returncode != 0 or [l for l in r.stderr.splitlines() if l.strip() and not l.startswith("debugfs ")]:
        raise SystemExit("make-boot-disk: debugfs could not set +F: %s" % r.stderr.strip()[:200])
    if subprocess.run([e2fsck, "-fyD", img], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode > 1:
        raise SystemExit("make-boot-disk: e2fsck -D could not index the casefolded directories")
    r = subprocess.run([e2fsck, "-fn", img], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       universal_newlines=True)
    if r.returncode != 0:
        raise SystemExit("make-boot-disk: the casefolded root is not clean (two names alike but for case?): " +
                         " ".join(r.stdout.split())[:300])


def name_root(esp, root):
    """Write the root's UUID to the ESP, EFI/VIBEOS/ROOT, one line
    "UUID=<uuid>" (hierarchy F2 step 2): the kernel mounts as / the volume
    with that UUID and no other. Read with blkid, the same reader a manifest
    line is copied from."""
    blkid, mmd, mcopy = shutil.which("blkid"), shutil.which("mmd"), shutil.which("mcopy")
    if not blkid or not mmd or not mcopy:
        raise SystemExit("make-boot-disk: blkid and mtools are needed to name the root on the ESP")
    uuid = subprocess.run([blkid, "-p", "-o", "value", "-s", "UUID", root], stdout=subprocess.PIPE,
                          universal_newlines=True).stdout.strip()
    if not uuid:
        raise SystemExit("make-boot-disk: blkid found no UUID on the root")
    spec = esp + ".ROOT"
    with open(spec, "w", newline="\n") as f:
        f.write("UUID=%s\n" % uuid)
    subprocess.run([mmd, "-D", "s", "-i", esp, "::/EFI/VIBEOS"], stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL)   # already there: fine
    r = subprocess.run([mcopy, "-o", "-i", esp, spec, "::/EFI/VIBEOS/ROOT"], stdout=subprocess.DEVNULL,
                       stderr=subprocess.PIPE, universal_newlines=True)
    os.unlink(spec)
    if r.returncode != 0:
        raise SystemExit("make-boot-disk: could not write EFI/VIBEOS/ROOT: %s" % r.stderr.strip()[:200])


def make(esp, tree, disk, root_mb):
    mkfs = shutil.which("mkfs.ext4")
    if not mkfs:
        print("boot-disk=absent reason=no_mkfs.ext4")
        return 3
    root = disk + ".root.tmp"
    with open(root, "wb") as f:
        f.truncate(root_mb * 1024 * 1024)
    subprocess.run([mkfs, "-q", "-F", "-L", LABEL, "-b", "4096", "-O", "casefold", "-d", tree, root],
                   check=True, stdout=subprocess.DEVNULL)
    owned_by_root(root, tree)
    casefolded(root)
    name_root(esp, root)
    esp_bytes = os.path.getsize(esp)
    esp_sectors = (esp_bytes + SECTOR - 1) // SECTOR
    root_start = (ALIGN + esp_sectors + ALIGN - 1) // ALIGN * ALIGN
    root_sectors = os.path.getsize(root) // SECTOR
    mbr = bytearray(SECTOR)
    mbr[440:444] = b"VIBE"   # disk signature
    mbr[446:462] = entry(0x80, 0xEF, ALIGN, esp_sectors)
    mbr[462:478] = entry(0x00, 0x83, root_start, root_sectors)
    mbr[510:512] = b"\x55\xaa"
    tmp = disk + ".tmp"
    with open(tmp, "wb") as out:
        out.write(mbr)
        out.truncate((root_start + root_sectors) * SECTOR)
        out.seek(ALIGN * SECTOR)
        with open(esp, "rb") as f:
            shutil.copyfileobj(f, out)
        out.seek(root_start * SECTOR)
        with open(root, "rb") as f:
            shutil.copyfileobj(f, out)
    os.replace(tmp, disk)
    os.unlink(root)
    print("boot-disk=ok esp_lba=%d root_lba=%d root_sectors=%d" % (ALIGN, root_start, root_sectors))
    return 0


def extract(disk, esp, root):
    with open(disk, "rb") as f:
        mbr = f.read(SECTOR)
        if mbr[510:512] != b"\x55\xaa":
            print("boot-disk=FAILED reason=no_partition_table")
            return 1
        parts = []
        for i in range(2):
            boot, _, ptype, _, start, count = struct.unpack("<B3sB3sII", mbr[446 + 16 * i:462 + 16 * i])
            parts.append((ptype, start, count))
        if parts[0][0] != 0xEF or parts[1][0] != 0x83:
            print("boot-disk=FAILED reason=partition_types_%02x_%02x" % (parts[0][0], parts[1][0]))
            return 1
        for (ptype, start, count), path in zip(parts, (esp, root)):
            f.seek(start * SECTOR)
            left = count * SECTOR
            with open(path, "wb") as out:
                while left > 0:
                    chunk = f.read(min(left, 1 << 20))
                    if not chunk:
                        print("boot-disk=FAILED reason=short_disk")
                        return 1
                    out.write(chunk)
                    left -= len(chunk)
    print("boot-disk=ok extracted")
    return 0


def main(argv):
    if len(argv) >= 5 and argv[1] == "make":
        return make(argv[2], argv[3], argv[4], int(argv[5]) if len(argv) > 5 else 64)
    if len(argv) == 5 and argv[1] == "extract":
        return extract(argv[2], argv[3], argv[4])
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
