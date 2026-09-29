#!/usr/bin/env python3
"""What the corpus needs, against what this kernel serves.

    python3 scripts/dev/corpus-report.py                 # write docs/abi/corpus.md
    python3 scripts/dev/corpus-report.py --check <dir>   # compare a fresh measurement

Reads tests/corpus/needs/*.txt (scripts/dev/corpus-measure.sh) and the state and
phase of every syscall from docs/abi/syscalls.md (scripts/dev/make-syscall-table.py),
and writes docs/abi/corpus.md: per workload, how many of the syscalls it asks for
are done, partial or missing; and across the corpus, which missing ones are asked
for and by whom, by phase - the order in which to write them.

--check compares a directory of freshly measured needs with the checked-in ones
and fails if any workload's set changed: a program's needs moved underneath the
plan (a new libc, a new busybox), and the report is no longer true. That is what
the nightly runs.

Exits non-zero if a needs list names a syscall the table does not know - which is
a parsing mistake here or a table out of date, and either would make every number
below a guess.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
NEEDS = os.path.join(ROOT, "tests", "corpus", "needs")
TABLE = os.path.join(ROOT, "docs", "abi", "syscalls.md")
OUT = os.path.join(ROOT, "docs", "abi", "corpus.md")
LTP = os.path.join(ROOT, "tests", "corpus", "ltp-built.txt")

PHASE_TITLES = {
    "L1": "files and paths", "L2": "processes, credentials, time", "L3": "memory",
    "L4": "event loops", "L5": "sockets", "L6": "threads and scheduling", "L7": "IPC",
    "L8": "system administration", "L9": "security", "D": "deferred",
    "R": "refused by decision",
}


NATIVE = set()   # VibeOS's own numbers (1000 and up): not Linux, not in the oracle


def load_table():
    table = {}
    for line in open(TABLE, encoding="utf-8"):
        m = re.match(r"\| (\d+) \| `(\w+)` \| (\w+) \| ([^|]+?) \|", line)
        if m:
            table[m.group(2)] = (m.group(3), m.group(4).strip())
            if int(m.group(1)) >= 1000:
                NATIVE.add(m.group(2))
    return table


def load_needs(d):
    needs = {}
    for f in sorted(os.listdir(d)):
        if f.endswith(".txt"):
            with open(os.path.join(d, f), encoding="utf-8") as fh:
                needs[f[:-4]] = sorted(set(l.strip() for l in fh if l.strip()))
    return needs


def load_files(d):
    files = {}
    for f in sorted(os.listdir(d)):
        if f.endswith(".files"):
            with open(os.path.join(d, f), encoding="utf-8") as fh:
                files[f[:-6]] = sorted(set(l.strip() for l in fh if l.strip()))
    return files


def load_ltp(table):
    """LTP test -> the syscall it tests, by the longest syscall name its binary
    name starts with: clone301 is clone3's, not clone's; fcntl34_64 is fcntl's."""
    tests = {}
    if not os.path.isfile(LTP):
        return tests
    names = sorted(table, key=len, reverse=True)
    for line in open(LTP, encoding="utf-8"):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        binary = line.split("/")[-1]
        for n in names:
            if binary.startswith(n):
                tests.setdefault(n, []).append(binary)
                break
        else:
            tests.setdefault(None, []).append(binary)
    return tests


def check(fresh_dir):
    old, new = load_needs(NEEDS), load_needs(fresh_dir)
    changed = 0
    for name in sorted(set(old) | set(new)):
        a, b = set(old.get(name, [])), set(new.get(name, []))
        if a != b:
            changed += 1
            print("corpus-check: %s: +%s -%s" % (name, " ".join(sorted(b - a)) or "()",
                                                 " ".join(sorted(a - b)) or "()"))
    print("corpus-check=%s changed=%d workloads=%d"
          % ("ok" if not changed else "FAIL", changed, len(new)))
    return 1 if changed else 0


