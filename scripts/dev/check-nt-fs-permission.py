#!/usr/bin/env python3
"""Does every NT function that changes the filesystem ask who is asking?

M-105 found NtOpenFile opening anything for anybody; three weeks later the v7
review found W4's rename, disposition, delete-on-close and time-setting paths
asking nothing either - the same class, written again in new code, because the
rule "a handler that makes, removes or changes a file asks the matching
question first" was a sentence in CLAUDE.md and not a check.

So: in kernel/abi/nt, a function that calls one of the VFS's mutating entry
points must also ask a credential question in the same function -
vibeos_cred_may*, nt_may_remove_path, or a walk as the caller
(vibeos_path_walk_as) - or be listed below with the reason it need not. A new
mutating call in a function that asks nothing fails here.

Usage: check-nt-fs-permission.py
"""

import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
AREA = os.path.join(ROOT, "kernel", "abi", "nt")
MUTATE = re.compile(r"\bvibeos_fs_(unlink|rmdir|rename|setattr|mkdir|create|link|symlink|truncate|mknod)\s*\(")
ASK = re.compile(r"\b(vibeos_cred_may\w*|nt_may_remove_path|vibeos_path_walk_as)\s*\(")
FUNC = re.compile(r"^(?:static\s+)?[A-Za-z_][A-Za-z0-9_ \*]*?\b([A-Za-z_][A-Za-z0-9_]*)\s*\(")

# Function -> why it may mutate without asking in its own body.
ALLOWED = {
    # FileBasicInformation's times: the handle had to be granted
    # FILE_WRITE_ATTRIBUTES, which the open judged by ownership or write
    # permission; the rename branch in the same function asks for itself.
    "nt_set_information_file": "FILE_WRITE_ATTRIBUTES judged at open; rename asks in place",
}


def functions(text):
    """(name, body) for each top-level function: from a line in column 0 that
    looks like a definition to the next line that is exactly '}'."""
    out, name, body = [], None, []
    for line in text.split("\n"):
        if name is None:
            m = FUNC.match(line)
            if m and line.rstrip().endswith("{") or (m and not line.rstrip().endswith(";")):
                if m and not line.startswith((" ", "\t", "#", "/", "*")):
                    name, body = m.group(1), [line]
            continue
        body.append(line)
        if line == "}":
            out.append((name, "\n".join(body)))
            name = None
    return out


def main():
    bad, seen = [], 0
    for path in sorted(glob.glob(os.path.join(AREA, "*.c"))):
        with open(path, encoding="utf-8") as f:
            text = f.read()
        for name, body in functions(text):
            if not MUTATE.search(body):
                continue
            seen += 1
            if ASK.search(body) or name in ALLOWED:
                continue
            bad.append("%s: %s changes the filesystem and asks no credential question"
                       % (os.path.basename(path), name))
    for b in bad:
        print("  " + b)
    if bad or seen == 0:
        print("nt-fs-permission=FAIL functions=%d unasked=%d" % (seen, len(bad)))
        return 1
    print("nt-fs-permission=ok functions=%d" % seen)
    return 0


if __name__ == "__main__":
    sys.exit(main())
