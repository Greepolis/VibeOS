#!/usr/bin/env python3
"""Is the Linux syscall table complete, unambiguous, and honest about its checks?

C4 gave the kernel a vocabulary (include/vibeos/abi.h) in which each operation is
declared once with the checks that apply to it, and made each syscall one row -

    X(0, read, READ, hw_sys_read(ARG(0), ARG(1), ARG(2)))

- in the file that holds its handler (kernel/abi/linux/*.c). This script reads the
rows against the declarations and against Linux's own numbers. Those files are
architecture code and are built only into the kernel image, so the host suite
cannot register them; this is the test that can.

## What it checks

  1. **A number claimed twice.** Two rows with one number would make the ABI
     ambiguous; the registry refuses it at boot too, and this says so before a boot.
  2. **A row's number is its name's number.** scripts/dev/linux-syscall-numbers.txt
     (from syscall_64.tbl) is the authority. A typo in a row is a *different
     syscall*: the handler compiles, the table registers, and a program calling the
     right number gets ENOSYS with nothing to say why.
  3. **An implemented syscall with no row, and a row nobody listed.** Both directions
     against that file, so adding a syscall means stating its number twice,
     independently.
  4. **An operation declared and never implemented.** An op in abi.h that no row
     names is a promise the kernel does not keep (and the boot stops on it too).
  5. **The declared checks equal the checks run**, in both directions: a check
     declared and not run (the handler stopped validating), or run and not declared
     (which is how "which calls touch user memory" stops being answerable from one
     place).

## How it decides "run"

For each row it takes the call expression, then follows every function it names
through the arch and Linux-ABI layers' own definitions - handlers call helpers that
call the choke points - and asks which are reachable:

  USER_MEMORY   hw_user_range_ok, hw_user_range_why, hw_user_addr_ok
  SIGNAL_PERMIT hw_signal_permitted
  TASK_GUARD    hw_task_alloc_guarded

An operation's checks are the union over its rows (fork and vfork are both FORK).

It is **reachability, not proof**. A handler that reaches `hw_user_range_ok` on a
path for a different argument satisfies the declaration without checking the
argument it should, and a function called through a pointer is not followed. Both
are stated rather than worked around: this catches a check that disappears entirely
(which is the failure that has actually occurred) and cannot certify that the right
pointer is the one checked. The choke-point counts in check-chokepoints.py are the
other half of the same argument.

Usage: check-syscall-checks.py [--list]
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

HEADER = os.path.join(ROOT, "include", "vibeos", "abi.h")
NUMBERS = os.path.join(ROOT, "scripts", "dev", "linux-syscall-numbers.txt")
ABI_DIR = os.path.join(ROOT, "kernel", "abi", "linux")
HANDLER_DIRS = (os.path.join(ROOT, "kernel", "arch", "x86_64"), ABI_DIR)

CHOKEPOINTS = {
    "USER_MEMORY": ("hw_user_range_ok", "hw_user_range_why", "hw_user_addr_ok",
                    "linux_user_ok", "USER_OUT", "USER_IN", "USER_OUT_OPT"),
    "SIGNAL_PERMIT": ("hw_signal_permitted",),
    "TASK_GUARD": ("hw_task_alloc_guarded",),
}

FUNC = re.compile(
    r"^(?:static\s+)?(?:[A-Za-z_][\w\s\*]*?)\b([A-Za-z_]\w*)\s*\(([^;{}]*)\)\s*\{",
    re.M)
IDENT = re.compile(r"\b[A-Za-z_]\w*\b")
ROW = re.compile(r"^\s*X\(\s*(\d+)\s*,\s*(\w+)\s*,\s*(\w+)\s*,\s*(.*)\)\s*\\?\s*$")


def read(path):
    with open(path, encoding="utf-8", errors="replace") as fh:
        return fh.read().replace("\r\n", "\n")


def declared():
    """op -> set of check names, from abi.h's X-macro list."""
    text = read(HEADER)
    start = text.index("#define VIBEOS_OP_LIST(X)")
    end = text.index("typedef enum vibeos_op_id")
    out = {}
    for m in re.finditer(r"X\((\w+),\s*\"[^\"]*\",\s*([^)]*)\)", text[start:end]):
        expr = m.group(2)
        out[m.group(1)] = set(
            n for n in ("USER_MEMORY", "SIGNAL_PERMIT", "TASK_GUARD")
            if "VIBEOS_CHECK_" + n in expr)
    return out


