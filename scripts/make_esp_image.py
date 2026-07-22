#!/usr/bin/env python3
"""Build a bootable UEFI FAT16 EFI System Partition image from a directory.

Self-contained: no mtools / mkfs.fat required. Produces a raw "superfloppy"
FAT16 image (no partition table) whose contents mirror the given source tree
(typically build/artifacts/efi_root), so UEFI firmware finds
\\EFI\\BOOT\\BOOTX64.EFI and boots it. The raw image boots directly under
QEMU+OVMF and can be converted to .vdi/.vmdk with qemu-img for VirtualBox /
VMware, or wrapped into an El Torito UEFI .iso with xorriso.

All input file names are 8.3-compatible (BOOTX64.EFI, VIBEOSKR.ELF,
STARTUP.NSH, EFI, BOOT), so no long-file-name directory entries are needed.

Usage: make_esp_image.py <source_dir> <output_img> [size_mb]
"""

import os
import struct
import sys

SECTOR = 512
SECTORS_PER_CLUSTER = 4               # 2 KiB clusters
RESERVED_SECTORS = 4
NUM_FATS = 2
ROOT_ENTRIES = 512                    # 512 * 32 / 512 = 32 root-dir sectors
MEDIA = 0xF8
DEFAULT_SIZE_MB = 48

ATTR_FILE = 0x20
ATTR_DIR = 0x10
ATTR_VOLUME = 0x08

FAT_FREE = 0x0000
FAT_EOC = 0xFFFF


def encode_83(name, is_dir):
    """Return the 11-byte 8.3 directory name field for an 8.3-safe name."""
    if name in (".", ".."):
        return name.encode("ascii").ljust(11, b" ")
    if is_dir:
        base, ext = name, ""
    else:
        base, _, ext = name.partition(".")
    base = base.upper()[:8].ljust(8, " ")
    ext = ext.upper()[:3].ljust(3, " ")
    return (base + ext).encode("ascii")


def dir_entry(name, is_dir, first_cluster, size):
    raw = bytearray(32)
    raw[0:11] = encode_83(name, is_dir)
    raw[11] = ATTR_DIR if is_dir else ATTR_FILE
    # ctime/cdate/adate/mtime/mdate left as 0 (firmware does not require them)
    struct.pack_into("<H", raw, 26, first_cluster & 0xFFFF)  # cluster low
    struct.pack_into("<I", raw, 28, size & 0xFFFFFFFF)       # file size
    return bytes(raw)


class FatBuilder:
    def __init__(self):
        self.next_cluster = 2
        self.fat = {}            # cluster -> next cluster (or FAT_EOC)
        self.cluster_bytes = {}  # cluster -> 2048-byte payload

    def cluster_size(self):
        return SECTOR * SECTORS_PER_CLUSTER

    def alloc_chain(self, data):
        """Store `data` across a fresh cluster chain; return the first cluster."""
        csize = self.cluster_size()
        if len(data) == 0:
            data = b"\x00" * csize
        chunks = [data[i:i + csize] for i in range(0, len(data), csize)]
        first = None
        prev = None
        for chunk in chunks:
            cl = self.next_cluster
            self.next_cluster += 1
            self.cluster_bytes[cl] = chunk.ljust(csize, b"\x00")
            if first is None:
                first = cl
            if prev is not None:
                self.fat[prev] = cl
            prev = cl
        self.fat[prev] = FAT_EOC
        return first

    def build_directory(self, entries, self_cluster, parent_cluster):
        """entries: list of (name, is_dir, first_cluster, size). Adds '.'/'..'."""
        blob = bytearray()
        blob += dir_entry(".", True, self_cluster, 0)
        blob += dir_entry("..", True, parent_cluster, 0)
        for name, is_dir, fc, size in entries:
            blob += dir_entry(name, is_dir, fc, size)
        return bytes(blob)


def add_tree(builder, src_dir):
    """Recursively add a subtree; return list of root-level directory entries."""
    def add_dir(path, self_cluster, parent_cluster):
        child_entries = []
        for name in sorted(os.listdir(path)):
            full = os.path.join(path, name)
            if os.path.isdir(full):
                dir_cluster = builder.next_cluster
                builder.next_cluster += 1  # reserve now for '.'/'..' back-refs
                sub_entries = build_children(full, dir_cluster, self_cluster)
                blob = builder.build_directory(sub_entries, dir_cluster, self_cluster)
                place_directory(builder, dir_cluster, blob)
                child_entries.append((name, True, dir_cluster, 0))
            else:
                with open(full, "rb") as fp:
                    data = fp.read()
                fc = builder.alloc_chain(data) if data else 0
                child_entries.append((name, False, fc, len(data)))
        return child_entries

    def build_children(path, self_cluster, parent_cluster):
        return add_dir(path, self_cluster, parent_cluster)

    # Root directory is a fixed region (cluster 0 as parent reference).
    return add_dir(src_dir, 0, 0)


def place_directory(builder, cluster, blob):
    """Write a pre-reserved directory cluster (may span multiple clusters)."""
    csize = builder.cluster_size()
    chunks = [blob[i:i + csize] for i in range(0, len(blob), csize)] or [b""]
    prev = None
    first = cluster
    for idx, chunk in enumerate(chunks):
        cl = cluster if idx == 0 else builder.next_cluster
        if idx != 0:
            builder.next_cluster += 1
        builder.cluster_bytes[cl] = chunk.ljust(csize, b"\x00")
        if prev is not None:
            builder.fat[prev] = cl
        prev = cl
    builder.fat[prev] = FAT_EOC
    return first


