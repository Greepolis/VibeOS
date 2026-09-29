#!/usr/bin/env python3
"""Write docs/abi/syscalls.md from the registry.

    python3 scripts/dev/make-syscall-table.py            # write it
    python3 scripts/dev/make-syscall-table.py --check    # fail if it is stale

Every line comes from kernel/abi/linux_syscalls.def, the one statement of every
Linux x86-64 number and the answer this kernel gives it (docs/abi/, phase A1);
the "Row" column names the file of the row that serves it. Until A1 this script
held the phases itself and read the numbers from the host's asm/unistd_64.h - a
second statement of the plan, which is what A1 exists to remove. check.sh runs
--check, so the published table cannot drift from what the kernel answers.
"""
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
REGISTRY = os.path.join(ROOT, "kernel", "abi", "linux_syscalls.def")
OUT = os.path.join(ROOT, "docs", "abi", "syscalls.md")

TITLES = {
    "L1": "files and paths", "L2": "processes, credentials, time", "L3": "memory",
    "L4": "event loops", "L5": "sockets", "L6": "threads and scheduling", "L7": "IPC",
    "L8": "system administration", "L9": "security", "D": "deferred", "R": "refused",
}
LINE = re.compile(r'^SYSCALL\(\s*(\d+)\s*,\s*(\w+)\s*,\s*(\w+)\s*,\s*(\w+)\s*,\s*(\w+)\s*,\s*"((?:[^"\\]|\\.)*)"\)',
                  re.M)


def main():
    text = open(REGISTRY, encoding="utf-8").read()
    entries = [(int(m.group(1)), m.group(2), m.group(3).lower(), m.group(4), m.group(5),
                m.group(6).replace('\\"', '"')) for m in LINE.finditer(text)]
    if len(entries) != len(re.findall(r"^SYSCALL\(", text, re.M)):
        print("make-syscall-table: a registry line does not parse")
        return 1

    rows = {}
    for f in glob.glob(os.path.join(ROOT, "kernel", "abi", "linux", "*.c")):
        for m in re.finditer(r"^\s*X\((\d+),\s*(\w+),", open(f, encoding="utf-8").read(), re.M):
            rows[int(m.group(1))] = os.path.basename(f)

    linux = [e for e in entries if e[0] < 1000]
    count = {}
    by_phase = {}
    for nr, name, state, phase, err, why in linux:
        count[state] = count.get(state, 0) + 1
        if state in ("missing", "deferred", "refused"):
            by_phase[phase] = by_phase.get(phase, 0) + 1

    L = []
    L.append("# Every Linux x86-64 syscall")
    L.append("")
    L.append("All %d Linux x86-64 numbers (0-334 and 424-461), each with the answer this" % len(linux))
    L.append("kernel gives it and the phase of [phases.md](phases.md) that owns it. Generated")
    L.append("by `scripts/dev/make-syscall-table.py` from the registry,")
    L.append("`kernel/abi/linux_syscalls.def` - the same file the kernel reads to answer a")
    L.append("number with no row, and `check-syscall-checks.py` holds every row against. Edit")
    L.append("the registry, not this page; `check.sh` fails while the two differ.")
    L.append("")
    L.append("| State | Count | What the kernel answers |")
    L.append("| --- | --- | --- |")
    L.append("| done | %d | through its row |" % count.get("done", 0))
    L.append("| partial | %d | through its row, with the gap named |" % count.get("partial", 0))
    L.append("| missing | %d | ENOSYS, and the boot gate fails naming the number |" % count.get("missing", 0))
    L.append("| deferred | %d | ENOSYS, and the boot gate fails naming the number |" % count.get("deferred", 0))
    L.append("| refused | %d | its errno, counted as refused - expected, never a failure |" % count.get("refused", 0))
    L.append("")
    L.append("Without a row, by phase:")
    L.append("")
    L.append("| Phase | Syscalls |")
    L.append("| --- | --- |")
    for ph in ("L1", "L2", "L3", "L4", "L5", "L6", "L7", "L8", "L9", "D", "R"):
        L.append("| %s - %s | %d |" % (ph, TITLES[ph], by_phase.get(ph, 0)))
    L.append("")
    L.append("A `partial` row names its gap and the phase that closes it. VibeOS's own two")
    L.append("calls, outside the Linux number space, are listed last.")
    L.append("")
    L.append("| Nr | Syscall | State | Phase | Errno | Row | Note |")
    L.append("| --- | --- | --- | --- | --- | --- | --- |")
    for nr, name, state, phase, err, why in entries:
        L.append("| %d | `%s` | %s | %s | %s | %s | %s |" % (
            nr, name, state, "-" if phase == "NONE" else phase,
            "-" if err == "NONE" else err,
            "`%s`" % rows[nr] if nr in rows else "", why.replace("|", "\\|")))
    out = "\n".join(L) + "\n"

    if "--check" in sys.argv:
        current = open(OUT, encoding="utf-8").read() if os.path.exists(OUT) else ""
        if current.replace("\r\n", "\n") != out:
            print("syscall-table=FAIL docs/abi/syscalls.md differs from the registry - "
                  "run scripts/dev/make-syscall-table.py")
            return 1
        print("syscall-table=ok numbers=%d" % len(linux))
        return 0
    with open(OUT, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(out)
    print("docs/abi/syscalls.md: %d numbers, %s" % (len(linux), count))
    return 0


if __name__ == "__main__":
    sys.exit(main())
