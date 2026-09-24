#!/usr/bin/env python3
"""Does every module have the parts a module here is supposed to have?

`docs/core/architecture.md` lists seven, and they were written down because
nobody had written them down: eight registration seams exist in this tree, each
invented separately by somebody solving one problem, and several modules arrived
missing a piece. A page cache with no lock ran for months on the accident of
having exactly one caller, and adding a second caller returned one file's pages
under another file's key.

This script enforces the parts a script can judge, and says which those are
rather than implying it covers all seven.

## What is checked

1. **One header.** `include/vibeos/<module>.h` exists.
2. **State confined to one .c.** The module exports no global *data* symbol.
   This is measured with `nm` against the built objects, not with a regex: a
   lowercase symbol class is a `static`, an uppercase one is not, and the
   linker's opinion is the only one that matters here. A module whose state has
   a name another translation unit can write is not a module.
3. **Mutable state that names no lock at all.** Not "has no *registered* lock" -
   the first version of this check asked that, and called `blkdev.c` unlocked
   while it holds its own. What is measured is narrower and is the shape that
   actually cost months: a module with file-scope mutable state (lowercase `b`
   or `d` from `nm`) whose source does not mention a lock anywhere. `backing.c`
   was exactly that, and it was correct for months on the accident of having one
   caller - adding a second returned one file's pages under another file's key.

   Such a module is **not necessarily wrong**: it may be called under a lock its
   caller holds, which is a real design and several here use it. That is why
   this is ratcheted rather than forbidden. What the number says is how many
   modules would have to be reasoned about, one at a time, if somebody added a
   caller - and it is 4,000 lines of arch code away from where that decision
   gets made.
4. **A sabotage case file.** Three checks in this tree have shipped unable to
   fail. `scripts/dev/cases/` is where the argument that a check works lives.

## What is deliberately not checked, and by whom instead

- (now checked, see no_mustbezero below) A must-be-zero counter per module; C2 step 3 enumerated
  the modules that have none and adds them before the check exists. Writing the
  check first would mean shipping it red against thirty modules, which is the
  state `check-mm-layering.sh` sat in for a phase - "a check red since before
  you arrived is a check nobody reads".
- **An init that names its reason**, and **whether the registration point is
  the right one**. Both are review, and this file says so instead of pretending.

## Ratcheted

Today's violations are the baseline, per property, and may only go down. A
baseline is raised as a decision, in the change that earns it - never as a
reflex.

Usage: check-subsystem.py [--list] [<build-dir>]
"""

import os
import subprocess
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# kernel/arch/ is excluded on purpose: arch_hw.c is the *subject* of this
# refactor, not a module that has failed to become one. Measuring it here would
# add one enormous violation that says nothing the plan does not already say.
AREAS = ("abi", "core", "diag", "exec", "fs", "io", "ipc", "mm", "net", "object",
         "proc", "sched", "time", "txn")

# Today's measurement. Each number is a debt, not a permission.
#
# exported_state is **already zero** across all 47 modules, which is the one
# part of the contract this tree got right without being told to - and it is
# worth stating rather than assuming, because it is the property the other three
# rest on: no module's state has a name another file can write.
BASELINE = {
    "no_header": 15,
    "exported_state": 0,
    "state_without_lock": 2,   # was 3: mm/anon. Its hand and counters are atomic now (M-056); what this textual check saw is the comment saying why it takes no lock
    "no_case": 32,   # was 33; mm/anon gained mm-anon-claims.txt (M-056). Before: 35, then ext2 with M-049
    # C2 step 3. Modules with no must-be-zero, no counter asserted elsewhere and
    # no written exemption. Was 36 of 50 when the property was added.
    "no_mustbezero": 0,
    # Modules exempted by reason (EXEMPT below). It may only go down: an
    # exemption is a claim that the module has no failure mode a counter could
    # see, and every one is a place a defect could hide without a detector.
    "mustbezero_exempt": 24,   # was 21; +1 sched/task_ident, +1 fs/fdtable (C5): pure functions over a caller-owned struct, decided not reflexed; +1 diag/crash (C6), see its entry
}

