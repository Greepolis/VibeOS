#!/usr/bin/env python3
"""A socket syscall that waits must re-verify its socket after every wake (M-020).

A blocking socket call holds a descriptor across a wait. A sibling thread can
close that descriptor and open another socket into the same slot, and a loop
that re-reads `f->net_sock` on each pass then carries on against somebody else's
socket. `hw_sock_stable(f, sock, gen)` is the re-check: descriptor still open,
still the same socket index, socket slot not reused.

M-020's fix put it into connect, accept and the stream read, and missed
`recvfrom` - found by an external review a phase later, still open while the
tracker said "fixed". Nothing checked the rule, so the fourth call site was left
to whoever remembered. This checks it.

## The rule

In kernel/abi/linux/net.c, every function that both holds a descriptor (names
`hw_fd_t`) and waits (calls `hw_net_wait_tick()`) must call `hw_sock_stable(`.
A function that waits without a descriptor - netctl's ping and DNS - has nothing
a sibling can close, and is not held to it.

Crude by design: functions are found by their opening line at column zero and
their closing brace at column zero, which is how every function in the file is
written. A function it cannot find is a failure, not a pass.

Usage: check-net-stable.py
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(ROOT, "kernel", "abi", "linux", "net.c")

OPEN = re.compile(r"^(?:static\s+)?[A-Za-z_][\w\s\*]*?\b(\w+)\s*\([^;]*\)\s*\{\s*$")


def functions(text):
    """(name, body) for every top-level function definition."""
    out = []
    name, body = None, []
    for line in text.splitlines():
        if name is None:
            m = OPEN.match(line)
            if m:
                name, body = m.group(1), [line]
            continue
        body.append(line)
        if line.startswith("}"):
            out.append((name, "\n".join(body)))
            name, body = None, []
    return out


def main():
    try:
        text = open(SRC, encoding="utf-8").read()
    except OSError as exc:
        print("net-stable=FAIL cannot read %s: %s" % (SRC, exc))
        return 1
    funcs = functions(text)
    if not funcs:
        print("net-stable=FAIL no functions found in net.c; the parser no longer matches the file")
        return 1

    waiting = [(n, b) for n, b in funcs if "hw_net_wait_tick()" in b and "hw_fd_t" in b]
    bad = [n for n, b in waiting if "hw_sock_stable(" not in b]
    # The check has to have something to look at: the three waits M-020 fixed
    # first are known to exist. Fewer means the parser lost them, not that they
    # were all removed.
    if len(waiting) < 3:
        print("net-stable=FAIL found %d waiting socket functions, expected at least 3 - "
              "the parser no longer sees them" % len(waiting))
        return 1
    if bad:
        for n in bad:
            print("  %s waits holding a descriptor and never calls hw_sock_stable" % n)
        print("net-stable=FAIL unchecked=%d" % len(bad))
        return 1
    print("net-stable=ok waiting=%d" % len(waiting))
    return 0


if __name__ == "__main__":
    sys.exit(main())
