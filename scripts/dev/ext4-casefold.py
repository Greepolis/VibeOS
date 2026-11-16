#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""ext4's casefolded directories through the driver (E4-030).

    scripts/dev/ext4-casefold.py <build-dir> [seeds] [ucd-12.1-dir]

Linux cannot be the oracle here as it is for the torture: a casefolded
directory needs a casefold volume mounted, which takes root. So the oracle is
two things the driver does not share code with:

  - a model in this script, which folds names with Python's own Unicode (NFD
    of the full case folding, on characters Unicode 12.1 assigns) and keeps,
    per directory, the name each file was made under;
  - e2fsck, whose casefold rules are e2fsprogs' own tables: a volume the
    driver left must be clean, and a name that collides by case with another
    is a duplicate entry to it.

Per seed: a casefold volume with /ci casefolded (set with debugfs, as chattr
+F would) and /cs not; random creates, mkdirs, unlinks, renames and reads under
names that differ by case, by precomposed against decomposed form, by ß
against SS, by final sigma, by Hangul syllable against its jamo, and by bytes
that are not UTF-8; each line carries the answer the model expects, and the
listing after must be the model's. Then e2fsck -fyD indexes every directory
with its own hash of the folded names, and the driver must find every file
again by another spelling - through the index (dx_lookups).