# A module counts as having a must-be-zero when one of these holds. Three routes,
# because the counters that exist grew three ways and pretending otherwise would
# leave the check red for the wrong reason:
#
#  1. It says so: its source or header carries "must be zero"/MUSTBEZERO, or it
#     reports through the registry (vibeos_mbz_hit, kernel/core/mbz.c).
#  2. Its counter lives in a shared stats struct (SHARED_STATS).
#  3. It is asserted by the boot gate under a name other than a MUSTBEZERO line
#     (ELSEWHERE). The gate is grepped for that name, so an entry whose
#     assertion has been deleted stops counting instead of staying green.
SHARED_STATS = {
    "mm/frame": "mm_stats.h",       # frames_leaked, frames_double_put
    "sched/task": "task_stats.h",    # procstate_double_put
}
ELSEWHERE = {
    "fs/blockcache": "blockcache_evict_failed",   # a block the caller was told was written
    "mm/swaparea": "out_of_range",                # a slot outside the swap area
    "sched/lifetime": "use_after_publish",        # teardown steps out of order
    "abi/abi": "abi_unimplemented_syscall_nr",      # a number that classifies to NONE
    "abi/abi_linux": "abi_unimplemented_syscall_nr",
    "mm/backing": "cache_audit_changed",          # a cached page that no longer matches its file
}

# The rest, each with the reason. Read these as a list of where a bug would have
# to hide to go unseen; the reason says why it cannot hide in a counter. Not a
# permission: an entry comes out the day the module gains a real failure mode.
EXEMPT = {
    "ipc/channel": "a full channel is backpressure the caller decides on; the ring indices are covered by host ring tests",
    "ipc/event": "one flag with no failure mode",
    "ipc/handle_transfer": "every refusal is the rights-subset check working; a wrong grant is a logic defect in one function the host tests pin",
    "ipc/waitset": "timeouts and wakes are outcomes, not faults; ownership refusals are policy",
    "core/log": "the ring overwrites by design and counts what it dropped",
    "core/policy": "a pure decision over its arguments; a denial is the behaviour",
    "core/security": "a pure decision over its arguments; a denial is the behaviour",
    "net/net_policy": "a pure decision over its arguments; a denial is the behaviour",
    "sched/sched_policy": "a pure decision over its arguments; no state",
    "exec/stats": "the counters themselves; what exec refuses is reported by exec, and a missing file is a normal outcome",
    "fs/storage": "a table of volumes; unmounted and not-found are results the caller handles",
    "fs/vfs": "a dispatch table; a refusal is the underlying filesystem's, counted there",
    "mm/pmm": "boot-time region arithmetic, saturating; a wrong region shows as frames_leaked or poison_hits, both gated",
    "mm/reclaim": "pressure events are load, not defect; freeing a live frame shows as poison_hits and frames_double_put, gated",
    "mm/usage": "read-only reporting with no state",
    "mm/vma": "a refused overlap is the caller's answer; a corrupted map shows in the ring-3 mmap/mprotect/munmap self-test, asserted",
    "object/handle_table": "allocation failure is exhaustion, a limit and not a defect",
    "proc/process": "state transitions are validated by the task-state table, whose illegal_transition is gated",
    "sched/forkguard": "refusing a fork is the guard's purpose",
    "fs/fdtable": "pure functions over a table the caller owns; an exhausted table is -1 and a limit, not a defect - the layout rules are proved by the host test and fs-fdtable.txt",
    "sched/task_ident": "pure functions over a struct the caller owns; no state and no refusal - the reset is proved byte-for-byte by the host test",
    "sched/runq": "a round-robin pick proven against a model by the scheduler torture harness; no state to be wrong",
    "diag/crash": "a ring of four under its own lock, copied whole in and out; overwriting the oldest is the design, and the capture itself is the arch's, proved on every boot by svc-crash and crash-recorder.txt",
    "time/timer": "tick arithmetic and one armed deadline; no state a defect could corrupt beyond that deadline",
}
MUSTBEZERO_RE = re.compile(r"must[ -]?be[ -]?zero|MUSTBEZERO|vibeos_mbz_hit", re.I)


def gate_text():
    try:
        return open(os.path.join(ROOT, "scripts", "qemu-cli-smoke-linux.py"),
                    encoding="utf-8", errors="replace").read()
    except OSError:
        return ""


