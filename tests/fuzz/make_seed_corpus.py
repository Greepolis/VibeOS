#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build real-tool parser seeds, then derive small malformed neighbours.

The valid ELF/PE images come from native and MinGW linkers. Filesystem images
come from mke2fs; no filesystem structure is hand-encoded here.
"""
import argparse
import pathlib
import shutil
import struct
import subprocess
import tempfile


def run(argv):
    subprocess.run(argv, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)


def patch_u16(data, offset, value):
    struct.pack_into("<H", data, offset, value)


def patch_u32(data, offset, value):
    struct.pack_into("<I", data, offset, value)


def ext4_superblock_crc32c(data):
    # Match vibeos_ext2_crc32c(~0u, sb, 0x3FC): no final xor.
    crc = 0xFFFFFFFF
    for value in data:
        crc ^= value
        for _ in range(8):
            crc = (crc >> 1) ^ (0x82F63B78 if (crc & 1) else 0)
    return crc & 0xFFFFFFFF


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=pathlib.Path)
    parser.add_argument("--blocks", type=int, default=2048,
                        help="filesystem size in blocks (default: 2048)")
    args = parser.parse_args()
    out = args.output
    out.mkdir(parents=True, exist_ok=True)
    required = ("gcc", "x86_64-w64-mingw32-gcc", "mke2fs")
    missing = [tool for tool in required if shutil.which(tool) is None]
    if missing:
        raise SystemExit("missing real-image tools: " + ", ".join(missing))

    with tempfile.TemporaryDirectory(prefix="vibeos-parser-seeds-") as tmp:
        tmp = pathlib.Path(tmp)
        source = tmp / "seed.c"
        source.write_text("void _start(void) { __asm__ volatile(\"nop\"); }\n",
                          encoding="utf-8")

        elf = out / "exec_images" / "elf64-real-toolchain"
        elf.parent.mkdir(parents=True, exist_ok=True)
        run(["gcc", "-nostdlib", "-static", "-no-pie", "-Wl,-e,_start",
             "-Wl,--build-id=none", str(source), "-o", str(elf)])
        bad_elf = bytearray(elf.read_bytes())
        if len(bad_elf) < 64 or bad_elf[:4] != b"\x7fELF":
            raise SystemExit("native linker did not produce an ELF64 seed")
        patch_u32(bad_elf, 32, 0xFFFFFFF0)
        (elf.parent / "elf64-invalid-phoff").write_bytes(bad_elf)

        pe = out / "exec_images" / "pe64-real-toolchain.exe"
        run(["x86_64-w64-mingw32-gcc", "-nostdlib", "-Wl,-e,_start",
             "-Wl,--subsystem,console", str(source), "-o", str(pe)])
        bad_pe = bytearray(pe.read_bytes())
        if len(bad_pe) < 0x40 or bad_pe[:2] != b"MZ":
            raise SystemExit("MinGW linker did not produce a PE seed")
        peoff = struct.unpack_from("<I", bad_pe, 0x3C)[0]
        section_count = struct.unpack_from("<H", bad_pe, peoff + 6)[0]
        optional_size = struct.unpack_from("<H", bad_pe, peoff + 20)[0]
        if section_count == 0 or len(bad_pe) < peoff + 24 + optional_size + 40:
            raise SystemExit("MinGW PE seed has no complete section table")
        raw_ptr = peoff + 24 + optional_size + 20
        patch_u32(bad_pe, raw_ptr, 0xFFFFFFF0)
        (pe.parent / "pe64-invalid-raw-pointer").write_bytes(bad_pe)

        seed_root = tmp / "root"
        seed_root.mkdir()
        (seed_root / "fuzz-seed.txt").write_bytes(
            b"created by mke2fs\n" * 4)

        for kind, features, inode_size, block_size in (
            ("ext2", "filetype,^64bit,^metadata_csum", 128, 1024),
            ("ext4", "extent,metadata_csum,^has_journal,^64bit,^flex_bg,^orphan_file", 256, 4096),
        ):
            image = out / kind / (kind + "-mke2fs.img")
            image.parent.mkdir(parents=True, exist_ok=True)
            with image.open("wb") as stream:
                stream.truncate(args.blocks * block_size)
            run(["mke2fs", "-q", "-F", "-t", kind, "-b", str(block_size),
                 "-I", str(inode_size), "-O", features,
                 "-E", "lazy_itable_init=0,lazy_journal_init=0",
                 "-d", str(seed_root), str(image)])
            # Keep a valid, tool-generated filesystem and a malformed copy
            # whose root directory entry has an impossible zero record length.
            malformed = bytearray(image.read_bytes())
            if len(malformed) >= 2048:
                # A zeroed metadata sector is still a useful early-reject seed;
                # the valid image remains the deep-path corpus entry.
                malformed[1024 + 56:1024 + 58] = b"\0\0"
                (image.parent / (kind + "-bad-magic.img")).write_bytes(malformed)

        # A valid ext4 image with impossible group geometry is reserved for the
        # production-bound negative control, not the ordinary fuzz smoke run.
        bad_geometry = bytearray((out / "ext4" / "ext4-mke2fs.img").read_bytes())
        patch_u32(bad_geometry, 1024 + 32, 32769)  # 4 KiB bitmap holds 32768 bits
        # Keep the superblock valid: otherwise metadata_csum rejects the seed
        # before the mount reaches the geometry guard under test.
        patch_u32(bad_geometry, 1024 + 0x3FC,
                  ext4_superblock_crc32c(bad_geometry[1024:1024 + 0x3FC]))
        (out / "ext4" / "ext4-bad-bitmap-geometry.img").write_bytes(bad_geometry)


if __name__ == "__main__":
    main()
