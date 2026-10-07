#!/usr/bin/env python3
"""Does the Linux ABI layer touch user memory only through the fault-safe copy?

A syscall handler receives user addresses as integers. The dispatcher checks the
range before the handler runs (kernel/abi/linux/dispatch.c), and that check and
the handler's access are two instants: a sibling thread can munmap the buffer in
between, and an ordinary load or store then faults in ring 0, outside the one
instruction the trap handler can recover (vibeos_uaccess_copy), and panics the
kernel. Any program with a second thread can do it to itself.

That shape has been found and fixed one site at a time: H-003 and H-010 (futex,
pipes, console read, sockets), H-026, M-040 (write to a file), then in one review
M-050 to M-052 - the socket address helpers, netctl, read() of a file, the
console branch of write(), getdents64, uname and prlimit. Eight sites in one pass
is the argument for a check rather than a ninth fix.

## The rule

In every .c file under kernel/abi/ - the personalities and the file types they
share - every conversion of an integer to a pointer - a cast to
a pointer type applied to `(uintptr_t)...` - is on a line that hands it straight
to one of the calls that copy fault-safely:

  vibeos_uaccess_copy(        the copy with a recovery point
  hw_copy_user_string(        built on it
  vibeos_pipe_read(           the pipe module copies through a callback,
  vibeos_pipe_write(          and it is given vibeos_uaccess_copy

A pointer made from a user address and kept in a variable is what every one of
those defects looked like, so a cast anywhere else is a failure. The exceptions
below are not user memory, and say what they are instead.

## What it cannot see

A cast split across lines from its call, or an access through a pointer that
arrived already typed. Neither exists today; the first would fail this check
(the cast line lacks the call), which is the safe direction.

## Where it looks

Until docs/abi/ L1 it looked in kernel/abi/linux/ only. A3 had moved read, write,
getdents64, the console and the sockets - most of the sites listed above - into
kernel/abi/files/, and from that day nothing watched them: the case that hands
read()'s user buffer to the filesystem went NOT RED when it was next run, which
is how this was found. A check scoped by directory follows the code only if
somebody moves the scope with it.

Usage: check-user-access.py [--list]
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
AREA = os.path.join(ROOT, "kernel", "abi")

CAST = re.compile(r"\(\s*(?:const\s+)?(?:volatile\s+)?[A-Za-z_][A-Za-z0-9_ ]*\*+\s*\)\s*\(uintptr_t\)")
SAFE = ("vibeos_uaccess_copy(", "hw_copy_user_string(", "vibeos_pipe_read(",
        "vibeos_pipe_write(")

# file -> substring of the line, with the reason it is not user memory.
EXCEPT = {
    "mm.c": [("(uintptr_t)phys",
              "pageinfo reads the first word of a physical frame through the "
              "kernel's identity map - kernel memory, not the caller's")],
}


MIN_CASTS = 120


def main():
    listing = "--list" in sys.argv
    bad = []
    seen = 0
    sources = []
    for base, _, names in os.walk(AREA):
        sources += [os.path.join(base, n) for n in names if n.endswith(".c")]
    for path in sorted(sources):
        name = os.path.basename(path)
        with open(path, encoding="utf-8", errors="replace") as fh:
            lines = fh.read().splitlines()
        for no, line in enumerate(lines, 1):
            code = line.split("//")[0]
            if not CAST.search(code):
                continue
            seen += 1
            if any(s in code for s in SAFE):
                continue
            if any(sub in code for sub, _ in EXCEPT.get(name, ())):
                if listing:
                    print("  allowed %s:%d  (not user memory)" % (name, no))
                continue
            bad.append("%s:%d: %s" % (name, no, line.strip()))
    for b in bad:
        print("  user pointer outside the fault-safe copy: " + b)
    if bad:
        print("user-access=FAIL raw=%d casts=%d" % (len(bad), seen))
        return 1
    # A scan that found nothing to judge is not a pass. The scope of this check
    # has already stopped matching the code once (A3 moved most of its sites
    # out of the directory it watched); a floor turns the next such move into a
    # failure instead of a quieter ok (external review, 2026-10-07). Raise it
    # as the code grows; lower it only as a decision.
    if seen < MIN_CASTS:
        print("user-access=FAIL reason=scanned_too_little casts=%d floor=%d" % (seen, MIN_CASTS))
        return 1
    print("user-access=ok casts=%d" % seen)
    return 0


if __name__ == "__main__":
    sys.exit(main())
