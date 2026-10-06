#!/usr/bin/env python3
"""A socket syscall that waits must re-verify its socket after every wake (M-020).

A blocking socket call holds a descriptor across a wait. A sibling thread can
close that descriptor and open another socket into the same slot, and a loop
that re-reads `f->net_sock` on each pass then carries on against somebody else's
socket. The re-check is `vibeos_sockfile_stable(f)` since docs/abi/ A3: the call
holds a reference to the socket's open file description, so the description
cannot be closed and reused under it any more, but the socket itself can still
go - a process's exit releases the sockets it owns - and its slot in the stack
be given to another; the check compares the socket's tenancy with the one the
description was opened on.

M-020's fix put it into connect, accept and the stream read, and missed
`recvfrom` - found by an external review a phase later, still open while the
tracker said "fixed". Nothing checked the rule, so the fourth call site was left
to whoever remembered. This checks it.

## The rule

In kernel/abi/files/socket.c - where the socket waits moved in A3, out of the
Linux handlers - every function that both holds a description (names
`vibeos_file_t`) and waits (calls `socket_wait_tick()`, or `socket_wait()`) must call
`vibeos_sockfile_stable(`. (vibeos_fd_t, linux_net_wait_tick and linux_sock_stable
in kernel/abi/linux/net.c until A3; hw_ names before A2.) A wait with no socket -
netctl's ping and DNS, in net.c - has nothing a sibling can close, and is not
held to it.

Functions are found by their definition and their matching brace. A function
it cannot find is a failure, not a pass.

Usage: check-net-stable.py
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(ROOT, "kernel", "abi", "files", "socket.c")

# A definition: a name, a parameter list that may span lines, and its brace.
OPEN = re.compile(r"^(?:static\s+)?[A-Za-z_][\w\s\*]*?\b(\w+)\s*\(([^;{}]*)\)\s*\{", re.M)


def functions(text):
    """(name, body) for every top-level function definition, its body found by
    matching braces. The first version read a signature off one line, and the
    waits that moved into socket.c in A3 have two-line signatures: it saw two of
    four and said so rather than passing, which is why that guard is there."""
    out = []
    for m in OPEN.finditer(text):
        if m.group(1) in ("if", "for", "while", "switch", "return", "sizeof"):
            continue
        depth, i = 1, m.end()
        while i < len(text) and depth:
            if text[i] == "{":
                depth += 1
            elif text[i] == "}":
                depth -= 1
            i += 1
        if depth == 0:
            out.append((m.group(1), text[m.start():i]))
    return out


def main():
    try:
        text = open(SRC, encoding="utf-8").read()
    except OSError as exc:
        print("net-stable=FAIL cannot read %s: %s" % (SRC, exc))
        return 1
    funcs = functions(text)
    if not funcs:
        print("net-stable=FAIL no functions found in socket.c; the parser no longer matches the file")
        return 1

    # A wait is a call to socket_wait_tick() or, since L4 step 7, to
    # socket_wait(), which adds the signal and O_NONBLOCK and knows no socket -
    # so the helper itself is not one of the functions held to the rule.
    waiting = [(n, b) for n, b in funcs
               if n != "socket_wait" and "vibeos_file_t" in b
               and ("socket_wait_tick()" in b or "socket_wait(f" in b)]
    bad = [n for n, b in waiting if "vibeos_sockfile_stable(" not in b]
    # The check has to have something to look at: the three waits M-020 fixed
    # first are known to exist. Fewer means the parser lost them, not that they
    # were all removed.
    if len(waiting) < 3:
        print("net-stable=FAIL found %d waiting socket functions, expected at least 3 - "
              "the parser no longer sees them" % len(waiting))
        return 1
    if bad:
        for n in bad:
            print("  %s waits holding a description and never calls vibeos_sockfile_stable" % n)
        print("net-stable=FAIL unchecked=%d" % len(bad))
        return 1
    print("net-stable=ok waiting=%d" % len(waiting))
    return 0


if __name__ == "__main__":
    sys.exit(main())
