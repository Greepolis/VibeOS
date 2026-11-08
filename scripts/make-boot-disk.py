#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The boot disk as a machine has it: an MBR, the EFI system partition, and the
native root on ext4 beside it (docs/filesystem-design/ext4.md step 4).

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


def make(esp, tree, disk, root_mb):
    mkfs = shutil.which("mkfs.ext4")
    if not mkfs:
        print("boot-disk=absent reason=no_mkfs.ext4")
        return 3
    root = disk + ".root.tmp"
    with open(root, "wb") as f:
        f.truncate(root_mb * 1024 * 1024)
    subprocess.run([mkfs, "-q", "-F", "-L", LABEL, "-b", "4096", "-d", tree, root],
                   check=True, stdout=subprocess.DEVNULL)
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
