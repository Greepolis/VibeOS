#!/usr/bin/env python3
"""Is a FAT16/FAT32 volume consistent with itself?

    scripts/dev/fat-fsck.py <image>
    scripts/dev/fat-fsck.py --dirty <image>

What mtools cannot say. mdir lists names and mcopy reads files, and both follow
whatever the directory entries and the table say - so a cluster nobody owns, a
chain longer than its file, a ".." that names the wrong directory or two files
under one 8.3 name read back perfectly well. Those are the defects a writer
makes, and verify-fat-mtools.sh runs this after mtools for them.

It shares no code with the kernel's driver (kernel/fs/fat.c): a different
language, and the checks are written from the format's rules rather than from
what the driver does. What it reports:

  lost_clusters        allocated in the table, reached from no file or directory
  cross_linked         a cluster reached twice
  chain_too_long       a file's chain has more clusters than its size needs
  chain_too_short      ... or fewer, or ends in a free or bad cluster
  size_without_chain   a non-empty file with no first cluster, or the reverse
  bad_dotdot           a directory whose ".." is not its parent, or "." not itself
  orphan_long_name     long-name entries that belong to no short entry
  duplicate_short_name two live entries in one directory with one 8.3 name
  fat_copies_differ    the two copies of the table are not the same
  bad_entry            an entry whose first cluster is outside the volume

Exit status 0 and "fat-fsck=ok ..." when there is nothing to report.

--dirty changes the volume instead of checking it: in every directory, the
entries after the end-of-directory marker are filled with the letter A. FAT
says nothing about what follows the marker - a reader stops there - and every
tool at hand happens to leave zeros, so a writer that puts a name where the
marker was and forgets to write a new one is never caught: the zeros are a
marker by accident. After --dirty the volume is as valid as before and that
writer leaves a directory full of files called AAAAAAAA.AAA.
"""

import struct
import sys