def authority():
    """nr -> name, from the numbers file."""
    out = {}
    for line in read(NUMBERS).splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        nr, name = line.split()
        out[int(nr)] = name
    return out


def rows():
    """[(nr, name, op, expression, file)] from every X(...) row in the ABI files."""
    out = []
    for name in sorted(os.listdir(ABI_DIR)):
        if not name.endswith(".c"):
            continue
        for line in read(os.path.join(ABI_DIR, name)).splitlines():
            m = ROW.match(line)
            if m:
                out.append((int(m.group(1)), m.group(2), m.group(3), m.group(4).strip(), name))
    return out


def function_bodies():
    """name -> body text, for every function in the arch layer and the Linux ABI layer."""
    bodies = {}
    for d in HANDLER_DIRS:
        if not os.path.isdir(d):
            continue
        for name in sorted(os.listdir(d)):
            if not name.endswith(".c"):
                continue
            text = read(os.path.join(d, name))
            for m in FUNC.finditer(text):
                fn = m.group(1)
                if fn in ("if", "for", "while", "switch", "return", "sizeof"):
                    continue
                close = text.find("\n}\n", m.end())
                if close < 0:
                    continue
                bodies.setdefault(fn, text[m.end():close])
    return bodies


def reachable_checks(text, bodies):
    seen = set()
    todo = [text]
    found = set()
    while todo:
        cur = todo.pop()
        for ident in IDENT.findall(cur):
            for check, names in CHOKEPOINTS.items():
                if ident in names:
                    found.add(check)
            if ident in bodies and ident not in seen:
                seen.add(ident)
                todo.append(bodies[ident])
    return found


def main():
    decl = declared()
    auth = authority()
    rs = rows()
    bodies = function_bodies()
    listing = "--list" in sys.argv
    bad = []

    by_nr = {}
    for nr, name, op, expr, f in rs:
        if nr in by_nr:
            bad.append("number %d is claimed twice: %s (%s) and %s (%s)"
                       % (nr, by_nr[nr][0], by_nr[nr][1], name, f))
        by_nr.setdefault(nr, (name, f))
        if op not in decl:
            bad.append("%s (%s): the operation %s is not declared in abi.h" % (name, f, op))
        if nr not in auth:
            bad.append("%s (%s): number %d is not in linux-syscall-numbers.txt" % (name, f, nr))
        elif auth[nr] != name:
            bad.append("number %d is %s in linux-syscall-numbers.txt but the row in %s calls it %s"
                       % (nr, auth[nr], f, name))
    for nr, name in sorted(auth.items()):
        if nr not in by_nr:
            bad.append("%s (%d) is listed as implemented and has no row" % (name, nr))

    per_op = {}
    for nr, name, op, expr, f in rs:
        per_op.setdefault(op, set()).update(reachable_checks(expr, bodies))
    for op in sorted(decl):
        if op not in per_op:
            bad.append("%s: declared in abi.h and no row implements it" % op)
            continue
        actual = per_op[op]
        if listing:
            print("  %-16s declared=%-34s reachable=%s" % (
                op, ",".join(sorted(decl[op])) or "-", ",".join(sorted(actual)) or "-"))
        for c in sorted(decl[op] - actual):
            bad.append("%s: declares %s and no path from its handler runs it" % (op, c))
        for c in sorted(actual - decl[op]):
            bad.append("%s: runs %s and does not declare it" % (op, c))

    if bad:
        for line in bad:
            print("  " + line)
        print("syscall-checks=FAIL problems=%d" % len(bad))
        return 1
    print("syscall-checks=ok operations=%d rows=%d" % (len(decl), len(rs)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
