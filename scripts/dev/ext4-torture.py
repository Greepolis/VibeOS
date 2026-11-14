#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""ext4's intensive test (docs/filesystem-design/ext4.md step 5).

    ext4-torture.py <build-dir> <first-seed> <seeds> [--ops N] [--cuts K] [-j J] [--keep DIR]

For every seed, a random sequence of operations is made up - create, write at an
offset, truncate, mkdir, rmdir, unlink, rename, link, symlink, chmod, the
whole-file write - and run twice: by the host's own kernel on a directory, and
by the driver (vibeos_ext4_exercise) on a volume mkfs.ext4 made. The model is
Linux. Nobody here decides what an operation should have done: whatever the
host's call returned, success or an errno, is what the script demands of the
driver, as `fail ERRNO ...` when Linux refused.

Each seed is then judged by tools that are not the driver:

  - e2fsck -fn must call the volume the driver left clean;
  - the tree read back through the driver must be the host's tree, line for
    line: paths, types, sizes, link targets and the crc32 of every file;
  - for --cuts seeds' worth of random points, the device is stopped after write
    n (a power cut), the driver's replay and e2fsck's own replay must each give
    a clean volume, the two must agree, and the tree must be one the host's
    tree passed through - never a mixture of two operations.

The seed picks the block size (1, 2 or 4 KiB), the inode count and the mix of
operations, so a run covers more than one volume shape. Names come from a small
pool, so operations collide: an existing name, a directory that is not empty,
a rename over something already there.

The seed is printed on every failure, and with --keep the failing seed's script,
volume and both listings are left in DIR (never removed by this script).

Prints ext4_torture=ok seeds=<n> ops=<n> cuts=<n> torn_writes=<n>, =skip reason=..., or
ext4_torture=FAILED seed=<s> reason=<why>.
"""

import argparse
import errno
import os
import random
import shutil
import stat
import subprocess
import sys
import tempfile
import zlib
from concurrent.futures import ProcessPoolExecutor

RAMP = bytes(range(256)) * 2

# The errnos the exerciser's `fail` line knows (errno_of in ext4_exercise.c).
# A refusal Linux gives with any other errno is left out of the script: a
# failed call changed nothing, so leaving it out changes nothing either.
KNOWN = {
    errno.EPERM: "EPERM", errno.ENOENT: "ENOENT", errno.EEXIST: "EEXIST",
    errno.ENOTDIR: "ENOTDIR", errno.EISDIR: "EISDIR", errno.EINVAL: "EINVAL",
    errno.ENOTEMPTY: "ENOTEMPTY", errno.ENAMETOOLONG: "ENAMETOOLONG",
}

NAMES = ["a", "b", "c", "dir", "x" * 40, "long-" + "n" * 200, "f1", "f2", "sub"]


def pat(seed, off, n):
    """The exerciser's pat(): byte p is seed*31 + (p>>8)*(2*seed+1) + (p&255), mod 256."""
    out = bytearray()
    p, end = off, off + n
    while p < end:
        base = (seed * 31 + (p >> 8) * ((seed << 1) | 1)) & 0xFF
        i = p & 255
        take = min(256 - i, end - p)
        out += RAMP[base + i:base + i + take]
        p += take
    return bytes(out)


