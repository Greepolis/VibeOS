#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The host's side of scripts/dev/verify-ext4.sh (docs/filesystem-design/ext4.md).

    ext4-tree.py make <dir>     build the tree mkfs.ext4 -d will put on a volume
    ext4-tree.py list <dir>     list it as vibeos_ext4_exercise lists the volume

The tree is chosen for what it makes the driver do, each entry named after it:
a file past twelve blocks, a large contiguous one, a sparse file of forty
separate pieces - more extents than the four an inode holds, so the tree has a
level of index blocks - an empty file, a directory deep enough to walk, and one
with six hundred entries, which mkfs.ext4 indexes with htree. Contents are a
function of the path, so a block read from the wrong file shows as a wrong
crc32 rather than as a plausible one.
"""

import os
import sys
import zlib


def body(path, n):
    seed = zlib.crc32(path.encode())
    out = bytearray(n)
    x = seed or 1
    for i in range(n):
        x = (x * 1103515245 + 12345) & 0x7FFFFFFF
        out[i] = (x >> 16) & 0xFF
    return bytes(out)


def put(root, rel, n):
    p = os.path.join(root, rel)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "wb") as f:
        f.write(body(rel, n))


def make(root):
    os.makedirs(root, exist_ok=True)
    put(root, "small.txt", 100)
    put(root, "empty", 0)
    put(root, "past-twelve-blocks.bin", 13 * 4096 + 17)
    put(root, "large-contiguous.bin", 3 * 1024 * 1024 + 5)
    # Forty pieces a megabyte apart: forty extents, and holes between them.
    sparse = os.path.join(root, "sparse-forty-extents.bin")
    with open(sparse, "wb") as f:
        for k in range(40):
            f.seek(k * 1024 * 1024)
            f.write(body("sparse%d" % k, 4096))
    for depth in range(1, 7):
        put(root, "/".join(["deep"] * depth) + "/level%d.txt" % depth, 300 * depth)
    for k in range(600):
        put(root, "many/entry-%04d with a longer name.txt" % k, k % 50)
    put(root, "accent è ü 中.txt", 64)


def listing(root):
    rows = []
    for d, dirs, files in os.walk(root):
        rel = os.path.relpath(d, root)
        base = "" if rel == "." else "/" + rel.replace(os.sep, "/")
        for name in dirs:
            rows.append("%s/%s\tD" % (base, name))
        for name in files:
            p = os.path.join(d, name)
            with open(p, "rb") as f:
                data = f.read()
            rows.append("%s/%s\tF\t%d\t%08x" % (base, name, len(data), zlib.crc32(data) & 0xFFFFFFFF))
    for r in sorted(rows):
        print(r)


if __name__ == "__main__":
    if len(sys.argv) != 3 or sys.argv[1] not in ("make", "list"):
        print(__doc__, file=sys.stderr)
        sys.exit(2)
    (make if sys.argv[1] == "make" else listing)(sys.argv[2])
