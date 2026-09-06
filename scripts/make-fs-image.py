#!/usr/bin/env python3
"""Build a small filesystem image with the host's own mkfs, for I5.

I5's whole point is that ext2, NTFS, ISO9660 and exFAT have never been mounted
by this kernel from anything. An image this project wrote itself would only
prove the drivers agree with the writer; an image made by the tool everybody
else uses is the thing worth reading.

So this shells out to mke2fs (and later mkisofs, mkfs.exfat) and puts one known
file inside, whose bytes carry their own offset. A constant would survive a
read that returned the wrong block, as long as that block had been written too.

The image is staged on the boot volume and mounted through the loop device, not
attached as a second disk: a second physical device needs virtio-blk to stop
being a singleton, which is a refactor of the driver the machine boots from.

Written only when missing or stale, so an incremental build does not rebuild a
filesystem every time.
"""

import os
import shutil
import subprocess
import sys

MARKER = "HELLO.TXT"
CONTENT_LEN = 4096


def content():
    return bytes(((i * 7) ^ (i >> 8) ^ 0x5A) & 0xFF for i in range(CONTENT_LEN))


def build_ext2(path, size_bytes):
    tool = shutil.which("mke2fs")
    if not tool:
        return "mke2fs not found"

    workdir = path + ".d"
    os.makedirs(workdir, exist_ok=True)
    with open(os.path.join(workdir, MARKER), "wb") as f:
        f.write(content())

    with open(path, "wb") as f:
        f.truncate(size_bytes)

    # -d stages a directory into the image. -F because the target is a plain
    # file rather than a device, and -q so a build log stays readable.
    #
    # revision 0 and no features: this kernel's ext2 driver is the thing under
    # test, and a modern mke2fs enables extents, 64bit and metadata_csum by
    # default - which are ext4 in an ext2 costume. Turning them off is not
    # making the test easy, it is making it a test of ext2.
    cmd = [tool, "-q", "-F", "-t", "ext2", "-b", "1024",
           "-O", "^resize_inode,^dir_index,^ext_attr,^has_journal",
           "-d", workdir, path]
    r = subprocess.run(cmd, capture_output=True, text=True)
    shutil.rmtree(workdir, ignore_errors=True)
    if r.returncode != 0:
        return "mke2fs failed: " + (r.stderr or r.stdout).strip()[:200]
    return None


def main():
    if len(sys.argv) != 4:
        print("usage: make-fs-image.py <kind> <path> <bytes>", file=sys.stderr)
        return 2
    kind, path, size = sys.argv[1], sys.argv[2], int(sys.argv[3])

    try:
        if os.path.getsize(path) == size:
            return 0
    except OSError:
        pass
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)

    if kind != "ext2":
        print("unknown filesystem kind: " + kind, file=sys.stderr)
        return 2
    err = build_ext2(path, size)
    if err:
        # Not fatal to the build. A machine without e2fsprogs still boots; it
        # just cannot run the ext2 half of I5, and the boot says so rather than
        # the build failing somewhere a developer has to guess about.
        print("fs image not built: " + err, file=sys.stderr)
        try:
            os.remove(path)
        except OSError:
            pass
        return 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
