#!/usr/bin/env python3
"""Patch and validate a PE32+ image so OVMF accepts it as an EFI app."""

import argparse
import struct
import sys


IMAGE_FILE_MACHINE_AMD64 = 0x8664
IMAGE_FILE_RELOCS_STRIPPED = 0x0001
IMAGE_SUBSYSTEM_EFI_APPLICATION = 10
IMAGE_DIRECTORY_ENTRY_BASERELOC = 5

IMAGE_SCN_CNT_INITIALIZED_DATA = 0x00000040
IMAGE_SCN_MEM_DISCARDABLE = 0x02000000
IMAGE_SCN_MEM_READ = 0x40000000


def align_up(value, alignment):
    if alignment == 0:
        raise ValueError("alignment cannot be zero")
    return (value + alignment - 1) & ~(alignment - 1)


def u16(data, offset):
    return struct.unpack_from("<H", data, offset)[0]


def u32(data, offset):
    return struct.unpack_from("<I", data, offset)[0]


def put_u16(data, offset, value):
    struct.pack_into("<H", data, offset, value)


def put_u32(data, offset, value):
    struct.pack_into("<I", data, offset, value)


def put_u64(data, offset, value):
    struct.pack_into("<Q", data, offset, value)


def pe_checksum(data, checksum_offset):
    total = 0
    length = len(data)
    index = 0
    while index < length:
        if index == checksum_offset:
            index += 4
            continue
        if index + 1 < length:
            word = data[index] | (data[index + 1] << 8)
        else:
            word = data[index]
        total = (total & 0xFFFF) + word + (total >> 16)
        index += 2
    total = (total & 0xFFFF) + (total >> 16)
    total = total + (total >> 16)
    return (total & 0xFFFF) + length


def parse_headers(data):
    if len(data) < 0x100:
        raise ValueError("image too small")
    if data[0:2] != b"MZ":
        raise ValueError("missing MZ header")
    pe = u32(data, 0x3C)
    if pe + 24 > len(data):
        raise ValueError("PE header out of range")
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("missing PE signature")

    machine = u16(data, pe + 4)
    sections = u16(data, pe + 6)
    opt_size = u16(data, pe + 20)
    characteristics = u16(data, pe + 22)
    opt = pe + 24
    if opt + opt_size > len(data):
        raise ValueError("optional header out of range")
    if u16(data, opt) != 0x20B:
        raise ValueError("not a PE32+ image")
    if machine != IMAGE_FILE_MACHINE_AMD64:
        raise ValueError(f"unexpected machine 0x{machine:04x}")
    if u16(data, opt + 68) != IMAGE_SUBSYSTEM_EFI_APPLICATION:
        raise ValueError("subsystem is not EFI application")
    if u32(data, opt + 108) <= IMAGE_DIRECTORY_ENTRY_BASERELOC:
        raise ValueError("missing base relocation data directory slots")

    section_table = opt + opt_size
    if section_table + sections * 40 > len(data):
        raise ValueError("section table out of range")

    return {
        "pe": pe,
        "opt": opt,
        "section_table": section_table,
        "sections": sections,
        "characteristics": characteristics,
        "section_alignment": u32(data, opt + 32),
        "file_alignment": u32(data, opt + 36),
        "size_of_image": u32(data, opt + 56),
        "size_of_headers": u32(data, opt + 60),
        "checksum_offset": opt + 64,
        "reloc_dir_offset": opt + 112 + (IMAGE_DIRECTORY_ENTRY_BASERELOC * 8),
    }


def section_name(data, header_offset):
    raw = bytes(data[header_offset:header_offset + 8])
    return raw.split(b"\0", 1)[0].decode("ascii", "replace")


