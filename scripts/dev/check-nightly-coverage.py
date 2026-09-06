#!/usr/bin/env python3
"""Which kernel modules have no intensive nightly test?

**Every module gets one.** That is the rule, and it is here rather than only in
CLAUDE.md because a rule nobody can check is a rule that decays - this project
has the evidence, in the shape of three lessons that were written down and then
broken by somebody quoting them.

## What an intensive test is

Not the host tests, which every module has and which check the cases somebody
thought of. Intensive means long, randomised, and checked against something
other than the code under test: many seeds against a reference model, a soak
whose length is the point, a fuzzer. The two that exist show the shape - the
memory manager's torture compares every frame's owner count against a model in
plain arrays, and the scheduler's does the same for the picker.

The reason is the same in both: a subsystem asked whether it is correct answers
from the numbers it used to decide, so the defects that survive are the
self-consistent ones. This kernel has had exactly that in both subsystems - a
frame the allocator believed nobody owned while a process ran from it, and a
task class derived from is_idle that collapsed KERNEL into NORMAL and stayed
self-consistent for months.

## How a job declares what it covers

A `# module: <names>` comment inside the job in .github/workflows/nightly.yml.
A comment rather than a YAML key because GitHub rejects keys it does not know,
and a separate manifest would be a second place to keep true.

## A ratchet

Eleven of thirteen modules had nothing when this was written. Demanding zero
would start red, and a check that starts red is one nobody reads. The number
may only go down.
"""

import os
import re
import sys

# Modules with nothing yet: seven of thirteen. Each is a debt, not a decision -
# unlike the
# exemptions in check-counters-produced.py, there is no good reason for any of
# these, only an order to do them in.
# Raised from 7 to 8 in I5c, as a decision rather than as a convenience.
#
# The journal moved from kernel/fs/ to kernel/txn/ - it is not a filesystem
# concern - and that created a module directory with no intensive nightly test.
#
# One was written and then deleted, which is the part worth recording. It ran
# thousands of randomised transactions with the power cut inside each, and
# three separate sabotages of the journal - committing without flushing the
# data, and dropping each of the two checksum comparisons in recovery - were
# caught on ZERO of thirty seeds. A test that cannot fail is not a test, and
# shipping one as coverage is worse than the gap, because the gap is at least
# visible. See scripts/dev/cases/io-journal.txt for what was tried.
#
# What the journal does have is an exhaustive host sweep: the power-off point
# walked across every write of a fixed transaction, on a drive model whose
# cache reorders. That is a real check and it is not nothing; it is just not
# what this rule asks for. Lowering this number again means writing a torture
# that discriminates.
BASELINE = 8

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
NIGHTLY = os.path.join(ROOT, ".github", "workflows", "nightly.yml")
KERNEL = os.path.join(ROOT, "kernel")


def modules():
    return sorted(d for d in os.listdir(KERNEL)
                  if os.path.isdir(os.path.join(KERNEL, d)))


def covered():
    if not os.path.isfile(NIGHTLY):
        return set()
    text = open(NIGHTLY, encoding="utf-8").read()
    out = set()
    for m in re.finditer(r"^\s*#\s*module:\s*(.+)$", text, re.M):
        for name in m.group(1).split():
            out.add(name.strip())
    return out


def main():
    have = covered()
    missing = [m for m in modules() if m not in have]

    print(f"modules={len(modules())} covered={len(modules()) - len(missing)} "
          f"uncovered={len(missing)}")
    for m in missing:
        print("  no intensive nightly test: kernel/" + m)
    # A declared module that does not exist is a job pointing at nothing.
    for h in sorted(have):
        if h not in modules():
            print("  a nightly job claims a module that is not there: " + h)
            return 1

    if len(missing) > BASELINE:
        print(f"nightly-coverage=FAIL uncovered={len(missing)} "
              f"baseline={BASELINE}")
        return 1
    print("nightly-coverage=ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