def mustbezero_status(area, name, src, gate):
    """'yes', 'exempt' or 'no'."""
    key = "%s/%s" % (area, name)
    if MUSTBEZERO_RE.search(src):
        return "yes"
    paths = [os.path.join(ROOT, "include", "vibeos", name + ".h")]
    if key in SHARED_STATS:
        paths.append(os.path.join(ROOT, "include", "vibeos", SHARED_STATS[key]))
    for p in paths:
        try:
            if MUSTBEZERO_RE.search(open(p, encoding="utf-8", errors="replace").read()):
                return "yes"
        except OSError:
            pass
    if key in ELSEWHERE and ELSEWHERE[key] in gate:
        return "yes"
    if key in EXEMPT and len(EXEMPT[key]) >= 20:
        return "exempt"
    return "no"


def modules():
    out = []
    for a in AREAS:
        d = os.path.join(ROOT, "kernel", a)
        if not os.path.isdir(d):
            continue
        for n in sorted(os.listdir(d)):
            if n.endswith(".c"):
                out.append((a, n[:-2], os.path.join(d, n)))
    return out


def objects(build):
    """module source path -> object file, from the kernel core object tree."""
    found = {}
    base = os.path.join(ROOT, build, "CMakeFiles", "vibeos_kernel_core.dir")
    if not os.path.isdir(base):
        return found
    for cur, _, names in os.walk(base):
        for n in names:
            if n.endswith(".c.o"):
                rel = os.path.relpath(os.path.join(cur, n), base)
                found[rel.replace("\\", "/")] = os.path.join(cur, n)
    return found


def data_symbols(obj):
    """(exported, mutable_static) data symbols.

    Uppercase B/D/G is a linker-visible definition; lowercase b/d is a
    file-scope `static` that is written. Lowercase `r` is rodata and is not
    state, so it is left out of both.
    """
    try:
        out = subprocess.run(["nm", "--defined-only", obj],
                             capture_output=True, text=True,
                             check=False).stdout
    except OSError:
        return None, None
    exported, mutable = [], []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) < 3:
            continue
        if parts[1] in ("B", "D", "G"):
            exported.append(parts[2])
        elif parts[1] in ("b", "d"):
            mutable.append(parts[2])
    return exported, mutable


def has_case(cases, area, name):
    for c in cases:
        stem = c[:-4] if c.endswith(".txt") else c
        if stem == name or stem == "%s-%s" % (area, name):
            return True
        if stem.startswith("%s-" % area) and name in stem:
            return True
    return False


def main():
    listing = "--list" in sys.argv
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    build = args[0] if args else "build-clang-Release"

    objs = objects(build)
    if not objs:
        print("subsystem=skip reason=no_objects_in_%s" % build)
        return 0

    cases = set(os.listdir(os.path.join(ROOT, "scripts", "dev", "cases")))
    gate = gate_text()

    counts = dict((k, 0) for k in BASELINE)
    detail = []

    for area, name, path in modules():
        rel = "kernel/%s/%s.c" % (area, name)
        problems = []

        hdr = os.path.join(ROOT, "include", "vibeos", name + ".h")
        if not os.path.exists(hdr):
            problems.append("no_header")

        obj = objs.get(rel + ".o")
        syms, mutable = data_symbols(obj) if obj else (None, None)
        if syms:
            problems.append("exported_state")

        src = open(path, encoding="utf-8", errors="replace").read()
        if mutable and "lock" not in src:
            problems.append("state_without_lock")

        if not has_case(cases, area, name):
            problems.append("no_case")

        mz = mustbezero_status(area, name, src, gate)
        if mz == "no":
            problems.append("no_mustbezero")
        elif mz == "exempt":
            problems.append("mustbezero_exempt")

        for p in problems:
            counts[p] += 1
        if problems:
            detail.append((rel, problems, syms or []))

    if listing:
        for rel, problems, syms in detail:
            extra = (" [%s]" % " ".join(syms[:4])) if syms else ""
            print("  %-34s %s%s" % (rel, ",".join(problems), extra))

    bad = [(k, counts[k], BASELINE[k]) for k in sorted(counts)
           if counts[k] > BASELINE[k]]
    if bad:
        for k, got, want in bad:
            print("  %s: %d modules, baseline %d" % (k, got, want))
        # Advice first, verdict last: check.sh reads this with `| tail -1`.
        print("      Raise a baseline in this file only as a decision, in the "
              "same commit that earns it.")
        print("subsystem=FAIL " +
              " ".join("%s=%d/%d" % (k, counts[k], BASELINE[k])
                       for k in sorted(counts)))
        return 1
    print("subsystem=ok modules=%d " % len(modules()) +
          " ".join("%s=%d" % (k, counts[k]) for k in sorted(counts)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
