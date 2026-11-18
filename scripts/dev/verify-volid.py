#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""What identifies a volume (hierarchy F2 step 1), against blkid.

    scripts/dev/verify-volid.py <build-dir>

Makes one image of each filesystem the scan mounts with the host's own tools,
with a known serial or UUID and label where the tool takes one, and holds the
driver's answer (vibeos_volid_cli) to blkid's for the same image: the one
reader neither the driver nor this script wrote. A manifest line copied from
blkid has to name the volume the kernel finds.

Prints volid=ok images=<n>, volid=skip with the missing tool, or volid=FAILED.
"""
import os
import shutil
import subprocess
import sys
import tempfile


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def blkid(img):
    r = run(["blkid", "-p", "-o", "export", img])
    out = {}
    for l in r.stdout.splitlines():
        if "=" in l:
            k, v = l.split("=", 1)
            out[k] = v.replace("\\", "")   # export quotes a space as "\ "
    return out.get("UUID", ""), out.get("LABEL", "")


def in_partition(fs_img, disk):
    """The image as partition 1 of an MBR disk, 1 MiB in, as the boot disk has
    its volumes: FAT at sector 0 is the boot volume to this driver, which
    mounts through the boot path, so a scanned FAT has to be a partition."""
    data = open(fs_img, "rb").read()
    sectors = len(data) // 512
    mbr = bytearray(512)
    mbr[446:462] = bytes([0, 0xFE, 0xFF, 0xFF, 0x0C, 0xFE, 0xFF, 0xFF]) + \
        (2048).to_bytes(4, "little") + sectors.to_bytes(4, "little")
    mbr[510:512] = b"\x55\xaa"
    with open(disk, "wb") as f:
        f.write(mbr)
        f.truncate(2048 * 512)
        f.seek(2048 * 512)
        f.write(data)


def main():
    build = sys.argv[1] if len(sys.argv) > 1 else "build-gcc-Release"
    cli = os.path.abspath(os.path.join(build, "vibeos_volid_cli"))
    if not os.path.exists(cli):
        print("volid=FAILED reason=cli_not_built")
        return 1
    for tool in ("blkid", "mkfs.ext4", "mformat", "mkfs.exfat", "mkntfs", "xorriso"):
        if not shutil.which(tool):
            print("volid=skip reason=no_%s" % tool)
            return 0
    bad, n = [], 0
    with tempfile.TemporaryDirectory(prefix="volid-") as d:
        def img(name, mb):
            p = os.path.join(d, name)
            with open(p, "wb") as f:
                f.truncate(mb << 20)
            return p

        made = []
        p = img("ext4.img", 16)
        run(["mkfs.ext4", "-q", "-F", "-L", "vibeos-root", "-U", "0b3e9c4a-5d6f-4a1b-9c8d-7e6f5a4b3c2d", p])
        made.append(("ext4", p))
        p = img("ext2.img", 8)
        run(["mkfs.ext4", "-q", "-F", "-t", "ext2", "-L", "Old Data", p])
        made.append(("ext2", p))
        p = img("fat16.img", 16)
        run(["mformat", "-i", p, "-N", "1a2b3c4d", "-v", "BOOT VOL", "::"])
        made.append(("fat", p))
        p = img("fat32.img", 64)
        run(["mformat", "-i", p, "-F", "-N", "deadbeef", "-v", "ESP", "::"])
        made.append(("fat32", p))
        p = img("fatnolabel.img", 8)
        run(["mformat", "-i", p, "-N", "00c0ffee", "::"])
        made.append(("fat-nolabel", p))
        p = img("exfat.img", 16)
        run(["mkfs.exfat", p])
        made.append(("exfat", p))
        p = img("ntfs.img", 16)
        run(["mkntfs", "-q", "-F", "-f", p])
        made.append(("ntfs", p))
        src = os.path.join(d, "isosrc")
        os.mkdir(src)
        open(os.path.join(src, "HELLO.TXT"), "w").write("hello\n")
        p = os.path.join(d, "cd.iso")
        run(["xorriso", "-as", "mkisofs", "-quiet", "-V", "VIBE_CD", "-o", p, src])
        made.append(("iso9660", p))

        for what, p in made:
            n += 1
            uuid, label = blkid(p)
            if what.startswith("fat"):
                disk = p + ".disk"
                in_partition(p, disk)
                p = disk
            # The CLI reads volid.img where it runs, a fixed name.
            shutil.copy(p, os.path.join(d, "volid.img"))
            r = run([cli], cwd=d)
            line = r.stdout.strip().split("\n")[0] if r.stdout.strip() else ""
            fields = dict(x.split("=", 1) for x in line.split(" ") if "=" in x) if line else {}
            # A label that holds a space reaches the CLI's line whole only as
            # the rest of it.
            if "label=" in line:
                fields["label"] = line.split("label=", 1)[1]
            got = (fields.get("uuid", ""), fields.get("label", ""))
            if not line:
                bad.append("%s: not mounted (%s)" % (what, r.stderr.strip()[:80]))
            elif got != (uuid, label):
                bad.append("%s: driver %r, blkid %r" % (what, got, (uuid, label)))
            elif not uuid:
                bad.append("%s: blkid has no UUID for it - the test says nothing" % what)
    if bad:
        print("\n".join(bad))
        print("volid=FAILED differences=%d of %d" % (len(bad), n))
        return 1
    print("volid=ok images=%d" % n)
    return 0


if __name__ == "__main__":
    sys.exit(main())