def listing(root, owners, pinned=False):
    """The tree as `vibeos_ext4_exercise walk attrs` prints it, sorted, after
    norm(): each file and directory with its permission bits, owner and group
    (E4-027). The owner is the generator's model, not the host's - the host's
    run is not root, so a chown there is only recorded (Gen.owners). The mtime
    is compared only where utimes set it and nothing has changed it since
    (pinned): the host's clock and the exerciser's are different clocks, and
    every other mtime is one of them. A cut's states never carry it."""
    rows = []
    for d, dirs, files in os.walk(root):
        rel = os.path.relpath(d, root)
        base = "" if rel == "." else "/" + rel.replace(os.sep, "/")
        for name in dirs + files:
            p = os.path.join(d, name)
            st = os.lstat(p)
            if os.path.islink(p):
                rows.append("%s/%s\tL\t%s" % (base, name, os.readlink(p)))
                continue
            u, g = owners.get(st.st_ino, (0, 0))
            mt = str(st.st_mtime_ns // 10**9) if pinned and is_pinned(st) else "-"
            tail = "\t%o\t%d\t%d\t%s" % (st.st_mode & 0o7777, u, g, mt)
            if os.path.isdir(p):
                rows.append("%s/%s\tD%s" % (base, name, tail))
            else:
                with open(p, "rb") as f:
                    data = f.read()
                rows.append("%s/%s\tF\t%d\t%08x%s" % (base, name, len(data), zlib.crc32(data) & 0xFFFFFFFF, tail))
    return "\n".join(sorted(rows)) + "\n"


# utimes sets whole seconds in this range; Linux's own clock never lands on an
# exact second inside it, so a file whose mtime is one is a file utimes set
# and nothing has written since.
PIN_LO, PIN_HI = 1000000000, 1500000000


def is_pinned(st):
    return st.st_mtime_ns % 10**9 == 0 and PIN_LO <= st.st_mtime_ns // 10**9 < PIN_HI and not stat.S_ISDIR(st.st_mode)


def norm(text, pins):
    """The exerciser's listing with every mtime the host cannot vouch for as
    '-': all of them for a cut's state, the pinned files' kept otherwise."""
    out = []
    for l in text.splitlines():
        w = l.split("\t")
        if len(w) >= 6 and w[1] in ("F", "D") and w[0] not in pins:
            w[-1] = "-"
        out.append("\t".join(w))
    return "\n".join(sorted(out)) + "\n"


class Gen:
    """Makes up operations and runs each on the host's tree, which decides the answer."""

    def __init__(self, rng, root, budget, inodes, block, states=True):
        self.rng = rng
        # The writer keeps a tree two index levels deep at most: four index
        # blocks under the root, of (bs-12)/12 leaves of as many extents each
        # (E4-008). Past that a write is EFBIG where Linux grows a third level.
        # Each write into a file can add at most two extents - its own and the
        # one a hole splits off - so the writes a file may take are counted and
        # kept under half of that; and under 3,000, so that a seed stays a
        # seed's worth of time. Files past one leaf's worth, past four leaves
        # and past one index block are all inside that, on 1 KiB blocks.
        per = (block - 12) // 12
        self.max_writes = min((4 * per * per - 8) // 2, 3000)
        self.writes = {}   # st_ino -> writes made into it
        self.inodes = inodes   # names the tree may hold: more and Linux, unbounded, would disagree
        self.root = root
        self.budget = budget   # bytes the files may hold at once: the volume is small
        self.lines = []
        # The tree after every line, which only a cut needs to judge by: listing
        # it reads every file, so a seed without cuts keeps only the last one.
        self.keep_states = states
        # Owners as the driver must hold them: what mkfs.ext4 -d copied for
        # the tree it was given, root for anything made since, and whatever
        # chown said after that. Keyed by inode, pruned of the dead after every
        # line - the host reuses numbers.
        self.owners = {}
        for q, k in self.entries():
            if k != "l":
                st = os.lstat(self.host(q))
                self.owners[st.st_ino] = (st.st_uid, st.st_gid)
        self.states = [listing(root, self.owners)]
        # For a write over bytes the file already had: line index -> (every name
        # of the file, its bytes before, its bytes after). See torn_write().
        self.mix = {}

    def host(self, rel):
        return self.root + rel

    def entries(self):
        """Every path in the tree: (rel, kind) with kind f, d or l."""
        out = []
        for d, dirs, files in os.walk(self.root):
            rel = os.path.relpath(d, self.root)
            base = "" if rel == "." else "/" + rel.replace(os.sep, "/")
            for name in dirs + files:
                p = os.path.join(d, name)
                kind = "l" if os.path.islink(p) else "d" if os.path.isdir(p) else "f"
                out.append((base + "/" + name, kind))
        return sorted(out)

    def dirs(self):
        return ["/"] + [p for p, k in self.entries() if k == "d"]

    def used(self):
        n = 0
        for p, k in self.entries():
            if k == "f":
                # The size, not st_blocks: what the host allocated depends on
                # its filesystem and when it flushed, and a seed has to make
                # the same sequence on every machine to be replayed.
                n += os.lstat(self.host(p)).st_size
        return n

    def fresh(self):
        """A name in some directory: may exist already, which is the point."""
        d = self.rng.choice(self.dirs())
        return (d.rstrip("/") + "/" + self.rng.choice(NAMES)) if d != "/" else "/" + self.rng.choice(NAMES)

    def pick(self, kinds):
        c = [p for p, k in self.entries() if k in kinds]
        return self.rng.choice(c) if c else None

    def emit(self, words, fn):
        """Run fn on the host; record the line with Linux's answer."""
        try:
            fn()
        except OSError as e:
            name = KNOWN.get(e.errno)
            if name is None:
                return
            # unlink and mkdir answer 0 or -1 by contract (vfs.h): the errno is
            # the Linux handler's to decide, so the driver is only asked to refuse.
            words = (["refuse"] if words[0] in ("unlink", "mkdir") else ["fail", name]) + words
        self.lines.append("\t".join(str(w) for w in words))
        # Everything keyed by the host's inode number forgets the dead after
        # every line, before any later line can make a file. The host gives a
        # removed file's number to the next file it makes whenever it likes -
        # which depends on what else is using its disk - so a count left until
        # the next write was inherited by a new file one run in some, and the
        # generator made a different sequence (E4-031: 26135 operations where
        # it usually made 26140, measured in 45 of 60 seeds as files born
        # with a dead file's count).
        live = {os.lstat(self.host(q)).st_ino for q, k in self.entries() if k != "l"}
        self.owners = {i: o for i, o in self.owners.items() if i in live}
        self.writes = {i: c for i, c in self.writes.items() if i in live}
        if self.keep_states:
            self.states.append(listing(self.root, self.owners))

    def pins(self):
        """The paths whose mtime utimes set and nothing has changed since."""
        return {q for q, k in self.entries() if k == "f" and is_pinned(os.lstat(self.host(q)))}

    def write(self, p, off, n, seed, word="write"):
        ino = os.lstat(self.host(p)).st_ino
        if self.writes.get(ino, 0) >= self.max_writes:
            return
        self.writes[ino] = self.writes.get(ino, 0) + 1
        with open(self.host(p), "rb") as f:
            before = f.read()
        names = [q for q, k in self.entries() if k == "f" and os.path.samefile(self.host(q), self.host(p))]

        def w():
            fd = os.open(self.host(p), os.O_WRONLY)
            try:
                os.pwrite(fd, pat(seed, off, n), off)
            finally:
                os.close(fd)
        at = len(self.lines)
        self.emit([word, p, off, n, seed], w)
        if off < len(before) and len(self.lines) > at:
            with open(self.host(p), "rb") as f:
                self.mix[at] = (names, before, f.read())

    def step(self):
        r = self.rng
        op = r.choices(
            ["create", "write", "truncate", "mkdir", "rmdir", "unlink", "rename", "link",
             "symlink", "chmod", "writefile", "stride", "fill", "chown", "utimes", "bigwrite"],
            weights=[14, 22, 8, 10, 6, 8, 10, 5, 5, 4, 8, 3, 2, 4, 4, 1])[0]
        room = self.budget - self.used()
        if op in ("create", "mkdir", "symlink", "fill", "writefile") and len(self.entries()) >= self.inodes - 130:
            op = "unlink"

        if op == "create":
            p, mode = self.fresh(), r.choice([0o644, 0o600, 0o664])
            self.emit(["create", p, "%o" % mode],
                      lambda: os.close(os.open(self.host(p), os.O_CREAT | os.O_EXCL | os.O_WRONLY, mode)))
        elif op == "write":
            p = self.pick("f")
            if p is None or room < 8192:
                return
            off = r.choice([0, r.randrange(0, 70000), r.randrange(0, 600000), os.lstat(self.host(p)).st_size])
            n = min(r.choice([1, r.randrange(1, 5000), r.randrange(1, 90000), r.randrange(1, 400000)]),
                    room // 2)
            self.write(p, off, n, r.randrange(1, 250))
        elif op == "stride":
            # Pieces with holes between them, each its own write: a file of more
            # extents than the four an inode holds, so the tree grows a level.
            p = self.pick("f")
            if p is None:
                return
            # Sometimes past a leaf's worth (84 extents in a 1 KiB block), so the
            # index level has more than one leaf under it.
            count = r.choice([r.randrange(5, 40), r.randrange(90, 400)])
            step = r.choice([8192, 20000, 65536]) if count < 90 else 8192
            n = r.randrange(1, 3000)
            if count >= 90:
                n = r.randrange(1, 900)
            if room < count * (n + 8192) + 65536:
                return
            seed = r.randrange(1, 250)
            count = min(count, self.max_writes - self.writes.get(os.lstat(self.host(p)).st_ino, 0))
            for k in range(count):
                self.write(p, k * step + r.randrange(0, 200), n, seed)
        elif op == "fill":
            # A directory past one block, so it has to grow - and, on a volume
            # that started with an indexed one, a change to an htree directory.
            d = self.rng.choice(self.dirs())
            for k in range(r.randrange(20, 120)):
                q = (d.rstrip("/") + "/m%03d-" % k + "q" * r.randrange(0, 40))
                self.emit(["create", q, "644"],
                          lambda q=q: os.close(os.open(self.host(q), os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o644)))
        elif op == "truncate":
            p = self.pick("f")
            if p is None:
                return
            size = os.lstat(self.host(p)).st_size
            to = r.choice([0, size // 2, max(0, size - 1), size + r.randrange(1, 9000), r.randrange(0, 300000)])
            if to > size and room < to - size + 8192:
                return
            self.emit(["truncate", p, to], lambda: os.truncate(self.host(p), to))
        elif op == "mkdir":
            p = self.fresh()
            # 0755, the mode the driver's mkdir makes (the umask here is 0).
            self.emit(["mkdir", p], lambda: os.mkdir(self.host(p), 0o755))
        elif op == "rmdir":
            p = self.pick("d") if r.random() < 0.85 else self.fresh()
            if p is None:
                return
            self.emit(["rmdir", p], lambda: os.rmdir(self.host(p)))
        elif op == "unlink":
            p = self.pick("fl") if r.random() < 0.85 else self.fresh()
            if p is None or os.path.isdir(self.host(p)) and not os.path.islink(self.host(p)):
                return   # unlink of a directory: Linux's EISDIR is the VFS's, not the filesystem's
            self.emit(["unlink", p], lambda: os.unlink(self.host(p)))
        elif op == "rename":
            src = self.pick("fdl")
            if src is None:
                return
            dst = self.fresh()
            hs, hd = self.host(src), self.host(dst)
            # Into its own subtree, and onto itself, are refused or no-ops by
            # the VFS before a filesystem is asked: not the driver's question.
            if dst == src or dst.startswith(src + "/"):
                return
            src_dir = os.path.isdir(hs) and not os.path.islink(hs)
            if os.path.lexists(hd):
                dst_dir = os.path.isdir(hd) and not os.path.islink(hd)
                if src_dir != dst_dir:
                    return   # EISDIR/ENOTDIR across kinds is also the VFS's
                if os.path.samefile(hs, hd) if not os.path.islink(hs) and not os.path.islink(hd) else False:
                    return   # two names of one inode: Linux does nothing, by rule
                if dst_dir and src.startswith(dst + "/"):
                    return
            nx = r.random() < 0.2
            if nx:
                def rn():
                    if os.path.lexists(hd):
                        raise OSError(errno.EEXIST, "exists")
                    os.rename(hs, hd)
                self.emit(["renamenx", src, dst], rn)
            else:
                self.emit(["rename", src, dst], lambda: os.rename(hs, hd))
        elif op == "link":
            src = self.pick("f")
            if src is None:
                return
            dst = self.fresh()
            self.emit(["link", src, dst], lambda: os.link(self.host(src), self.host(dst)))
        elif op == "symlink":
            target = r.choice(["a", "../b", "/dir/f1", "t" * 59, "t" * 60, "u" * 61,
                               "/".join(["seg%02d" % k for k in range(r.randrange(10, 120))])])
            p = self.fresh()
            self.emit(["symlink", target, p], lambda: os.symlink(target, self.host(p)))
        elif op == "chmod":
            p = self.pick("fd")
            if p is None:
                return
            # Always with the owner's read and write (and search, for a
            # directory): the host's run is not root, and a mode that locked
            # it out of its own tree would end the sequence, not test it.
            mode = r.choice([0o700, 0o755, 0o750] if os.path.isdir(self.host(p)) else [0o600, 0o644, 0o640, 0o664])
            self.emit(["chmod", p, "%o" % mode], lambda: os.chmod(self.host(p), mode))
        elif op == "chown":
            # Recorded, not done: the host's run cannot give a file away. A
            # name that is not there is still asked, for Linux's ENOENT.
            p = self.pick("fd") if r.random() < 0.85 else self.fresh()
            if p is None or os.path.islink(self.host(p)):
                return
            u, g = r.choice([0, 1000, 65534, 123456]), r.choice([0, 100, 1000, 65534])

            def ch():
                self.owners[os.lstat(self.host(p)).st_ino] = (u, g)
            self.emit(["chown", p, u, g], ch)
        elif op == "utimes":
            p = self.pick("f")
            if p is None:
                return
            at, mt = r.randrange(PIN_LO, PIN_HI), r.randrange(PIN_LO, PIN_HI)
            self.emit(["utimes", p, at, mt], lambda: os.utime(self.host(p), (at, mt)))
        elif op == "bigwrite":
            # One call past 2 MiB (E4-026): `write` hands the driver at most
            # 1 MiB at a time.
            p = self.pick("f")
            if p is None or room < (2 << 20) + (1 << 20):
                return
            n = r.randrange((2 << 20) + 1, min(4 << 20, room - (512 << 10)))
            off = r.choice([0, os.lstat(self.host(p)).st_size, r.randrange(0, 70000)])
            self.write(p, off, n, r.randrange(1, 250), word="bigwrite")
        elif op == "writefile":
            p = self.pick("f") if r.random() < 0.6 else self.fresh()
            if p is None:
                return
            hp = self.host(p)
            if os.path.lexists(hp) and (os.path.islink(hp) or os.path.isdir(hp) or os.lstat(hp).st_nlink > 1):
                return   # whether replacing keeps the inode is not a Linux question
            if not os.path.isdir(os.path.dirname(hp)):
                return
            n = min(r.choice([0, r.randrange(1, 3000), r.randrange(1, 200000)]), max(0, room // 2))
            seed = r.randrange(1, 250)

            def wf():
                # A new file is 0644, as the driver's write_file makes it.
                with os.fdopen(os.open(hp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644), "wb") as f:
                    f.write(pat(seed, 0, n))
            self.emit(["writefile", p, n, seed], wf)


def torn_write(gen, mine, img):
    """Whether a tree that is no state is a write cut half-way over old bytes.

    ext4's ordered mode logs metadata and writes data in place. A write over
    bytes a file already has therefore reaches the disk before the commit that
    would make it part of a state, and a cut between the two leaves the old
    tree - same names, same sizes - with some of those bytes new. Linux leaves
    exactly that too; it is what ordered mode promises and no more.

    So it is accepted only in that shape: the tree is the state before some
    write over existing bytes, but for the crc of that file's names; the size
    is the old one; and every byte, read back by debugfs rather than the
    driver, is the old byte or the one the write put there."""
    got = set(mine.splitlines())
    for at, (names, old, new) in gen.mix.items():
        want = set(gen.states[at].splitlines())
        differ = got ^ want
        if not differ:
            continue
        rows = {l.split("\t")[0] for l in differ}
        if not rows <= set(names):
            continue
        if any(l.split("\t")[1:3] != ["F", str(len(old))] for l in differ):
            continue
        ok = True
        for name in rows:
            r = subprocess.run(["debugfs", "-R", "cat \"%s\"" % name, img], stdout=subprocess.PIPE,
                               stderr=subprocess.DEVNULL, timeout=120)
            data = r.stdout
            if len(data) != len(old) or any(
                    b != old[k] and (k >= len(new) or b != new[k]) for k, b in enumerate(data)):
                ok = False
                break
        if ok:
            return True
    return False


def run(cmd, cwd, timeout=300):
    return subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          timeout=timeout, universal_newlines=True)


def fsck_clean(img):
    r = run(["e2fsck", "-fn", img], None)
    return r.returncode == 0, r.stdout


def superblock_problem(img):
    """What e2fsck -n forgives in the superblock: its free counts are a summary
    the kernel is allowed to let drift, so e2fsck mends them without calling
    the volume unclean. This driver keeps them exact (step 2), so they must be
    the groups' sum - read by dumpe2fs, which also says if the superblock's own
    checksum does not match."""
    r = subprocess.run(["dumpe2fs", img], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       universal_newlines=True, timeout=120)
    if "checksum" in r.stdout.lower() and "does not match" in r.stdout.lower():
        return "superblock_checksum_wrong"
    sb_b = sb_i = None
    gb = gi = 0
    for l in r.stdout.splitlines():
        w = l.split()
        if l.startswith("Free blocks:") and len(w) == 3:
            sb_b = int(w[2])
        elif l.startswith("Free inodes:") and len(w) == 3:
            sb_i = int(w[2])
        elif len(w) >= 6 and w[1:3] == ["free", "blocks,"] and w[4:6] == ["free", "inodes,"]:
            gb += int(w[0].lstrip())
            gi += int(w[3])
    if sb_b is None or sb_i is None or gb == 0:
        return "dumpe2fs_unreadable"
    if sb_b != gb or sb_i != gi:
        return "superblock_counts_wrong"
    return ""


def walk(ex, d, pins=()):
    r = run([ex, "walk", "attrs"], d)
    if r.returncode != 0:
        return None, r.stdout
    # run() takes stderr with stdout; the walk's count of htree lookups is
    # stderr's, and is not a file of the tree.
    return norm("\n".join(l for l in r.stdout.splitlines() if l and not l.startswith("dx_lookups=")),
                set(pins)), ""


def one_seed(args):
    ex, seed, nops, cuts, keep = args
    rng = random.Random(seed)
    work = tempfile.mkdtemp(prefix="ext4-torture-")
    result = {"seed": seed, "ok": False, "ops": 0, "cuts": 0, "torn": 0, "why": "", "detail": ""}

    def failed(why, detail=""):
        result["why"], result["detail"] = why, detail.strip().replace("\n", " | ")[:400]
        if keep:
            dst = os.path.join(keep, "ext4-torture-seed-%d" % seed)
            if os.path.exists(dst):
                shutil.rmtree(dst)
            shutil.copytree(work, dst, symlinks=True)
        return result

    try:
        # The volume's shape: block size, inode count and size, so that some
        # seeds have one block group and others several - with groups mkfs left
        # uninitialised, which the driver has to bring up on first use.
        block = rng.choice([1024, 2048, 4096])
        inodes = rng.choice([512, 1024, 2048])
        mib = rng.choice([16, 64, 128])
        # A fifth without a journal: step 2's writer, which keeps the
        # superblock itself rather than through the log. No cuts on those - a
        # volume without a journal promises nothing about a power cut.
        journal = rng.random() >= 0.2
        old = os.umask(0)
        try:
            tree = os.path.join(work, "tree")
            os.mkdir(tree)
            # A third of the seeds start from a volume that already has a tree,
            # one directory of it large enough that e2fsck -D indexes it: the
            # driver does not make htree directories, but it has to change one
            # it is given without leaving it inconsistent.
            populated = rng.random() < 0.33
            if populated:
                big = os.path.join(tree, "big")
                os.mkdir(big)
                for k in range(300):
                    with open(os.path.join(big, "entry-%04d-%s" % (k, "p" * (k % 13))), "wb") as f:
                        f.write(pat(k + 1, 0, rng.randrange(0, 600)))
                os.mkdir(os.path.join(tree, "dir"))
                with open(os.path.join(tree, "dir", "f1"), "wb") as f:
                    f.write(pat(7, 0, 50000))
            start = os.path.join(work, "start")
            shutil.copytree(tree, start, symlinks=True)
            gen = Gen(rng, tree, budget=rng.choice([3, 6]) * 1024 * 1024, inodes=inodes - 64, block=block,
                      states=cuts > 0 and journal)
            for _ in range(nops):
                gen.step()
        finally:
            os.umask(old)
        result["ops"] = len(gen.lines)
        script = "\n".join(gen.lines) + "\n"

        base = os.path.join(work, "base.img")
        with open(base, "wb") as f:
            f.truncate(mib * 1024 * 1024)
        r = run(["mkfs.ext4", "-q", "-F", "-b", str(block), "-N", str(inodes)] + ([] if journal else ["-O", "^has_journal"]) + ["-d", start, base], None)
        if r.returncode != 0:
            return failed("mkfs_failed", r.stdout)
        if populated:
            r = run(["e2fsck", "-fyD", base], None)
            if r.returncode > 1:
                return failed("e2fsck_D_failed", r.stdout)

        # The whole script, through the driver.
        full = os.path.join(work, "full")
        os.mkdir(full)
        shutil.copy(base, os.path.join(full, "fs.img"))
        with open(os.path.join(full, "script.txt"), "w") as f:
            f.write(script)
        r = run([ex, "apply"], full)
        if r.returncode != 0:
            return failed("driver_apply_failed", r.stdout)
        ok, out = fsck_clean(os.path.join(full, "fs.img"))
        if not ok:
            return failed("volume_not_clean", out)
        why = superblock_problem(os.path.join(full, "fs.img"))
        if why:
            return failed(why)
        got, err = walk(ex, full, gen.pins())
        if got is None:
            return failed("driver_walk_failed", err)
        want = listing(gen.root, gen.owners, pinned=True)
        if got != want:
            with open(os.path.join(work, "want.txt"), "w") as f:
                f.write(want)
            with open(os.path.join(work, "got.txt"), "w") as f:
                f.write(got)
            diff = [l for l in set(want.splitlines()) ^ set(got.splitlines())][:4]
            return failed("tree_differs_from_linux", " ".join(diff))

        if cuts > 0 and journal:
            pc = os.path.join(work, "pc")
            os.mkdir(pc)
            shutil.copy(base, os.path.join(pc, "fs.img"))
            with open(os.path.join(pc, "script.txt"), "w") as f:
                f.write(script)
            r = run([ex, "powercut", "0"], pc)
            total = 0
            for l in r.stdout.splitlines():
                if l.startswith("writes="):
                    total = int(l.split()[0].split("=")[1])
            if r.returncode != 0 or total == 0:
                return failed("uncut_run_failed", r.stdout)
            states = set(gen.states)
            for n in sorted(rng.sample(range(1, total + 1), min(cuts, total))):
                c = os.path.join(work, "cut")
                if os.path.exists(c):
                    shutil.rmtree(c)
                os.mkdir(c)
                shutil.copy(base, os.path.join(c, "fs.img"))
                with open(os.path.join(c, "script.txt"), "w") as f:
                    f.write(script)
                r = run([ex, "powercut", str(n)], c)
                if r.returncode != 0:
                    return failed("cut_run_failed", "n=%d %s" % (n, r.stdout))
                theirs = os.path.join(c, "e")
                os.mkdir(theirs)
                shutil.copy(os.path.join(c, "fs.img"), os.path.join(theirs, "fs.img"))

                r = run([ex, "recover"], c)
                if r.returncode != 0:
                    return failed("driver_replay_failed", "n=%d %s" % (n, r.stdout))
                ok, out = fsck_clean(os.path.join(c, "fs.img"))
                if not ok:
                    return failed("driver_replay_not_clean", "n=%d %s" % (n, out))
                mine, err = walk(ex, c)
                if mine is None:
                    return failed("driver_replay_unreadable", "n=%d %s" % (n, err))

                r = run(["e2fsck", "-fy", os.path.join(theirs, "fs.img")], None)
                if r.returncode > 1:
                    return failed("e2fsck_replay_failed", "n=%d %s" % (n, r.stdout))
                ok, out = fsck_clean(os.path.join(theirs, "fs.img"))
                if not ok:
                    return failed("e2fsck_replay_not_clean", "n=%d %s" % (n, out))
                other, err = walk(ex, theirs)
                if other is None:
                    return failed("e2fsck_replay_unreadable", "n=%d %s" % (n, err))
                if mine != other:
                    return failed("replays_disagree", "n=%d" % n)
                if mine not in states:
                    if torn_write(gen, mine, os.path.join(c, "fs.img")):
                        result["torn"] += 1
                    else:
                        with open(os.path.join(work, "cut-tree.txt"), "w") as f:
                            f.write(mine)
                        return failed("not_a_state", "n=%d of %d" % (n, total))
                result["cuts"] += 1
        result["ok"] = True
        return result
    except subprocess.TimeoutExpired as e:
        return failed("timeout", str(e.cmd))
    finally:
        shutil.rmtree(work, ignore_errors=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("build")
    ap.add_argument("first", type=int)
    ap.add_argument("seeds", type=int)
    ap.add_argument("--ops", type=int, default=120)
    ap.add_argument("--cuts", type=int, default=0)
    ap.add_argument("-j", type=int, default=os.cpu_count() or 2)
    ap.add_argument("--keep", default="")
    a = ap.parse_args()

    for tool in ("mkfs.ext4", "e2fsck", "debugfs", "dumpe2fs"):
        if shutil.which(tool) is None:
            print("ext4_torture=skip reason=no_%s" % tool)
            return 0
    ex = os.path.join(os.path.abspath(a.build), "vibeos_ext4_exercise")
    if not os.access(ex, os.X_OK):
        print("ext4_torture=FAILED seed=- reason=exercise_not_built")
        return 1
    keep = os.path.abspath(a.keep) if a.keep else ""
    if keep:
        os.makedirs(keep, exist_ok=True)

    jobs = [(ex, s, a.ops, a.cuts, keep) for s in range(a.first, a.first + a.seeds)]
    ops = cuts = torn = 0
    bad = []
    with ProcessPoolExecutor(max_workers=max(1, a.j)) as pool:
        for res in pool.map(one_seed, jobs):
            ops += res["ops"]
            if os.environ.get("EXT4_TORTURE_VERBOSE"):
                print("  seed=%d ops=%d cuts=%d torn=%d" % (res["seed"], res["ops"], res["cuts"], res["torn"]))
            cuts += res["cuts"]
            torn += res["torn"]
            if not res["ok"]:
                bad.append(res)
                print("  seed=%d reason=%s %s" % (res["seed"], res["why"], res["detail"]), flush=True)
    if bad:
        print("ext4_torture=FAILED seed=%d reason=%s failing_seeds=%d of %d"
              % (bad[0]["seed"], bad[0]["why"], len(bad), a.seeds))
        return 1
    print("ext4_torture=ok seeds=%d ops=%d cuts=%d torn_writes=%d" % (a.seeds, ops, cuts, torn))
    return 0


if __name__ == "__main__":
    sys.exit(main())