def main():
    if len(sys.argv) > 2 and sys.argv[1] == "--check":
        return check(sys.argv[2])

    table = load_table()
    needs = load_needs(NEEDS)
    files = load_files(NEEDS)
    unknown = sorted({s for v in needs.values() for s in v if s not in table})
    if unknown:
        print("corpus-report: not in docs/abi/syscalls.md: " + " ".join(unknown))
        return 1

    def phase_of(name):
        state, ph = table[name]
        if state == "missing":
            return ph
        if state == "partial":
            return ph.split()[-1]
        return None

    rows = []
    wanted = {}   # missing or partial syscall -> workloads asking
    for w, calls in needs.items():
        done = [c for c in calls if table[c][0] == "done"]
        partial = [c for c in calls if table[c][0] == "partial"]
        missing = [c for c in calls if table[c][0] == "missing"]
        for c in partial + missing:
            wanted.setdefault(c, []).append(w)
        rows.append((w, len(calls), len(done), partial, missing))

    union = sorted({c for v in needs.values() for c in v})
    u_done = sum(1 for c in union if table[c][0] == "done")
    u_partial = [c for c in union if table[c][0] == "partial"]
    u_missing = [c for c in union if table[c][0] == "missing"]
    # Ready means nothing it asks for is missing. A partial row is not a blocker
    # by itself - ioctl's ENOTTY is the right answer when output is not a
    # terminal - so it is listed, not counted against. The musl programs the
    # boot gate already runs must come out ready; if they do not, this report is
    # wrong, not the kernel.
    ready = sum(1 for r in rows if not r[4])

    L = []
    L.append("# What the corpus needs")
    L.append("")
    L.append("Generated by `scripts/dev/corpus-report.py` from the syscalls each corpus")
    L.append("workload made on Linux (`tests/corpus/needs/`, measured by")
    L.append("`scripts/dev/corpus-measure.sh` under strace) and the state of each syscall in")
    L.append("[syscalls.md](syscalls.md). Phase A0 of [phases.md](phases.md). The workloads are")
    L.append("listed in `tests/corpus/workloads.txt`.")
    L.append("")
    L.append("A workload is *ready* when nothing it asks for is missing here. A *partial*")
    L.append("syscall does not count against it - it is served, with a named gap that may or")
    L.append("may not be the case this workload reaches. Ready is necessary, not sufficient:")
    L.append("running the workload on VibeOS, and LTP, are what say it works. The musl test")
    L.append("programs the boot gate already runs are the check on this report: they must")
    L.append("come out ready.")
    L.append("")
    L.append("| | |")
    L.append("| --- | --- |")
    L.append("| Workloads | %d, of which ready: %d |" % (len(rows), ready))
    L.append("| Distinct syscalls asked for | %d |" % len(union))
    L.append("| ... done | %d |" % u_done)
    L.append("| ... partial | %d: %s |" % (len(u_partial), ", ".join("`%s`" % c for c in u_partial)))
    L.append("| ... missing | %d |" % len(u_missing))
    L.append("")
    L.append("## Missing and partial, by phase - the order to write them in")
    L.append("")
    L.append("| Phase | Syscall | State | Asked for by |")
    L.append("| --- | --- | --- | --- |")
    for c in sorted(wanted, key=lambda c: (phase_of(c) or "Z", -len(wanted[c]), c)):
        ph = phase_of(c)
        L.append("| %s - %s | `%s` | %s | %d: %s |" % (
            ph, PHASE_TITLES.get(ph, ph), c, table[c][0], len(wanted[c]),
            ", ".join(sorted(wanted[c]))))
    L.append("")
    L.append("## Per workload")
    L.append("")
    L.append("| Workload | Asks for | Done | Partial | Missing |")
    L.append("| --- | --- | --- | --- | --- |")
    for w, n, d, p, m in rows:
        L.append("| `%s` | %d | %d | %s | %s |" % (
            w, n, d, " ".join("`%s`" % c for c in p) or "-",
            " ".join("`%s`" % c for c in m) or "-"))
    ltp = load_ltp(table)
    unmatched = sorted(ltp.pop(None, []))
    if ltp:
        total = sum(len(v) for v in ltp.values())
        L.append("")
        L.append("## The LTP oracle")
        L.append("")
        L.append("The Linux Test Project's syscall tests that build against musl")
        L.append("(`scripts/dev/corpus-build.sh`; the list is `tests/corpus/ltp-built.txt`): %d" % total)
        L.append("tests covering %d syscalls. Each is matched to the syscall its name starts" % len(ltp))
        L.append("with, longest name first. What they are for is the phase column: a syscall")
        L.append("with tests here has a conformance oracle waiting for it; one without has to")
        L.append("be proved by the corpus programs alone. Another %d built tests have a name" % len(unmatched))
        L.append("that starts with no syscall's (for example %s) and are not counted." % ", ".join("`%s`" % t for t in unmatched[:5]))
        L.append("")
        L.append("| Phase | Syscalls in the phase | with LTP tests | tests |")
        L.append("| --- | --- | --- | --- |")
        phases = {}
        for name, (state, ph) in table.items():
            if name in NATIVE:
                continue
            key = "done" if state == "done" else (ph.split()[-1] if state == "partial" else ph)
            phases.setdefault(key, []).append(name)
        for key in ["done", "L1", "L2", "L3", "L4", "L5", "L6", "L7", "L8", "L9", "D", "R"]:
            names = phases.get(key, [])
            covered = [n for n in names if n in ltp]
            L.append("| %s | %d | %d | %d |" % (
                key if key == "done" else "%s - %s" % (key, PHASE_TITLES[key]),
                len(names), len(covered), sum(len(ltp[n]) for n in covered)))

    all_files = sorted({f for v in files.values() for f in v})
    if all_files:
        L.append("")
        L.append("## System files the corpus opens")
        L.append("")
        L.append("Not syscalls, and just as necessary: `ps` needs nothing VibeOS lacks at the")
        L.append("syscall level and still fails without `/proc`. Process ids are folded to")
        L.append("`<pid>`. These are the requirements of L2's minimal `/proc`, and of the `/etc`")
        L.append("files the credential model will need.")
        L.append("")
        L.append("| Path | Opened by |")
        L.append("| --- | --- |")
        for f in all_files:
            by = sorted(w for w, v in files.items() if f in v)
            L.append("| `%s` | %s |" % (f, ", ".join(by)))
    with open(OUT, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(L) + "\n")
    print("corpus-report: workloads=%d ready=%d asked=%d done=%d partial=%d missing=%d"
          % (len(rows), ready, len(union), u_done, len(u_partial), len(u_missing)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