Prints ext4_casefold=ok seeds=<n> ops=<n> dx=<n>, or =FAILED with the reason.
"""
import os
import random
import subprocess
import sys
import tempfile
import unicodedata

WORDS = [
    "file", "FILE", "File", "fIlE",
    "straße", "STRASSE", "strasse", "Straße",
    "Ångström", "Ångström", "ångström", "ÅNGSTRÖM",
    "Σίσυφος", "ΣΊΣΥΦΟΣ", "σίσυφοσ",
    "ǅemal", "ǄEMAL", "ǆemal",
    "한글", "한글",
    "ﬁle", "FILE2", "file2",
    "ẹ́", "ẹ́", "Ẹ́",
]
RAW = [b"\xffodd", b"\xffODD", b"caf\xe9"]


def load_assigned(ucd):
    out, first = set(), None
    for line in open(os.path.join(ucd, "UnicodeData.txt"), encoding="utf-8"):
        f = line.split(";")
        cp, name = int(f[0], 16), f[1]
        if name.endswith(", First>"):
            first = cp
            continue
        lo = first if name.endswith(", Last>") else cp
        first = None
        if cp - lo < 0x30000:
            out.update(range(lo, cp + 1))
    return out


class Model:
    def __init__(self, assigned):
        self.assigned = assigned
        self.dirs = {b"/ci": (True, {}), b"/cs": (False, {})}

    def key(self, cf, name):
        if cf:
            try:
                s = name.decode("utf-8")
            except UnicodeDecodeError:
                return ("raw", name)
            if all(ord(c) in self.assigned for c in s):
                return ("fold", unicodedata.normalize("NFD", s.casefold()))
        return ("raw", name)

    def find(self, d, name):
        cf, ents = self.dirs[d]
        return ents.get(self.key(cf, name))


def run(cmd, cwd=None, inp=None):
    return subprocess.run(cmd, cwd=cwd, input=inp, capture_output=True)


def one_seed(ex, assigned, seed, work):
    rng = random.Random(seed)
    wd = os.path.join(work, "s%d" % seed)
    os.makedirs(wd)
    img = os.path.join(wd, "fs.img")
    r = run(["mkfs.ext4", "-q", "-F", "-O", "casefold", "-b", "1024", "-N", "4096", img, "32M"])
    if r.returncode:
        return "mkfs_failed", 0, 0
    run(["debugfs", "-w", "-R", "mkdir ci", img])
    run(["debugfs", "-w", "-R", "mkdir cs", img])
    st = run(["debugfs", "-R", "stat ci", img]).stdout.decode()
    flags = int(st.split("Flags: ")[1].split()[0], 16)
    run(["debugfs", "-w", "-R", "set_inode_field ci flags 0x%x" % (flags | 0x40000000), img])
    m = Model(assigned)
    pool = [w.encode() for w in WORDS] + RAW
    lines = []

    def emit(words, ok=True, err=None):
        if not ok:
            words = ([b"refuse"] if words[0] in (b"unlink", b"mkdir") else [b"fail", err.encode()]) + words
        lines.append(b"\t".join(words))

    # A big directory each, so that e2fsck -D gives them an index.
    for i in range(160):
        for top in (b"/ci", b"/cs"):
            n = ("Bulk-%03d" % i).encode()
            emit([b"create", top + b"/" + n, b"644"])
            cf, ents = m.dirs[top]
            ents[m.key(cf, n)] = (n, "f")
    ops = 0
    for _ in range(400):
        ops += 1
        d = rng.choice(sorted(m.dirs))
        cf, ents = m.dirs[d]
        a = rng.choice(pool)
        op = rng.choice(["create", "create", "unlink", "rename", "read", "read", "mkdir"])
        hit = m.find(d, a)
        if op == "create":
            if hit:
                emit([b"create", d + b"/" + a, b"644"], False, "EEXIST")
            else:
                emit([b"create", d + b"/" + a, b"644"])
                ents[m.key(cf, a)] = (a, "f")
        elif op == "mkdir":
            if hit or d.count(b"/") > 1:
                if hit:
                    emit([b"mkdir", d + b"/" + a], False)
                continue
            emit([b"mkdir", d + b"/" + a])
            ents[m.key(cf, a)] = (a, "d")
            m.dirs[d + b"/" + a] = (cf, {})   # a directory made in a casefolded one is casefolded
        elif op == "unlink":
            if not hit or hit[1] != "f":
                emit([b"unlink", d + b"/" + a], False)
            else:
                emit([b"unlink", d + b"/" + a])
                del ents[m.key(cf, a)]
        elif op == "read":
            if hit and hit[1] == "f":
                emit([b"read", d + b"/" + a])
            elif not hit:
                emit([b"read", d + b"/" + a], False, "ENOENT")
        elif op == "rename":
            b = rng.choice(pool)
            hb = m.find(d, b)
            if not hit or hit[1] != "f" or (hb and hb[1] != "f"):
                continue
            emit([b"rename", d + b"/" + a, d + b"/" + b])
            ka, kb = m.key(cf, a), m.key(cf, b)
            if ka == kb:
                continue   # the same entry: nothing happens
            del ents[ka]
            # A replaced entry keeps the name it had; a new one takes b.
            ents[kb] = (hb[0], "f") if hb else (b, "f")
    open(os.path.join(wd, "script.txt"), "wb").write(b"\n".join(lines) + b"\n")
    r = run([ex, "apply"], cwd=wd)
    if r.returncode:
        return "driver_apply_failed " + r.stderr.decode(errors="replace").strip().split("\n")[-1], ops, 0

    def want_listing():
        rows = []
        for dp in sorted(m.dirs):
            for _, (n, k) in m.dirs[dp][1].items():
                rows.append(dp + b"/" + n + (b"\tD" if k == "d" else b"\tF"))
        rows += [b"/ci\tD", b"/cs\tD"]
        return sorted(rows)

    def got_listing():
        r = run([ex, "walk"], cwd=wd)
        rows = []
        for l in r.stdout.split(b"\n"):
            w = l.split(b"\t")
            if len(w) >= 2 and w[1] in (b"F", b"D"):
                rows.append(w[0] + b"\t" + w[1])
        return sorted(rows)

    if got_listing() != want_listing():
        g, w = set(got_listing()), set(want_listing())
        return "listing_differs got-only=%r want-only=%r" % (sorted(g - w)[:3], sorted(w - g)[:3]), ops, 0
    r = run(["e2fsck", "-fn", img])
    if r.returncode:
        return "volume_not_clean " + r.stdout.decode(errors="replace").replace("\n", " ")[:300], ops, 0
    # e2fsck's own index of the folded names, and every file found again by
    # another spelling through it. e2fsprogs 1.47.0 cannot index a casefolded
    # directory holding a name that does not fold ("Failed to optimize
    # directory ... Unknown code ____ 220"), so the driver removes those
    # first - they were held to the model above, unindexed.
    lines = []
    for dp in sorted(m.dirs):
        if dp not in m.dirs:
            continue   # a raw-named directory removed with its parent's entry
        cf, ents = m.dirs[dp]
        for k in [k for k in ents if cf and k[0] == "raw"]:
            n, kind = ents[k]
            if kind == "d":
                sub = dp + b"/" + n
                for _, (sn, _sk) in m.dirs[sub][1].items():
                    lines.append(b"unlink\t" + sub + b"/" + sn)
                lines.append(b"rmdir\t" + sub)
                del m.dirs[sub]
            else:
                lines.append(b"unlink\t" + dp + b"/" + n)
            del ents[k]
    if lines:
        open(os.path.join(wd, "script.txt"), "wb").write(b"\n".join(lines) + b"\n")
        r = run([ex, "apply"], cwd=wd)
        if r.returncode:
            return "raw_unlink_failed", ops, 0
    r = run(["e2fsck", "-fyD", img])
    if b"Failed to optimize" in r.stdout:
        return "e2fsck_could_not_index " + r.stdout.decode(errors="replace").replace("\n", " ")[:200], ops, 0
    if r.returncode not in (0, 1):
        return "e2fsck_D_failed", ops, 0
    st = run(["debugfs", "-R", "htree ci", img]).stdout
    if b"Root node" not in st and b"Root node dump" not in st:
        return "ci_not_indexed", ops, 0
    swap = {"file": "FiLe", "bulk": "BULK"}
    lines = []
    for dp in sorted(m.dirs):
        cf, ents = m.dirs[dp]
        for _, (n, k) in ents.items():
            if k != "f":
                continue
            alt = n
            if cf:
                try:
                    alt = n.decode().swapcase().encode()
                except UnicodeDecodeError:
                    pass
                if m.key(cf, alt) != m.key(cf, n):
                    alt = n   # a swap that is not the same name (ß -> SS is; some are not)
            lines.append(b"read\t" + dp + b"/" + alt)
    for i in range(20):
        lines.append(b"fail\tENOENT\tread\t/ci/bulk-%03dx" % i)
    open(os.path.join(wd, "script.txt"), "wb").write(b"\n".join(lines) + b"\n")
    r = run([ex, "apply"], cwd=wd)
    if r.returncode:
        return "lookup_after_index_failed " + r.stderr.decode(errors="replace").strip().split("\n")[-1], ops, 0
    dx = 0
    for l in r.stderr.decode(errors="replace").split("\n"):
        if l.startswith("dx_lookups="):
            dx = int(l.split("=")[1])
    if dx == 0:
        return "index_not_used", ops, 0
    r = run(["e2fsck", "-fn", img])
    if r.returncode:
        return "volume_not_clean_after_index", ops, dx
    return None, ops, dx


def main():
    build = sys.argv[1] if len(sys.argv) > 1 else "build-gcc-Release"
    seeds = int(sys.argv[2]) if len(sys.argv) > 2 else 6
    ucd = sys.argv[3] if len(sys.argv) > 3 else os.path.expanduser("~/vibeos-scratch/ucd-12.1")
    ex = os.path.abspath(os.path.join(build, "vibeos_ext4_exercise"))
    if not os.path.exists(os.path.join(ucd, "UnicodeData.txt")):
        print("ext4_casefold=skip reason=no_ucd_12_1")
        return 0
    assigned = load_assigned(ucd)
    total_ops = total_dx = 0
    with tempfile.TemporaryDirectory(prefix="ext4-casefold-") as work:
        for seed in range(1, seeds + 1):
            why, ops, dx = one_seed(ex, assigned, seed, work)
            total_ops += ops
            total_dx += dx
            if why:
                print("ext4_casefold=FAILED seed=%d reason=%s" % (seed, why))
                return 1
    print("ext4_casefold=ok seeds=%d ops=%d dx=%d" % (seeds, total_ops, total_dx))
    return 0


if __name__ == "__main__":
    sys.exit(main())
