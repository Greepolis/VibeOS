#!/usr/bin/env python3
"""C5: who owns "what is a task".

Two properties, both of which the type system cannot state:

1. **One definition.** The identity fields - ids, parentage, exit status, name,
   pending signals - are `vibeos_task_t` (include/vibeos/task_ident.h). `hw_task_t`
   must not declare one of them again: a second copy is the defect C5 exists to end,
   and it compiles happily (the compiler sees two unrelated members).

2. **arch_hw.c names identity only in the context switch.** The phase's
   done-condition: arch_hw.c names no task field that is not part of a context switch.
   The check reads the file function by function; identity outside the five that make
   the switch is a failure, and the lines inside them may only go down.

Usage: check-task-identity.py [--list]
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
HEADER = os.path.join(ROOT, "kernel", "arch", "x86_64", "arch_hw_internal.h")
ARCH = os.path.join(ROOT, "kernel", "arch", "x86_64", "arch_hw.c")
IDENT = os.path.join(ROOT, "include", "vibeos", "task_ident.h")

# The functions of arch_hw.c that may name a task's identity: they *are* the context
# switch - deciding whether a task is idle or a user task, and reporting who it was when
# the switch refuses. Everything else in that file asks (hw_task_pid_of, ...) or lives in
# task_life.c. Done-condition of C5, as a check.
SWITCH_FUNCTIONS = {"vibeos_x86_64_isr_handler", "hw_task_runnable", "hw_task_load_cpu_state",
                    "hw_ctx_check", "hw_schedule"}
# Lines they name it on. It was 92 across the file before the lifecycle moved out; it may
# only go DOWN from here.
ARCH_IDENTITY_LINES = 12
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

    # Where in arch_hw.c does code name a task's identity? Per function, because the
    # done-condition is about *which* code may: the context switch and nothing else.
    lines = 0
    outside = {}
    cur = None
    head = None   # the name of a function whose signature is still being read over several lines
    for l in read(ARCH).splitlines():
        m = re.match(r"^[A-Za-z_][^;=]*?\b(\w+)\s*\([^;]*\)\s*\{\s*$", l)
        h = re.match(r"^[A-Za-z_][^;=(]*?\b(\w+)\s*\([^)]*$", l)
        if h:
            head = h.group(1)
        if m:
            cur = m.group(1)
            head = None
        elif head and re.match(r"^\s+[^;]*\)\s*\{\s*$", l):
            cur = head
            head = None
        elif l.startswith("}"):
            cur = None
        if re.search(r"(?:\.|->)id\.", l):
            lines += 1
            if cur not in SWITCH_FUNCTIONS:
                outside[cur] = outside.get(cur, 0) + 1
    if "--list" in sys.argv:
        print("  identity fields: %s" % " ".join(fields))
        print("  arch_hw.c lines naming identity: %d in the context switch (ratchet %d), %d elsewhere"
              % (lines - sum(outside.values()), ARCH_IDENTITY_LINES, sum(outside.values())))
    for fn, n in sorted(outside.items(), key=lambda kv: str(kv[0])):
        bad.append("arch_hw.c names a task's identity in %s (%d lines), which is not part of the "
                   "context switch - ask hw_task_*_of / a vibeos_task_* function, or move it to task_life.c"
                   % (fn, n))
    if lines > ARCH_IDENTITY_LINES:
        bad.append("arch_hw.c names task identity on %d lines, ratchet is %d - "
                   "go through a vibeos_task_* function, or record why it grew"
                   % (lines, ARCH_IDENTITY_LINES))
    elif lines < ARCH_IDENTITY_LINES:
        bad.append("arch_hw.c names task identity on %d lines, DOWN from the ratchet %d - "
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