def main():
    dirty = len(sys.argv) == 3 and sys.argv[1] == "--dirty"
    if len(sys.argv) != 2 and not dirty:
        print(__doc__)
        return 2
    path = sys.argv[-1]
    img = open(path, "rb").read()
    out = bytearray(img) if dirty else None
    spans = []   # (offset, length) of each directory's bytes, for --dirty
    bps, spc, reserved, nfats, root_entries, total16 = struct.unpack_from("<HBHBHH", img, 11)
    spf16 = struct.unpack_from("<H", img, 22)[0]
    total32 = struct.unpack_from("<I", img, 32)[0]
    total = total16 or total32
    if bps != 512 or spc == 0 or nfats == 0:
        print("fat-fsck=FAIL reason=not_a_fat_volume")
        return 1
    fat32 = spf16 == 0
    spf = struct.unpack_from("<I", img, 36)[0] if fat32 else spf16
    root_sectors = (root_entries * 32 + 511) // 512
    fat_lba = reserved
    root_lba = fat_lba + nfats * spf
    data_lba = root_lba + root_sectors
    clusters = (total - data_lba) // spc
    root_cluster = struct.unpack_from("<I", img, 44)[0] if fat32 else 0
    eoc = 0x0FFFFFF8 if fat32 else 0xFFF8

    def entry(cl, copy=0):
        off = (fat_lba + copy * spf) * 512
        if fat32:
            return struct.unpack_from("<I", img, off + cl * 4)[0] & 0x0FFFFFFF
        return struct.unpack_from("<H", img, off + cl * 2)[0]

    def cluster_bytes(cl):
        off = (data_lba + (cl - 2) * spc) * 512
        return img[off:off + spc * 512]

    problems = []
    owner = {}

    def chain(first, who):
        out, cl, seen = [], first, 0
        while 2 <= cl < eoc:
            if cl - 2 >= clusters:
                problems.append("bad_entry %s: cluster %d is outside the volume" % (who, cl))
                break
            if cl in owner:
                problems.append("cross_linked cluster %d: %s and %s" % (cl, owner[cl], who))
                break
            owner[cl] = who
            out.append(cl)
            seen += 1
            if seen > clusters:
                problems.append("chain_too_long %s: the chain loops" % who)
                break
            nxt = entry(cl)
            if nxt == 0:
                problems.append("chain_too_short %s: cluster %d links to a free cluster" % (who, cl))
                break
            cl = nxt
        return out

    def short_sum(name11):
        s = 0
        for b in name11:
            s = (((s & 1) << 7) + (s >> 1) + b) & 0xFF
        return s

    def mark(offsets, data):
        # After the first 0x00 entry, everything in this directory: `offsets`
        # maps each 32-byte entry of `data` to where it is in the image.
        for i in range(0, len(data), 32):
            if data[i] == 0x00:
                for j in range(i + 32, len(data), 32):
                    spans.append(offsets[j // 32])
                break

    def walk(data, where, self_cluster, parent_cluster, offsets=None):
        shorts = {}
        if dirty and offsets:
            mark(offsets, data)
        run = None           # (count, checksum) of a long-name run being read
        subdirs = []
        for i in range(0, len(data), 32):
            d = data[i:i + 32]
            if d[0] == 0x00:
                if run:
                    problems.append("orphan_long_name in %s before the end of the directory" % where)
                break
            if d[0] == 0xE5:
                if run:
                    problems.append("orphan_long_name in %s before a deleted entry" % where)
                run = None
                continue
            attr = d[11]
            if attr & 0x3F == 0x0F:
                seq = d[0] & 0x1F
                if d[0] & 0x40:
                    if run:
                        problems.append("orphan_long_name in %s: a run cut off by another" % where)
                    run = [seq, d[13]]
                elif not run or seq != run[0] - 1 or d[13] != run[1]:
                    problems.append("orphan_long_name in %s: a piece out of sequence" % where)
                    run = None
                else:
                    run[0] = seq
                continue
            if attr & 0x08:
                run = None
                continue
            name = bytes(d[0:11])
            label = "%s/%s" % (where, name.decode("latin-1").strip())
            if run:
                if run[0] != 1 or run[1] != short_sum(name):
                    problems.append("orphan_long_name before %s: it is not this entry's" % label)
                run = None
            first = (struct.unpack_from("<H", d, 20)[0] << 16) | struct.unpack_from("<H", d, 26)[0]
            size = struct.unpack_from("<I", d, 28)[0]
            if name[:2] == b". " or name[:3] == b".. ":
                want = self_cluster if name[1:2] == b" " else parent_cluster
                if first != want:
                    problems.append("bad_dotdot %s: cluster %d, should be %d" % (label, first, want))
                continue
            if name in shorts:
                problems.append("duplicate_short_name %s" % label)
            shorts[name] = 1
            if attr & 0x10:
                if first < 2:
                    problems.append("bad_entry %s: a directory with no cluster" % label)
                    continue
                subdirs.append((label, first))
            else:
                got = chain(first, label) if first >= 2 else []
                need = (size + spc * 512 - 1) // (spc * 512)
                if first < 2 and size:
                    problems.append("size_without_chain %s: %d bytes and no cluster" % (label, size))
                elif len(got) > need:
                    problems.append("chain_too_long %s: %d clusters for %d bytes" % (label, len(got), size))
                elif len(got) < need:
                    problems.append("chain_too_short %s: %d clusters for %d bytes" % (label, len(got), size))
        for label, first in subdirs:
            cls = chain(first, label + "/")
            walk(b"".join(cluster_bytes(c) for c in cls), label, first, self_cluster,
                 cluster_offsets(cls))

    def cluster_offsets(cls):
        return [(data_lba + (c - 2) * spc) * 512 + k
                for c in cls for k in range(0, spc * 512, 32)]

    if fat32:
        cls = chain(root_cluster, "/")
        walk(b"".join(cluster_bytes(c) for c in cls), "", 0, 0, cluster_offsets(cls))
    else:
        walk(img[root_lba * 512:(root_lba + root_sectors) * 512], "", 0, 0,
             [root_lba * 512 + k for k in range(0, root_sectors * 512, 32)])

    if dirty:
        for off in spans:
            out[off:off + 32] = b"A" * 32
        open(path, "wb").write(out)
        print("fat-fsck=dirtied entries=%d" % len(spans))
        return 0

    lost = [cl for cl in range(2, clusters + 2) if entry(cl) != 0 and cl not in owner
            and entry(cl) != (0x0FFFFFF7 if fat32 else 0xFFF7)]
    if lost:
        problems.append("lost_clusters %d, the first %d" % (len(lost), lost[0]))
    for copy in range(1, nfats):
        a = img[fat_lba * 512:(fat_lba + spf) * 512]
        b = img[(fat_lba + copy * spf) * 512:(fat_lba + (copy + 1) * spf) * 512]
        if a != b:
            problems.append("fat_copies_differ: copy %d" % copy)

    if problems:
        for p in problems[:8]:
            print("  " + p)
        print("fat-fsck=FAIL problems=%d first=%s" % (len(problems), problems[0].split()[0]))
        return 1
    print("fat-fsck=ok clusters=%d used=%d" % (clusters, len(owner)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
