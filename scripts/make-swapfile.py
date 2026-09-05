#!/usr/bin/env python3
"""Create the swap file staged on the boot medium.

Somewhere for swap to write. The memory manager's P5 - swap map, page-out,
page-in and reclaim's anonymous tier - is built, host-tested and
sabotage-verified, and had never run on a booting machine, because the medium
carried no swap area at all. That is the state this project distrusts most.

A file rather than a partition: this medium is one FAT volume with no spare
space to carve, and a file is the case a real machine usually has to fall back
on anyway. The partition follows and is described by the same structure.

Written in one go rather than grown, because what matters here is not the
contents - the swap map writes over them - but the *extent*. Allocating the
whole length at once is what gives a filesystem the chance to lay it down as
one run. It is only a chance: the kernel checks whether it actually did and
refuses a fragmented file rather than writing through the gaps into other
files. Nothing here may assume contiguity, and nothing does.

Rewritten only when missing or the wrong size, so an incremental build does not
copy eight megabytes every time.
"""

import os
import sys

CHUNK = 1 << 20


def main():
    if len(sys.argv) != 3:
        print("usage: make-swapfile.py <path> <bytes>", file=sys.stderr)
        return 2
    path, want = sys.argv[1], int(sys.argv[2])

    try:
        if os.path.getsize(path) == want:
            return 0
    except OSError:
        pass

    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    # Not truncate(): a sparse file has no blocks until they are written, and
    # the point of this file is the blocks. Under QEMU's synthesised FAT that
    # distinction does not exist, but the same file is put on the real images
    # the VM appliances boot from, where it does.
    zero = b"\0" * CHUNK
    with open(path, "wb") as f:
        left = want
        while left > 0:
            n = CHUNK if left >= CHUNK else left
            f.write(zero[:n])
            left -= n
    return 0


if __name__ == "__main__":
    sys.exit(main())
