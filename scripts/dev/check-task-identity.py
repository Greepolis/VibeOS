#!/usr/bin/env python3
"""C5: who owns "what is a task".

Two properties, both of which the type system cannot state:

1. **One definition.** The identity fields - ids, parentage, exit status, name,
   pending signals - are `vibeos_task_t` (include/vibeos/task_ident.h). `hw_task_t`
   must not declare one of them again: a second copy is the defect C5 exists to end,
   and it compiles happily (the compiler sees two unrelated members).

2. **A ratchet on how much of the arch layer reaches into identity.** The phase's
   done-condition is that arch_hw.c names no task field that is not part of a context
   switch. That is not true yet - it names identity 92 times - so the number is
   recorded and may only go DOWN, by moving a caller onto a function of the portable
   type (`vibeos_task_accountable_to`, `vibeos_task_set_comm`, ...). It goes up only
   as a decision, in the change that earns it.

Usage: check-task-identity.py [--list]
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
HEADER = os.path.join(ROOT, "kernel", "arch", "x86_64", "arch_hw_internal.h")
ARCH = os.path.join(ROOT, "kernel", "arch", "x86_64", "arch_hw.c")
IDENT = os.path.join(ROOT, "include", "vibeos", "task_ident.h")

# Lines of arch_hw.c that reach into a task's identity (`.id.` / `->id.`).
ARCH_IDENTITY_LINES = 92
# Lines anywhere in the arch layer or the Linux ABI that index the descriptor table
# themselves (`files.fds` / `files.std`) instead of asking vibeos_fdtable_*.
FILES_INDEX_LINES = 17


def read(p):
    return open(p, encoding="utf-8", errors="replace").read()


def identity_fields():
    t = read(IDENT)
    body = t[t.index("typedef struct vibeos_task {"):t.index("} vibeos_task_t;")]
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    names = []
    for m in re.finditer(r"^\s*(?:unsigned\s+)?\w+\s+(\w+)(?:\[\d+\])?;", body, re.M):
        names.append(m.group(1))
    return names


def task_struct():
    t = read(HEADER)
    end = t.index("} hw_task_t;")
    start = t.rfind("typedef struct {", 0, end)
    body = t[start:end]
    return re.sub(r"/\*.*?\*/", "", body, flags=re.S)


def main():
    bad = []
    fields = identity_fields()
    if len(fields) < 10:
        bad.append("could not read the identity fields from task_ident.h (%d found)" % len(fields))
    body = task_struct()
    for f in fields:
        if re.search(r"\b%s\b\s*(\[\d+\])?\s*;" % re.escape(f), body):
            bad.append("hw_task_t declares `%s` again - it is vibeos_task_t's (task_ident.h)" % f)
    if not re.search(r"\bvibeos_task_t\s+id\s*;", body):
        bad.append("hw_task_t does not embed vibeos_task_t as `id`")

    lines = sum(1 for l in read(ARCH).splitlines() if re.search(r"(?:\.|->)id\.", l))
    if "--list" in sys.argv:
        print("  identity fields: %s" % " ".join(fields))
        print("  arch_hw.c lines reaching into identity: %d (ratchet %d)" % (lines, ARCH_IDENTITY_LINES))
    if lines > ARCH_IDENTITY_LINES:
        bad.append("arch_hw.c reaches into task identity on %d lines, ratchet is %d - "
                   "go through a vibeos_task_* function, or record why it grew"
                   % (lines, ARCH_IDENTITY_LINES))
    elif lines < ARCH_IDENTITY_LINES:
        bad.append("arch_hw.c reaches into task identity on %d lines, DOWN from the ratchet %d - "
                   "lower ARCH_IDENTITY_LINES in the same commit" % (lines, ARCH_IDENTITY_LINES))

    for f in ("state", "ready_at", "ran_once"):
        if re.search(r"\b%s\b\s*;" % f, body):
            bad.append("hw_task_t declares `%s` again - it is the task layer's (task.h): "
                       "a second copy is a second opinion about what a task is doing" % f)
    for f in ("fds", "std_redirect"):
        if re.search(r"\b%s\b\s*(\[[^\]]*\])?\s*;" % f, body):
            bad.append("hw_task_t declares `%s` again - descriptors are vibeos_fdtable_t's (fdtable.h)" % f)
    if not re.search(r"\bvibeos_fdtable_t\s+files\s*;", body):
        bad.append("hw_task_t does not embed vibeos_fdtable_t as `files`")
    idx = 0
    for d in (os.path.join(ROOT, "kernel", "arch", "x86_64"), os.path.join(ROOT, "kernel", "abi", "linux")):
        for name in sorted(os.listdir(d)):
            if name.endswith(".c"):
                idx += sum(1 for l in read(os.path.join(d, name)).splitlines()
                           if re.search(r"files\.(fds|std)\b", l))
    if "--list" in sys.argv:
        print("  lines indexing the descriptor table directly: %d (ratchet %d)" % (idx, FILES_INDEX_LINES))
    if idx > FILES_INDEX_LINES:
        bad.append("%d lines index the descriptor table directly, ratchet is %d - ask vibeos_fdtable_*"
                   % (idx, FILES_INDEX_LINES))
    elif idx < FILES_INDEX_LINES:
        bad.append("%d lines index the descriptor table directly, DOWN from %d - lower FILES_INDEX_LINES"
                   % (idx, FILES_INDEX_LINES))

    if bad:
        for b in bad:
            print("  " + b)
        print("task-identity=FAIL problems=%d" % len(bad))
        return 1
    print("task-identity=ok fields=%d arch_lines=%d" % (len(fields), lines))
    return 0


if __name__ == "__main__":
    sys.exit(main())