def ensure_reloc_section(data, hdr):
    reloc_dir_rva = u32(data, hdr["reloc_dir_offset"])
    reloc_dir_size = u32(data, hdr["reloc_dir_offset"] + 4)
    if reloc_dir_rva != 0 and reloc_dir_size >= 8:
        return False

    sections = hdr["sections"]
    section_table = hdr["section_table"]
    first_raw = None
    last_raw_end = 0
    last_virtual_end = 0
    for i in range(sections):
        sh = section_table + i * 40
        raw_ptr = u32(data, sh + 20)
        raw_size = u32(data, sh + 16)
        virt_addr = u32(data, sh + 12)
        virt_size = u32(data, sh + 8)
        if raw_ptr:
            first_raw = raw_ptr if first_raw is None else min(first_raw, raw_ptr)
            last_raw_end = max(last_raw_end, raw_ptr + raw_size)
        last_virtual_end = max(last_virtual_end, virt_addr + max(virt_size, raw_size))

    new_header = section_table + sections * 40
    if first_raw is not None and new_header + 40 > first_raw:
        raise ValueError("no room for .reloc section header")

    file_alignment = hdr["file_alignment"]
    section_alignment = hdr["section_alignment"]
    raw_ptr = align_up(max(len(data), last_raw_end, hdr["size_of_headers"]), file_alignment)
    virt_addr = align_up(max(last_virtual_end, hdr["size_of_image"]), section_alignment)

    reloc_payload = struct.pack("<IIHH", virt_addr, 12, 0, 0)
    raw_size = align_up(len(reloc_payload), file_alignment)
    if len(data) < raw_ptr:
        data.extend(b"\0" * (raw_ptr - len(data)))
    data[raw_ptr:raw_ptr + raw_size] = reloc_payload + (b"\0" * (raw_size - len(reloc_payload)))

    section = bytearray(40)
    section[0:8] = b".reloc\0\0"
    put_u32(section, 8, len(reloc_payload))
    put_u32(section, 12, virt_addr)
    put_u32(section, 16, raw_size)
    put_u32(section, 20, raw_ptr)
    put_u32(section, 36, IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_DISCARDABLE | IMAGE_SCN_MEM_READ)
    data[new_header:new_header + 40] = section

    put_u16(data, hdr["pe"] + 6, sections + 1)
    put_u32(data, hdr["opt"] + 56, align_up(virt_addr + len(reloc_payload), section_alignment))
    put_u32(data, hdr["reloc_dir_offset"], virt_addr)
    put_u32(data, hdr["reloc_dir_offset"] + 4, len(reloc_payload))
    return True


def fix_and_validate(path):
    with open(path, "rb") as fp:
        data = bytearray(fp.read())

    hdr = parse_headers(data)
    changed = ensure_reloc_section(data, hdr)

    characteristics = u16(data, hdr["pe"] + 22)
    if characteristics & IMAGE_FILE_RELOCS_STRIPPED:
        put_u16(data, hdr["pe"] + 22, characteristics & ~IMAGE_FILE_RELOCS_STRIPPED)
        changed = True

    if u64_or_zero(data, hdr["opt"] + 72) == 0:
        put_u64(data, hdr["opt"] + 72, 1024 * 1024)
        put_u64(data, hdr["opt"] + 80, 4096)
        changed = True
    if u64_or_zero(data, hdr["opt"] + 88) == 0:
        put_u64(data, hdr["opt"] + 88, 1024 * 1024)
        put_u64(data, hdr["opt"] + 96, 4096)
        changed = True

    put_u32(data, hdr["checksum_offset"], 0)
    put_u32(data, hdr["checksum_offset"], pe_checksum(data, hdr["checksum_offset"]))
    changed = True

    if changed:
        with open(path, "wb") as fp:
            fp.write(data)

    hdr = parse_headers(data)
    characteristics = u16(data, hdr["pe"] + 22)
    reloc_rva = u32(data, hdr["reloc_dir_offset"])
    reloc_size = u32(data, hdr["reloc_dir_offset"] + 4)
    if characteristics & IMAGE_FILE_RELOCS_STRIPPED:
        raise ValueError("RELOCS_STRIPPED is still set")
    if reloc_rva == 0 or reloc_size < 8:
        raise ValueError("base relocation directory is missing")
    if u32(data, hdr["opt"] + 56) == 0:
        raise ValueError("SizeOfImage is zero")

    names = {section_name(data, hdr["section_table"] + i * 40) for i in range(hdr["sections"])}
    if ".reloc" not in names:
        raise ValueError(".reloc section missing")


def u64_or_zero(data, offset):
    if offset + 8 > len(data):
        return 0
    return struct.unpack_from("<Q", data, offset)[0]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("image")
    args = parser.parse_args()
    try:
        fix_and_validate(args.image)
    except Exception as exc:
        print(f"efi-pe-fixup: {exc}", file=sys.stderr)
        return 1
    print(f"efi-pe-fixup: ok {args.image}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
