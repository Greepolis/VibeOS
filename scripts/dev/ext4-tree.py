#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The host's side of scripts/dev/verify-ext4.sh (docs/filesystem-design/ext4.md).

    ext4-tree.py make <dir>                  build the tree mkfs.ext4 -d will put on a volume
    ext4-tree.py list <dir>                  list it as vibeos_ext4_exercise lists the volume
    ext4-tree.py script <file>               the changes step 2 makes, as the exerciser's script
    ext4-tree.py apply <dir>                 the same changes, made to the host's copy of the tree
    ext4-tree.py compare <want> <got> <img>  the tree debugfs dumped, held against the host's copy
    ext4-tree.py patcheck <file> <seed>      a file is the pattern, for as long as it is
    ext4-tree.py junk <img> <groups>         fill the free blocks of the first groups with 0xA5

The tree is chosen for what it makes the driver do, each entry named after it:
a file past twelve blocks, a large contiguous one, a sparse file of forty
separate pieces - more extents than the four an inode holds, so the tree has a
level of index blocks - an empty file, a directory deep enough to walk, and one
with six hundred entries, which mkfs.ext4 indexes with htree. Contents are a
function of the path, so a block read from the wrong file shows as a wrong
crc32 rather than as a plausible one.

The changes are a scenario written once, as data (SCENARIO below). `script`
writes it for the exerciser, which runs it through the driver; `apply` runs it on
the host's tree with the host's own calls; `compare` holds the two results
together after debugfs has read the volume back. The scenario's file contents
are pat(seed, position), the same function the exerciser computes.
"""

import os
import re
import subprocess
import sys
import zlib

MIB = 1024 * 1024
RAMP = bytes(range(256)) * 2


def body(path, n):
    seed = zlib.crc32(path.encode())
    out = bytearray(n)
    x = seed or 1
    for i in range(n):
        x = (x * 1103515245 + 12345) & 0x7FFFFFFF
        out[i] = (x >> 16) & 0xFF
    return bytes(out)


def pat(seed, off, n):
    """n bytes of the pattern from position off: byte p is seed*31 + (p>>8)*(2*seed+1) + (p&255), mod 256."""
    out = bytearray()
    p = off
    end = off + n
    while p < end:
        k = p >> 8
        base = (seed * 31 + k * ((seed << 1) | 1)) & 0xFF
        i = p & 255
        take = min(256 - i, end - p)
        out += RAMP[base + i:base + i + take]
        p += take
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
    # Four hundred pieces: more than a leaf block holds (340), so the index has
    # two entries and a lookup has to choose between them.
    sparse2 = os.path.join(root, "sparse-400-extents.bin")
    with open(sparse2, "wb") as f:
        for k in range(400):
            f.seek(k * 65536)
            f.write(body("sparse400-%d" % k, 4096))
    for depth in range(1, 7):
        put(root, "/".join(["deep"] * depth) + "/level%d.txt" % depth, 300 * depth)
    for k in range(600):
        put(root, "many/entry-%04d with a longer name.txt" % k, k % 50)
    put(root, "accent è ü 中.txt", 64)
    # A hundred blocks of zeros: the verify script has debugfs fallocate it, so
    # the volume holds an extent that is allocated and not written - which
    # mkfs.ext4 -d never makes - and reads as zeros.
    with open(os.path.join(root, "prealloc.bin"), "wb") as f:
        f.truncate(100 * 4096)


def listing(root):
    rows = []
    for d, dirs, files in os.walk(root):
        rel = os.path.relpath(d, root)
        base = "" if rel == "." else "/" + rel.replace(os.sep, "/")
        for name in dirs:
            p = os.path.join(d, name)
            if os.path.islink(p):
                rows.append("%s/%s\tL\t%s" % (base, name, os.readlink(p)))
            else:
                rows.append("%s/%s\tD" % (base, name))
        for name in files:
            p = os.path.join(d, name)
            if name == ".ext4-meta":
                continue
            if os.path.islink(p):
                rows.append("%s/%s\tL\t%s" % (base, name, os.readlink(p)))
                continue
            with open(p, "rb") as f:
                data = f.read()
            rows.append("%s/%s\tF\t%d\t%08x" % (base, name, len(data), zlib.crc32(data) & 0xFFFFFFFF))
    for r in sorted(rows):
        print(r)


# ---- the scenario -----------------------------------------------------------

LONG_NAME = "n" * 255
LONG_TARGET = "/".join(["very-long-target-component"] * 4)           # 111 bytes: a block, not the inode
HUGE_TARGET = "/".join(["t%03d" % k for k in range(400)])             # 1999 bytes
SHORT_TARGET = "../small.txt"                                         # in the inode


def scenario(which="main"):
    s = []
    c = s.append
    if which == "fresh":
        # A volume mkfs.ext4 made empty, whose second group is INODE_UNINIT: four
        # hundred files in one directory outgrow the first group's inodes, so
        # the next group's bitmap is made on first use.
        c(("mkdir", "/d"))
        c(("fill", "/d", 400, 2000))
        c(("mkdir", "/d/sub"))
        c(("fill", "/d/sub", 60, 5000))
        c(("create", "/top", "644"))
        c(("write", "/top", 0, 3 * MIB, 61))
        c(("symlink", "/d/f00000", "/ln"))
        c(("rename", "/d/sub", "/sub-at-top"))
        return s
    # --- files: create, append, overwrite, holes -----------------------------
    c(("create", "/new.txt", "644"))
    c(("write", "/new.txt", 0, 100, 7))
    c(("write", "/new.txt", 100, 5000, 7))                        # an append across a block edge
    c(("stat", "/new.txt", 5100, 1))
    c(("fail", "EEXIST", "create", "/small.txt", "644"))
    c(("fail", "ENAMETOOLONG", "create", "/" + "n" * 256, "644"))
    c(("fail", "ENOTDIR", "create", "/small.txt/x", "644"))
    c(("fail", "ENOENT", "create", "/no-such-dir/x", "644"))
    c(("write", "/small.txt", 50, 200, 3))                        # over its end
    c(("write", "/past-twelve-blocks.bin", 20000, 70000, 5))      # inside, partial blocks at both ends
    c(("write", "/empty", 3 * 4096 + 10, 20, 9))                  # a file that is nothing but a hole and 20 bytes
    c(("create", "/" + LONG_NAME, "600"))
    # --- truncate ------------------------------------------------------------
    c(("truncate", "/large-contiguous.bin", MIB + 7))             # down, into a block
    c(("truncate", "/large-contiguous.bin", 2 * MIB))             # up: a hole
    c(("write", "/large-contiguous.bin", 2 * MIB - 10, 20, 13))   # across the old end and the new
    c(("truncate", "/new.txt", 3))
    c(("write", "/new.txt", 10, 4, 7))                            # the gap must read as zeros
    # --- an unwritten extent --------------------------------------------------
    c(("write", "/prealloc.bin", 40000, 5000, 21))
    c(("write", "/prealloc.bin", 0, 4096, 22))
    c(("write", "/prealloc.bin", 100 * 4096 - 5, 5, 23))
    # --- trees that change depth --------------------------------------------
    c(("write", "/sparse-forty-extents.bin", 5 * MIB + 3 * 4096, 2 * 4096 + 3, 11))
    c(("truncate", "/sparse-forty-extents.bin", 20 * MIB))        # half the pieces go; the tree stays two-level
    c(("truncate", "/sparse-forty-extents.bin", 3 * MIB + 4096))  # four pieces: back into the inode
    c(("write", "/sparse-forty-extents.bin", 9 * MIB, 4096, 12))  # and out again
    c(("write", "/sparse-400-extents.bin", 350 * 65536 + 3 * 4096, 5000, 62))   # a hole under the second leaf
    c(("write", "/sparse-400-extents.bin", 10 * 65536 + 4096, 4096, 63))        # and one under the first
    c(("truncate", "/sparse-400-extents.bin", 100 * 65536))       # the second leaf goes
    c(("create", "/frag.bin", "644"))
    c(("stride", "/frag.bin", 600, 8192, 100, 31))                # 600 extents: two leaves
    c(("truncate", "/frag.bin", 100 * 8192))
    c(("stride", "/frag.bin", 150, 8192, 60, 32))                 # filling holes between: a few merge
    # --- directories ---------------------------------------------------------
    c(("mkdir", "/d1"))
    c(("mkdir", "/d1/d2"))
    c(("mkdir", "/d1/d2/d3"))
    c(("mkdir", "/d1/d2/d3/d4"))
    c(("create", "/d1/d2/d3/d4/leaf.txt", "640"))
    c(("write", "/d1/d2/d3/d4/leaf.txt", 0, 1234, 14))
    c(("rename", "/d1/d2", "/d1/d2x"))                            # a directory, in place
    c(("rename", "/d1/d2x/d3", "/d3moved"))                       # a directory, to another parent
    c(("rename", "/small.txt", "/d1/small-moved.txt"))            # a file, across directories
    c(("create", "/a", "644"))
    c(("create", "/b", "644"))
    c(("write", "/a", 0, 9000, 15))
    c(("write", "/b", 0, 9000, 16))
    c(("fail", "EEXIST", "renamenx", "/a", "/b"))
    c(("rename", "/a", "/b"))                                     # a file over a file: b's blocks go
    c(("mkdir", "/e1"))
    c(("mkdir", "/e2"))
    c(("rename", "/e1", "/e2"))                                   # a directory over an empty one
    c(("mkdir", "/e3"))
    c(("create", "/e3/inside", "644"))
    c(("fail", "ENOTEMPTY", "rmdir", "/e3"))
    c(("fail", "EINVAL", "rename", "/d1", "/d1/d2x/zz"))
    c(("fail", "ENOENT", "rename", "/no-such", "/x"))
    c(("fail", "ENOTDIR", "rename", "/d1", "/b/under-a-file"))
    c(("unlink", "/e3/inside"))
    c(("rmdir", "/e3"))
    c(("rmdir", "/e2"))
    c(("mkdir", "/d3moved/more"))
    c(("rename", "/d3moved/more", "/d1/more"))
    # --- links and symlinks --------------------------------------------------
    c(("link", "/new.txt", "/new-hard.txt"))
    c(("stat", "/new.txt", 14, 2))
    c(("fail", "EPERM", "link", "/d1", "/d1-link"))
    c(("fail", "EEXIST", "link", "/new.txt", "/b"))
    c(("unlink", "/new.txt"))
    c(("stat", "/new-hard.txt", 14, 1))
    c(("symlink", SHORT_TARGET, "/ln-short"))
    c(("symlink", LONG_TARGET, "/ln-long"))
    c(("symlink", HUGE_TARGET, "/d1/ln-huge"))
    c(("symlink", "/d1", "/ln-dir"))
    c(("create", "/doomed", "644"))
    c(("write", "/doomed", 0, 300000, 17))
    c(("unlink", "/doomed"))
    c(("unlink", "/ln-dir"))
    # --- a big directory ------------------------------------------------------
    c(("create", "/many/zzz-new-entry", "644"))                   # the first change to an htree directory
    c(("unlink", "/many/entry-0000 with a longer name.txt"))
    c(("rename", "/many/entry-0001 with a longer name.txt", "/many/renamed within"))
    c(("mkdir", "/fillme"))
    c(("fill", "/fillme", 300, 3000))                             # more entries than a block holds; inodes in the next group
    c(("unlink", "/fillme/f00007"))
    c(("unlink", "/fillme/f00150"))
    c(("create", "/fillme/f99999", "644"))                        # into a record one of those left behind
    # --- attributes ----------------------------------------------------------
    c(("chmod", "/b", "600"))
    c(("chmod", "/d1", "700"))
    c(("chown", "/new-hard.txt", 1234, 5678))
    c(("chown", "/d1/ln-huge", 70000, 80000))                     # past 16 bits: the high halves
    c(("utimes", "/empty", 1000000000, 1100000000))
    c(("utimes", "/d1", 1200000000, 1300000000))
    # --- whole-file writes ----------------------------------------------------
    c(("writefile", "/whole.bin", 123456, 18))
    c(("writefile", "/whole.bin", 777, 19))                       # over an existing file: shorter
    # --- enough data to leave the first group (and its uninitialised siblings) -
    c(("create", "/big.bin", "644"))
    c(("write", "/big.bin", 0, 200 * MIB, 41))
    c(("write", "/big.bin", 50 * MIB + 1, 3 * MIB, 42))           # overwrite in the middle: no new blocks
    c(("truncate", "/big.bin", 120 * MIB + 5))                    # blocks go back in more than one group
    c(("write", "/big.bin", 150 * MIB, 4096, 43))
    c(("write", "/many/zzz-new-entry", 0, 40000, 44))
    return s


def script(path, which="main"):
    with open(path, "w") as f:
        for cmd in scenario(which):
            if cmd is not None:
                f.write("\t".join(str(x) for x in cmd) + "\n")


def apply_one(root, cmd, meta):
    def hp(p):
        return root + p

    name = cmd[0]
    if name == "fail" or name == "stat":
        return                                      # the exerciser's own checks; the host's tree does not change
    if name == "create":
        open(hp(cmd[1]), "xb").close()
        os.chmod(hp(cmd[1]), int(cmd[2], 8))
    elif name == "write":
        _, p, off, n, seed = cmd
        with open(hp(p), "r+b") as f:
            f.seek(off)
            done = 0
            while done < n:
                piece = min(n - done, MIB)
                f.write(pat(seed, off + done, piece))
                done += piece
    elif name == "stride":
        _, p, count, step, n, seed = cmd
        with open(hp(p), "r+b") as f:
            for k in range(count):
                f.seek(k * step)
                f.write(pat(seed, k * step, n))
    elif name == "truncate":
        os.truncate(hp(cmd[1]), cmd[2])
    elif name == "mkdir":
        os.mkdir(hp(cmd[1]))
        # The mode the driver gives a directory it makes, so that the two trees
        # compare: 0755 is what mkdir means here, not a widening.
        os.chmod(hp(cmd[1]), 0o755)  # nosemgrep: python.lang.security.audit.insecure-file-permissions.insecure-file-permissions
    elif name == "rmdir":
        os.rmdir(hp(cmd[1]))
    elif name == "unlink":
        os.unlink(hp(cmd[1]))
    elif name in ("rename", "renamenx"):
        os.rename(hp(cmd[1]), hp(cmd[2]))
    elif name == "link":
        os.link(hp(cmd[1]), hp(cmd[2]))
    elif name == "symlink":
        os.symlink(cmd[1], hp(cmd[2]))
    elif name == "chmod":
        os.chmod(hp(cmd[1]), int(cmd[2], 8))
    elif name == "chown":
        meta["owner"][cmd[1]] = (cmd[2], cmd[3])    # the host user cannot chown; debugfs stat says what the volume has
    elif name == "utimes":
        os.utime(hp(cmd[1]), (cmd[2], cmd[3]))
        meta["times"][cmd[1]] = (cmd[2], cmd[3])
    elif name == "fill":
        _, d, count, n = cmd
        for k in range(count):
            p = "%s/f%05d" % (d, k)
            with open(hp(p), "xb") as f:
                f.write(pat(k + 1, 0, n))
            os.chmod(hp(p), 0o644)
    elif name == "writefile":
        _, p, n, seed = cmd
        with open(hp(p), "wb") as f:
            f.write(pat(seed, 0, n))
        os.chmod(hp(p), 0o644)
    else:
        raise SystemExit("ext4-tree.py: unknown scenario command %r" % (cmd,))


def renamed(meta, old, new):
    """A rename moves what the scenario recorded about the path (and below it)."""
    for table in meta.values():
        for k in list(table):
            if k == old or k.startswith(old + "/"):
                table[new + k[len(old):]] = table.pop(k)


def apply_tree(root, which="main"):
    meta = {"owner": {}, "times": {}}
    for cmd in scenario(which):
        if cmd is None:
            continue
        apply_one(root, cmd, meta)
        if cmd[0] in ("rename", "renamenx"):
            renamed(meta, cmd[1], cmd[2])
    # Where the paths ended up is what the check needs.
    with open(os.path.join(root, ".ext4-meta"), "w") as f:
        for p, (u, g) in sorted(meta["owner"].items()):
            f.write("owner\t%s\t%d\t%d\n" % (p, u, g))
        for p, (a, m) in sorted(meta["times"].items()):
            f.write("times\t%s\t%d\t%d\n" % (p, a, m))


# ---- the comparison ---------------------------------------------------------

def file_crc(path):
    crc = 0
    with open(path, "rb") as f:
        while True:
            chunk = f.read(4 * MIB)
            if not chunk:
                break
            crc = zlib.crc32(chunk, crc)
    return crc & 0xFFFFFFFF


def walk_tree(root):
    rows = {}
    for d, dirs, files in os.walk(root):
        rel = os.path.relpath(d, root)
        base = "" if rel == "." else "/" + rel.replace(os.sep, "/")
        for name in dirs + files:
            if name == ".ext4-meta" or (base == "" and name == "lost+found"):
                continue
            rows[base + "/" + name] = os.path.join(d, name)
    return rows


def debugfs_stat(img, path):
    out = subprocess.run(["debugfs", "-R", 'stat "%s"' % path, img], capture_output=True, text=True).stdout
    m = re.search(r"User:\s+(\d+)\s+Group:\s+(\d+)", out)
    return (int(m.group(1)), int(m.group(2))) if m else None


def compare(want, got, img):
    bad = []
    a = walk_tree(want)
    b = walk_tree(got)
    for p in sorted(set(a) - set(b)):
        bad.append("missing from the volume: " + p)
    for p in sorted(set(b) - set(a)):
        bad.append("not expected on the volume: " + p)
    for p in sorted(set(a) & set(b)):
        x, y = a[p], b[p]
        xl, yl = os.path.islink(x), os.path.islink(y)
        if xl != yl:
            bad.append("%s: link on one side only" % p)
        elif xl:
            if os.readlink(x) != os.readlink(y):
                bad.append("%s: link target differs" % p)
        elif os.path.isdir(x) != os.path.isdir(y):
            bad.append("%s: directory on one side only" % p)
        elif os.path.isdir(x):
            if (os.stat(x).st_mode & 0o7777) != (os.stat(y).st_mode & 0o7777):
                bad.append("%s: mode %o, volume %o" % (p, os.stat(x).st_mode & 0o7777, os.stat(y).st_mode & 0o7777))
        else:
            sx, sy = os.stat(x), os.stat(y)
            if sx.st_size != sy.st_size:
                bad.append("%s: size %d, volume %d" % (p, sx.st_size, sy.st_size))
            elif file_crc(x) != file_crc(y):
                bad.append("%s: contents differ" % p)
            if (sx.st_mode & 0o7777) != (sy.st_mode & 0o7777):
                bad.append("%s: mode %o, volume %o" % (p, sx.st_mode & 0o7777, sy.st_mode & 0o7777))
        if len(bad) > 12:
            break
    # What cannot be read back through the host's calls: owners, set by the
    # volume's own inode (debugfs stat), and times that were set explicitly.
    with open(os.path.join(want, ".ext4-meta")) as f:
        for line in f:
            kind, p, u, v = line.rstrip("\n").split("\t")
            if kind == "owner":
                got_owner = debugfs_stat(img, p)
                if got_owner != (int(u), int(v)):
                    bad.append("%s: owner %s, volume %s" % (p, (u, v), got_owner))
            elif p in b and not os.path.islink(b[p]):
                if int(os.stat(b[p]).st_mtime) != int(v):
                    bad.append("%s: mtime %s, volume %d" % (p, v, int(os.stat(b[p]).st_mtime)))
    for line in bad[:12]:
        print(line)
    print("compare=%s entries=%d" % ("FAILED" if bad else "ok", len(a)))
    return 1 if bad else 0


def junk(img, ngroups):
    """Fill the free blocks of the first `ngroups` groups with 0xA5.

    A volume mkfs just made is zeros wherever nothing was written, and that
    forgives every block a driver forgets to zero (CLAUDE.md: a volume that is
    zero wherever nobody wrote forgives every missing memset). With junk in the
    free space a block taken from it and not written over shows."""
    out = subprocess.run(["dumpe2fs", img], capture_output=True, text=True).stdout
    bs = int(re.search(r"Block size:\s+(\d+)", out).group(1))
    g = -1
    ranges = []
    for line in out.splitlines():
        m = re.match(r"Group (\d+):", line)
        if m:
            g = int(m.group(1))
            continue
        m = re.match(r"\s+Free blocks: (.*)", line)
        if m and 0 <= g < ngroups:
            for part in m.group(1).split(","):
                part = part.strip()
                if part:
                    a, _, b = part.partition("-")
                    ranges.append((int(a), int(b or a)))
    with open(img, "r+b") as f:
        for a, b in ranges:
            f.seek(a * bs)
            left = (b - a + 1) * bs
            while left > 0:
                n = min(left, 16 * MIB)
                f.write(bytes([0xA5]) * n)
                left -= n
    print("junk ranges=%d" % len(ranges))


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    if cmd in ("make", "list", "script", "apply") and len(sys.argv) in (3, 4):
        which = sys.argv[3] if len(sys.argv) == 4 else "main"
        if cmd in ("script", "apply"):
            {"script": script, "apply": apply_tree}[cmd](sys.argv[2], which)
        else:
            {"make": make, "list": listing}[cmd](sys.argv[2])
    elif cmd == "junk" and len(sys.argv) == 4:
        junk(sys.argv[2], int(sys.argv[3]))
    elif cmd == "patcheck" and len(sys.argv) == 4:
        # patcheck <file> <seed>: the file is the pattern, whatever its length is.
        with open(sys.argv[2], "rb") as f:
            data = f.read()
        ok = data == pat(int(sys.argv[3]), 0, len(data))
        print("patcheck=%s size=%d" % ("ok" if ok else "FAILED", len(data)))
        sys.exit(0 if ok else 1)
    elif cmd == "compare" and len(sys.argv) == 5:
        sys.exit(compare(sys.argv[2], sys.argv[3], sys.argv[4]))
    else:
        print(__doc__, file=sys.stderr)
        sys.exit(2)


if __name__ == "__main__":
    main()
