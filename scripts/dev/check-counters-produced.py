#!/usr/bin/env python3
"""Which counters does the kernel report that nothing ever increments?

A counter with no producer is an assertion that cannot go red. This project
shipped one and did not notice for months: `VIBEOS_BLK_TIMEOUT` was defined in
blkdev.h, named by blk_result_name, printed by kmain on the MUSTBEZERO line and
asserted by the boot gate - and no driver had ever returned it, because both
disk drivers collapsed a timeout into the same -1 the device's own error bit
produces. The mechanism was in place, the reporting was in place, and the two
were not connected to each other.

That is greppable, so it should be grepped.

## What counts as a producer

A `++`, a `+=`, or an assignment to the field somewhere other than a bulk reset
- an init function that zeroes every field is not a producer, and treating it
as one is how this check would bless the exact defect it exists for. Resets are
recognised by their value being a literal zero.

## What it cannot see

A field written only through a pointer or a memset, and an enum value passed as
a variable rather than named. Both are reported as unproduced, which is the
safe direction: a false positive costs a line in the list below, a false
negative costs months.

**It matches by field name across every watched struct, so two structs sharing
a name cover for each other.** `io_stats.cache_hits` reads as produced because
`mm_stats.cache_hits` exists, and they are different caches. Fixing that
properly needs to know which struct an expression belongs to, which needs a C
parser; naming the limitation is worth more than a tool that quietly gets it
wrong. Field names that appear in more than one watched struct are called out.
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# Structs whose fields are reported and asserted. Each entry is the header that
# declares it and the struct's name in that file.
WATCHED = [
    ("include/vibeos/mm_model.h", "vibeos_mm_stats"),
    ("include/vibeos/io_stats.h", "vibeos_io_stats"),
    ("include/vibeos/swapmap.h", "vibeos_swap_stats"),
    ("include/vibeos/swaparea.h", "vibeos_swaparea_stats"),
    ("include/vibeos/anon.h", "vibeos_anon_stats"),
    ("include/vibeos/reclaim.h", "vibeos_reclaim_stats"),
    ("include/vibeos/rmap.h", "vibeos_rmap_stats"),
    ("include/vibeos/blockdev.h", "vibeos_blockcache"),
    ("include/vibeos/exec_stats.h", "vibeos_exec_stats"),
]

SOURCE_DIRS = ["kernel", "include"]

# Counters that are declared and legitimately not produced, each with the
# reason. This list is the point of the check as much as the failures are: it
# is the difference between "nothing writes this" and "nothing writes this
# *yet*, and here is what will".
#
# Adding an entry is a decision. A counter parked here because it is
# inconvenient is the defect this check exists for, wearing a permission slip.
EXPECTED_UNPRODUCED = {
    # Declared ahead of the I/O phases that will produce them. docs/io/phases.md
    # names each one; they are here so the reporting exists before the
    # mechanism, which is the order this project's plans use deliberately.
    "vibeos_io_stats.max_wait_iterations": "I6, asynchronous I/O",
    "vibeos_io_stats.requests_in_flight_peak": "I6, asynchronous I/O",
    "vibeos_io_stats.write_back_pending": "I4, when the cache stops being write-through",
    "vibeos_io_stats.write_back_failed": "I4, when the cache stops being write-through",
    "vibeos_io_stats.table_writes_refused": "I4c, writing a partition table",
    "vibeos_io_stats.file_reads": "I5, when a filesystem other than FAT mounts",
    "vibeos_io_stats.file_bytes_read": "I5, when a filesystem other than FAT mounts",
    "vibeos_io_stats.file_writes": "I5, when a filesystem other than FAT mounts",
    "vibeos_io_stats.file_bytes_written": "I5, when a filesystem other than FAT mounts",
    "vibeos_io_stats.short_reads": "I5, when a filesystem other than FAT mounts",

    # The block cache reports its own counters on the [IO] BLKCACHE line rather
    # than through io_stats, so these three are a second home for the same
    # facts. Left declared rather than deleted because I4b brings a second
    # cache instance and a per-device tally will want them - but a number with
    # two homes is how two structures come to disagree, so whichever survives
    # I4b, the other goes.
    "vibeos_io_stats.cache_evictions": "reported on [IO] BLKCACHE instead; resolve at I4b",
    "vibeos_io_stats.cache_resident": "reported on [IO] BLKCACHE instead; resolve at I4b",

    # Needs a mechanism that does not exist: detecting a hit that returned the
    # wrong block means checksumming every cached block, and the cheap version
    # - comparing the key - is what the lookup already does. The mm page cache
    # had this defect for real, and what found it was the free-page poison, not
    # a counter. Kept declared so the name is reserved for whatever does find
    # it here.
    "vibeos_io_stats.cache_wrong_key": "no mechanism yet; see the page cache's history",

    # Written through the out-pointers of the auditor callback, which this
    # check cannot follow. vibeos_exec_audit_cache passes their addresses to
    # g_auditor.
    "vibeos_exec_stats.cache_audit_checked": "written through the auditor's out-pointers",
    "vibeos_exec_stats.cache_audit_changed": "written through the auditor's out-pointers",
}


def fields_of(header, struct):
    """Field names of the first struct in `header` whose typedef ends in
    `struct` (or `struct` + _t)."""
    path = os.path.join(ROOT, header)
    if not os.path.isfile(path):
        return []
    text = open(path, encoding="utf-8").read()
    m = re.search(r"typedef struct[^{]*\{(.*?)\}\s*" + re.escape(struct) + r"_?t?\s*;",
                  text, re.S)
    if not m:
        return []
    body = m.group(1)
    body = re.sub(r"/\*.*?\*/", " ", body, flags=re.S)
    out = []
    for line in body.split(";"):
        f = re.search(r"\b(uint\d+_t|uint32_t|uint64_t)\s+([a-z_][a-z0-9_]*)\s*$",
                      line.strip())
        if f:
            out.append(f.group(2))
    return out


def sources():
    for d in SOURCE_DIRS:
        for root, _dirs, files in os.walk(os.path.join(ROOT, d)):
            for name in files:
                if name.endswith((".c", ".h")):
                    yield os.path.join(root, name)


def main():
    text = "\n".join(open(p, encoding="utf-8", errors="replace").read()
                     for p in sources())
    unproduced = []
    total = 0

    for header, struct in WATCHED:
        for field in fields_of(header, struct):
            total += 1
            # A producer: ++, +=, or an assignment to something other than 0.
            # Both forms. The first version looked only for a postfix ++ and
            # reported `++bc->clock` as unproduced - a check that invents a
            # defect is the failure this project has spent the most time on.
            inc = (re.search(r"[->.]" + re.escape(field) + r"\s*(\+\+|\+=)", text)
                   or re.search(r"\+\+\s*[a-z_][a-z0-9_]*\s*(->|\.)\s*"
                                + re.escape(field) + r"\b", text)
                   # A counter written through an atomic helper rather than
                   # with ++. The block layer's counters became atomic when I7
                   # measured two requests inside it at once, and this check
                   # went red for the right reason: it could no longer see them
                   # being produced. Teaching it the new shape is the fix;
                   # loosening it to "the name appears somewhere" would not be,
                   # since the point is to tell a counter that is written from
                   # one that is only declared.
                   or re.search(r"BLK_COUNT\s*\([^,;]*[->.]"
                                + re.escape(field) + r"\b", text))
            if inc:
                continue
            assign = re.findall(r"[->.]" + re.escape(field) + r"\s*=\s*([^;=][^;]*);",
                                text)
            if any(not re.match(r"^\s*0(ull|u|l)?\s*$", a) for a in assign):
                continue
            unproduced.append(struct + "." + field)

    unexpected = [u for u in unproduced if u not in EXPECTED_UNPRODUCED]
    stale = [k for k in EXPECTED_UNPRODUCED if k not in unproduced]

    print(f"counters={total} unproduced={len(unproduced)} "
          f"expected={len(unproduced) - len(unexpected)} "
          f"unexpected={len(unexpected)}")
    for u in unexpected:
        print("  nothing ever increments: " + u)
    # An entry that is no longer needed is not harmless: it is a permission
    # slip left lying around for the next counter that goes quiet.
    for k in stale:
        print("  stale exemption, something produces this now: " + k)

    if unexpected or stale:
        print("counters-produced=FAIL")
        return 1
    print("counters-produced=ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
