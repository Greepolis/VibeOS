#!/usr/bin/env python3
"""Does every syscall personality stay above the kernel services?

docs/abi/ phase A2 took the Linux handlers off the architecture's private header:
about seven hundred references to `hw_*`, `g_tasks`, the trap frame and inline
assembly became calls into include/vibeos/ksvc.h, which the architecture
implements (kernel/arch/x86_64/ksvc.c) and the host tests implement again
(tests/kernel/ksvc_fake.c). That is what lets the handlers run in the host test
binary. It is also the kind of property that erodes one convenient line at a
time, so it is checked:

  1. No file under kernel/abi/ (any personality - Linux today, Windows next)
     includes an architecture header or names the architecture: `hw_` symbols,
     any global arch_hw_internal.h declares (g_tasks, g_timer_ticks, ...),
     `vibeos_x86_64_`, `arch_hw_internal.h`, inline assembly.
     Comments are ignored - a note saying where a file was lifted from is
     history, not a dependency.
  2. Every function ksvc.h declares is defined in *both* implementations. The
     linker cannot say this: a host test binary only pulls in what the tests
     reach, so a service the fake forgot fails the day a test first calls it,
     far from the change that added it.

Ratcheted at zero.

Usage: check-abi-layering.py [--list]
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ABI = os.path.join(ROOT, "kernel", "abi")
HEADER = os.path.join(ROOT, "include", "vibeos", "ksvc.h")
IMPLS = (os.path.join(ROOT, "kernel", "arch", "x86_64", "ksvc.c"),
         os.path.join(ROOT, "tests", "kernel", "ksvc_fake.c"))
# Where an implementation defines a service outside its ksvc file. The copy that
# can take a fault and recover is assembly: the fault handler resumes it at a
# known address, which C cannot promise.
ELSEWHERE = {
    "vibeos_uaccess_copy": (os.path.join(ROOT, "kernel", "arch", "x86_64", "uaccess.S"),
                            "vibeos_uaccess_copy:"),
}

ARCH_HEADER = os.path.join(ROOT, "kernel", "arch", "x86_64", "arch_hw_internal.h")

FORBIDDEN = re.compile(r"\bhw_\w+|\bg_tasks\b|\bvibeos_x86_64_\w+|arch_hw_internal\.h|"
                       r"#\s*include\s*\"[^\"]*arch/|__asm__|\basm\s*\(")


def read(p):
    with open(p, encoding="utf-8", errors="replace") as fh:
        return fh.read().replace("\r\n", "\n")


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def declared(text):
    text = strip_comments(text)
    names = []
    for m in re.finditer(r"^[A-Za-z_][\w\s\*]*?\b(ks_\w+|vibeos_uaccess_copy)\s*\(", text, re.M):
        if m.group(1) not in names:
            names.append(m.group(1))
    return names


def defined(text):
    text = strip_comments(text)
    return set(re.findall(r"^[A-Za-z_][\w\s\*]*?\b(ks_\w+|vibeos_uaccess_copy)\s*\([^;]*?\)\s*\{",
                          text, re.M | re.S))


def arch_globals():
    """Every global the architecture's private header names - its externs and
    the macros standing in for them (g_current_task). Read from the header so a
    global added there is forbidden here the same day; the first version of this
    check listed g_tasks alone, and a handler reading g_timer_ticks passed it."""
    text = strip_comments(read(ARCH_HEADER))
    names = set(re.findall(r"^extern\b[^;]*?\b(g_\w+)\s*(?:\[[^\]]*\])*\s*(?:__attribute__\S*)?\s*;", text, re.M))
    names |= set(re.findall(r"^#define\s+(g_\w+)\b", text, re.M))
    return names


def main():
    bad = []
    files = 0
    globals_ = arch_globals()
    uses_global = re.compile(r"\b(" + "|".join(sorted(globals_)) + r")\b") if globals_ else None
    for base, _, names in os.walk(ABI):
        for n in sorted(names):
            if not n.endswith((".c", ".h")):
                continue
            p = os.path.join(base, n)
            files += 1
            for i, line in enumerate(strip_comments(read(p)).split("\n"), 1):
                m = FORBIDDEN.search(line) or (uses_global.search(line) if uses_global else None)
                if m:
                    bad.append("%s:%d names the architecture: %s"
                               % (os.path.relpath(p, ROOT), i, m.group(0)))

    decl = declared(read(HEADER))
    for impl in IMPLS:
        have = defined(read(impl))
        for name in decl:
            other = ELSEWHERE.get(name)
            if impl == IMPLS[0] and other and other[1] in read(other[0]):
                continue
            if name not in have:
                bad.append("%s declares %s and %s does not define it"
                           % (os.path.relpath(HEADER, ROOT), name, os.path.relpath(impl, ROOT)))
    if "--list" in sys.argv:
        print("  services declared: %d; personality files: %d; architecture globals forbidden: %d"
              % (len(decl), files, len(globals_)))
    if bad:
        for b in bad:
            print("  " + b)
        print("abi-layering=FAIL problems=%d" % len(bad))
        return 1
    print("abi-layering=ok services=%d files=%d" % (len(decl), files))
    return 0


if __name__ == "__main__":
    sys.exit(main())
