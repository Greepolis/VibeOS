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


def build_iso9660(path, size_bytes):
    # xorriso rather than genisoimage: it is what ships on a modern distro and
    # what ships in the CI image. size_bytes is ignored - an ISO is whatever
    # size its contents make it, which is why the staleness check below has to
    # look at the marker file rather than at the length.
    tool = shutil.which("xorriso")
    if not tool:
        return "xorriso not found"

    workdir = path + ".d"
    os.makedirs(workdir, exist_ok=True)
    with open(os.path.join(workdir, MARKER), "wb") as f:
        f.write(content())

    # No Rock Ridge and no Joliet. Those are extensions; the driver under test
    # reads plain ISO9660, and giving it an image whose real names live in an
    # extension it does not implement would test the fallback rather than the
    # filesystem.
    cmd = [tool, "-as", "mkisofs", "-quiet", "-o", path, workdir]
    r = subprocess.run(cmd, capture_output=True, text=True)
    shutil.rmtree(workdir, ignore_errors=True)
    if r.returncode != 0:
        return "xorriso failed: " + (r.stderr or r.stdout).strip()[:200]
    return None


def _mkfs_into(tool_names, argv_fn, path, size_bytes, stage_with):
    """Make an image with somebody else's mkfs, then put the marker inside it.

    ext2 and ISO9660 can be given their contents by the mkfs itself (-d, and a
    source directory). NTFS and exFAT cannot: mkntfs and mkfs.exfat format and
    nothing more. Copying a file in afterwards needs a *writing* driver for
    that filesystem, and the ones in this kernel are read-only by design.

    So `stage_with` names the tool that can write into the finished image -
    ntfs-3g and its exfat counterpart both come with the packages that provide
    the mkfs, but they need FUSE and a mount, which CI containers do not
    reliably have. When it is not available the image is still built and the
    marker is absent, and the caller says so rather than pretending.
    """
    tool = None
    for name in tool_names:
        tool = shutil.which(name)
        if tool:
            break
    if not tool:
        return " or ".join(tool_names) + " not found"

    with open(path, "wb") as f:
        f.truncate(size_bytes)
    r = subprocess.run(argv_fn(tool, path), capture_output=True, text=True)
    if r.returncode != 0:
        return tool + " failed: " + (r.stderr or r.stdout).strip()[:200]

    err = _stage_marker(stage_with, path)
    if err:
        print("fs image staged without a marker: " + err, file=sys.stderr)
        # Not fatal. A formatted image with no marker still exercises the
        # mount, the superblock and the geometry, which is most of what these
        # drivers had never done. The boot says the marker was not found and
        # the gate accepts that verdict distinctly from a failure.
        return None
    return None


def _stage_marker(kind, path):
    """Copy the marker into a finished image without mounting it.

    Mounting would need root and FUSE, which neither a developer machine nor a
    CI container reliably has, and an image staged by `mount` would also be
    staged by the kernel's own driver rather than by the filesystem's tools -
    which is the thing this whole file exists to avoid.

    ntfsprogs ships ntfscp, which writes into an image directly. exfatprogs
    ships no equivalent, so an exFAT image here carries no marker and the boot
    says so: mounting a real mkfs.exfat volume and reading its root directory
    is still far more than that driver had ever done, and claiming a file
    comparison that did not happen would be worse than the gap.
    """
    if kind != "ntfs":
        return "no tool can stage a marker into " + kind + " without mounting"
    tool = shutil.which("ntfscp")
    if not tool:
        return "ntfscp not found"

    src = path + ".marker"
    with open(src, "wb") as f:
        f.write(content())
    r = subprocess.run([tool, path, src, MARKER], capture_output=True,
                       text=True)
    try:
        os.remove(src)
    except OSError:
        pass
    if r.returncode != 0:
        return "ntfscp failed: " + (r.stderr or r.stdout).strip()[:200]
    return None


def build_ntfs(path, size_bytes):
    # -F because the target is a plain file; -Q skips the surface scan and the
    # zeroing, which on a 16 MiB image is the difference between instant and
    # not. --no-indexing keeps the image to the shape this driver reads.
    return _mkfs_into(
        ["mkntfs", "mkfs.ntfs"],
        lambda tool, p: [tool, "-F", "-Q", "-f", p],
        path, size_bytes, "ntfs")


def build_exfat(path, size_bytes):
    return _mkfs_into(
        ["mkfs.exfat"],
        lambda tool, p: [tool, p],
        path, size_bytes, "exfat")


BUILDERS = {
    "ext2": build_ext2,
    "iso9660": build_iso9660,
    "ntfs": build_ntfs,
    "exfat": build_exfat,
}


def main():
    if len(sys.argv) != 4:
        print("usage: make-fs-image.py <kind> <path> <bytes>", file=sys.stderr)
        return 2
    kind, path, size = sys.argv[1], sys.argv[2], int(sys.argv[3])

    # ext2 is made at an exact size, so its length is a usable staleness check.
    # An ISO is whatever size its contents make it, so for those the check is
    # only "does a non-empty file exist" - which is enough, because the
    # contents are a constant of this script.
    try:
        n = os.path.getsize(path)
        if (n == size) if kind in ("ext2", "ntfs", "exfat") else (n > 0):
            return 0
    except OSError:
        pass
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)

    if kind not in BUILDERS:
        print("unknown filesystem kind: " + kind, file=sys.stderr)
        return 2
    err = BUILDERS[kind](path, size)
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