def compute_geometry(total_sectors):
    root_dir_sectors = (ROOT_ENTRIES * 32 + SECTOR - 1) // SECTOR
    tmp1 = total_sectors - (RESERVED_SECTORS + root_dir_sectors)
    tmp2 = (256 * SECTORS_PER_CLUSTER) + NUM_FATS
    sectors_per_fat = (tmp1 + (tmp2 - 1)) // tmp2
    data_sectors = total_sectors - (RESERVED_SECTORS + NUM_FATS * sectors_per_fat + root_dir_sectors)
    cluster_count = data_sectors // SECTORS_PER_CLUSTER
    return root_dir_sectors, sectors_per_fat, cluster_count


def build_boot_sector(total_sectors, sectors_per_fat, label):
    bs = bytearray(SECTOR)
    bs[0:3] = b"\xEB\x3C\x90"                      # jmp + nop
    bs[3:11] = b"VIBEOS  "                          # OEM name
    struct.pack_into("<H", bs, 11, SECTOR)          # bytes per sector
    bs[13] = SECTORS_PER_CLUSTER
    struct.pack_into("<H", bs, 14, RESERVED_SECTORS)
    bs[16] = NUM_FATS
    struct.pack_into("<H", bs, 17, ROOT_ENTRIES)
    if total_sectors < 0x10000:
        struct.pack_into("<H", bs, 19, total_sectors)   # total sectors 16
        struct.pack_into("<I", bs, 32, 0)
    else:
        struct.pack_into("<H", bs, 19, 0)
        struct.pack_into("<I", bs, 32, total_sectors)   # total sectors 32
    bs[21] = MEDIA
    struct.pack_into("<H", bs, 22, sectors_per_fat)
    struct.pack_into("<H", bs, 24, 32)              # sectors per track
    struct.pack_into("<H", bs, 26, 8)               # heads
    struct.pack_into("<I", bs, 28, 0)               # hidden sectors
    bs[36] = 0x80                                    # drive number
    bs[38] = 0x29                                    # extended boot signature
    struct.pack_into("<I", bs, 39, 0x5642_454F & 0xFFFFFFFF)  # volume id
    bs[43:54] = label.upper()[:11].ljust(11).encode("ascii")
    bs[54:62] = b"FAT16   "
    bs[510] = 0x55
    bs[511] = 0xAA
    return bytes(bs)


def main():
    if len(sys.argv) < 3:
        print("usage: make_esp_image.py <source_dir> <output_img> [size_mb]", file=sys.stderr)
        return 2
    src_dir = sys.argv[1]
    out_img = sys.argv[2]
    size_mb = int(sys.argv[3]) if len(sys.argv) > 3 else DEFAULT_SIZE_MB

    if not os.path.isdir(src_dir):
        print(f"source dir not found: {src_dir}", file=sys.stderr)
        return 1

    total_sectors = (size_mb * 1024 * 1024) // SECTOR
    root_dir_sectors, sectors_per_fat, cluster_count = compute_geometry(total_sectors)
    if not (4085 <= cluster_count < 65525):
        print(f"cluster count {cluster_count} out of FAT16 range; adjust size_mb", file=sys.stderr)
        return 1

    builder = FatBuilder()
    root_entries = add_tree(builder, src_dir)

    if builder.next_cluster - 2 > cluster_count:
        print("content does not fit in image; increase size_mb", file=sys.stderr)
        return 1

    # --- assemble the image ---
    data_start_sector = RESERVED_SECTORS + NUM_FATS * sectors_per_fat + root_dir_sectors
    image = bytearray(total_sectors * SECTOR)

    # boot sector
    image[0:SECTOR] = build_boot_sector(total_sectors, sectors_per_fat, "VIBEOS")

    # FAT tables (two identical copies)
    fat_bytes = bytearray(sectors_per_fat * SECTOR)
    struct.pack_into("<H", fat_bytes, 0, 0xFF00 | MEDIA)   # FAT[0]
    struct.pack_into("<H", fat_bytes, 2, FAT_EOC)          # FAT[1]
    for cl, nxt in builder.fat.items():
        struct.pack_into("<H", fat_bytes, cl * 2, nxt & 0xFFFF)
    for i in range(NUM_FATS):
        off = (RESERVED_SECTORS + i * sectors_per_fat) * SECTOR
        image[off:off + len(fat_bytes)] = fat_bytes

    # root directory region (fixed)
    root_blob = bytearray()
    root_blob += dir_entry("VIBEOS", False, 0, 0)[:11] + bytes([ATTR_VOLUME]) + bytes(20)
    for name, is_dir, fc, size in root_entries:
        root_blob += dir_entry(name, is_dir, fc, size)
    root_off = (RESERVED_SECTORS + NUM_FATS * sectors_per_fat) * SECTOR
    image[root_off:root_off + len(root_blob)] = root_blob

    # data region (clusters)
    for cl, payload in builder.cluster_bytes.items():
        sector = data_start_sector + (cl - 2) * SECTORS_PER_CLUSTER
        off = sector * SECTOR
        image[off:off + len(payload)] = payload

    with open(out_img, "wb") as fp:
        fp.write(image)

    print(f"[ESP] wrote {out_img} ({total_sectors * SECTOR} bytes, FAT16, "
          f"{cluster_count} clusters, {builder.next_cluster - 2} used)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
