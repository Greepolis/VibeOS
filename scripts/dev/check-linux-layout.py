#!/usr/bin/env python3
"""Is every Linux layout and number the kernel declares compared with Linux's?

docs/abi/ phase A5. The kernel is freestanding, so it declares Linux's
structures and constants itself (include/vibeos/linux_layout.h), and its errno
values, signal numbers and file flags are its own numbered as Linux's.
tests/kernel/linux_layout_tests.c compiles against the host's uapi headers and
compares them. That comparison is only as complete as the list of things it
names, and a list somebody has to remember to extend is the failure this project
keeps producing - so it is checked:

  1. Every field of every structure in linux_layout.h is compared (FIELD,
     FIELD2, LIBC_FIELD or LIBC_OFFSET), and every structure without a flexible
     array member has its size compared (SIZE).
  2. Every Linux-valued constant in the headers that hold them is compared
     (CONST or LIBC_CONST). Which names are Linux-valued is decided by prefix
     per header, below; a new family of constants in a new header has to be
     added here, which is a decision rather than an accident.
  3. No file under kernel/abi/ defines a constant under Linux's own spelling -
     `#define AT_FDCWD`, `#define TIOCGPGRP` - which is how two copies of the
     same number came to live in two files. They belong in linux_layout.h.

Usage: check-linux-layout.py [--list]
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
LAYOUT = os.path.join(ROOT, "include", "vibeos", "linux_layout.h")
TEST = os.path.join(ROOT, "tests", "kernel", "linux_layout_tests.c")
ABI = os.path.join(ROOT, "kernel", "abi")

# header -> the names in it that carry Linux's values.
CONSTANT_HEADERS = {
    "include/vibeos/linux_layout.h": r"LINUX_\w+",
    "include/vibeos/abi_linux.h": r"VIBEOS_E[A-Z0-9]+|VIBEOS_SIG[A-Z]+|VIBEOS_SA_\w+|SIG_DFL_ADDR|SIG_IGN_ADDR",
    "include/vibeos/file.h": r"VIBEOS_O_\w+|VIBEOS_SEEK_\w+",
    "include/vibeos/vfs.h": r"VIBEOS_S_I[FS]\w+",
    "include/vibeos/fdtable.h": r"VIBEOS_FD_CLOEXEC",
    "include/vibeos/fileops.h": r"VIBEOS_IOCTL_\w+",
    "include/vibeos/tty.h": r"VIBEOS_TTY_\w+",
}

# Linux's own spellings, which a file under kernel/abi/ must not define itself.
LINUX_SPELLING = re.compile(
    r"^\s*#\s*define\s+((?:AT|F|O|PROT|MAP|CLONE|SIG|SA|FUTEX|PR|ARCH|RLIMIT|RLIM64|"
    r"SEEK|DT|AF|SOCK|CLOSE_RANGE|FD|S)_\w+|TIOC\w+|W(?:NOHANG|UNTRACED|CONTINUED|EXITED|NOWAIT)|"
    r"E(?:PERM|NOENT|INVAL|NOSYS|FAULT|BADF|AGAIN|NOMEM|EXIST|NOTDIR|ISDIR|RANGE|NAMETOOLONG)\w*)\b",
    re.M)


def read(p):
    with open(p, encoding="utf-8", errors="replace") as fh:
        return fh.read().replace("\r\n", "\n")


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def structs(text):
    """{typedef name: ([fields], has_flexible_array)} from `typedef struct {...} name;`."""
    out = {}
    for m in re.finditer(r"typedef\s+struct\s*\{(.*?)\}\s*(\w+)\s*;", text, re.S):
        fields, flexible = [], False
        for decl in m.group(1).split(";"):
            decl = decl.strip()
            if not decl:
                continue
            f = re.search(r"(\w+)\s*(\[[^\]]*\])?\s*$", decl)
            if not f:
                continue
            fields.append(f.group(1))
            if f.group(2) == "[]":
                flexible = True
        out[m.group(2)] = (fields, flexible)
    return out


def main():
    bad = []
    test = strip_comments(read(TEST))
    layout = strip_comments(read(LAYOUT))

    compared_fields = set()
    for m in re.finditer(r"\b(?:FIELD2?|TAIL|HOST_FIELD|LIBC_FIELD|LIBC_OFFSET)\(\s*(\w+)\s*,\s*(?:[^,()]+,\s*)?(\w+)", test):
        compared_fields.add((m.group(1), m.group(2)))
    # PAD(ours, field, theirs, next): padding Linux leaves unnamed, compared by
    # where Linux's next field starts.
    for m in re.finditer(r"\bPAD\(\s*(\w+)\s*,\s*(\w+)\s*,", test):
        compared_fields.add((m.group(1), m.group(2)))
    sized = set(re.findall(r"\bSIZE\(\s*(\w+)\s*,", test))
    compared_consts = set(re.findall(r"\b(?:CONST|LIBC_CONST)\(\s*(\w+)\s*,", test))

    nfields = 0
    table = structs(layout)
    for name, (fields, flexible) in sorted(table.items()):
        for f in fields:
            nfields += 1
            if (name, f) not in compared_fields:
                bad.append("%s.%s is declared and never compared with Linux's" % (name, f))
        if not flexible and name not in sized:
            bad.append("sizeof(%s) is never compared with Linux's" % name)

    nconsts = 0
    for rel, pattern in CONSTANT_HEADERS.items():
        text = strip_comments(read(os.path.join(ROOT, rel)))
        for m in re.finditer(r"^\s*#\s*define\s+(" + pattern + r")\b(?!\()", text, re.M):
            name = m.group(1)
            nconsts += 1
            if name not in compared_consts:
                bad.append("%s defines %s and the layout test never compares it" % (rel, name))

    for base, _, names in os.walk(ABI):
        for n in sorted(names):
            if not n.endswith((".c", ".h")):
                continue
            p = os.path.join(base, n)
            for m in LINUX_SPELLING.finditer(strip_comments(read(p))):
                bad.append("%s defines %s under Linux's own spelling: it belongs in "
                           "vibeos/linux_layout.h, compared with Linux's"
                           % (os.path.relpath(p, ROOT), m.group(1)))

    if "--list" in sys.argv:
        print("  structures: %d, fields: %d, constants: %d" % (len(table), nfields, nconsts))
    if bad:
        for b in bad:
            print("  " + b)
        print("linux-layout=FAIL problems=%d" % len(bad))
        return 1
    print("linux-layout=ok structs=%d fields=%d constants=%d" % (len(table), nfields, nconsts))
    return 0


if __name__ == "__main__":
    sys.exit(main())
