#!/usr/bin/env python3
"""Which boot-gate assertions has nobody ever proved can fire?

An assertion that cannot go red is not a check. This project has shipped one:
`VIBEOS_BLK_TIMEOUT` was defined, printed by kmain and asserted by the gate,
and no driver ever produced it - so the assertion was green by construction for
as long as it existed. It was found by reading the code, which is the expensive
way and does not scale to a hundred and twenty of them.

The mechanical version: every assertion the gate can raise should be named by
some sabotage case in scripts/dev/cases/, because that is where this project
records "I broke this on purpose and watched it go red". An assertion no case
mentions has never been observed failing, and might not be able to.

## What it does not claim

Coverage here is a *name* appearing in a case file, not proof the case was run
or that it turned that assertion red. It is a floor, not a ceiling: it finds
assertions nobody has even written a case for, which is the cheap half of the
problem and the half that hides a check like BLK_TIMEOUT.

Some assertion names are assembled in a loop - `f"mm_{name}={value}"` - and the
literal prefix is all that survives. Those are reported separately rather than
counted as missing: the tool cannot tell, and a tool that guesses here would
produce exactly the confidently-wrong answer this project distrusts.

## A ceiling, not equality

Reported against a baseline rather than demanding zero, for the reason this
project already applies to the frame accounting and the cache ratio: a check
that fails on every run is a check people route around. The number may not
grow. Lowering it is a decision somebody makes deliberately, here, in the same
change that earns it.
"""

import os
import re
import sys

# How many uncovered assertions this tree had when the check was written: 108
# of 115. That number is not a target and not an excuse - it is a ratchet.
#
# Zero would be the honest goal and a check that starts red is a check nobody
# reads; this project has one of those in its history and a rule about it. So
# the job here is narrower and achievable: **stop the number growing**. A new
# assertion arriving without a case is exactly how VIBEOS_BLK_TIMEOUT happened,
# and it is the case this catches from the day it is written rather than months
# later by reading.
#
# Lower it whenever cases are added. Raising it is a decision somebody makes
# here, in the same change that earns it, with a reason.
BASELINE = 108

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
GATE = os.path.join(ROOT, "scripts", "qemu-cli-smoke-linux.py")
CASES = os.path.join(ROOT, "scripts", "dev", "cases")


def assertion_keys(path):
    """Every distinct name the gate can put in `reason=`."""
    text = open(path, encoding="utf-8").read()
    keys, dynamic = set(), set()
    for m in re.finditer(r'problems\.append\(\s*f?"([^"]*)"', text):
        raw = m.group(1)
        head = re.match(r"[a-z0-9_]+", raw)
        if not head:
            continue
        key = head.group(0)
        # A name that is only a prefix before a substitution cannot be matched
        # against a case file with any confidence.
        if raw.startswith("{") or (key.endswith("_") and "{" in raw[:len(key) + 1]):
            dynamic.add(key)
        else:
            keys.add(key)
    return keys, dynamic


def case_text():
    if not os.path.isdir(CASES):
        return ""
    out = []
    for name in sorted(os.listdir(CASES)):
        if name.endswith(".txt"):
            out.append(open(os.path.join(CASES, name), encoding="utf-8").read())
    return "\n".join(out)


def main():
    keys, dynamic = assertion_keys(GATE)
    cases = case_text()
    uncovered = sorted(k for k in keys if k not in cases)

    print(f"assertions={len(keys)} dynamic={len(dynamic)} "
          f"covered={len(keys) - len(uncovered)} uncovered={len(uncovered)}")
    for k in uncovered:
        print("  no sabotage case names: " + k)
    if dynamic:
        print("  (assembled at runtime, not checkable here: "
              + ", ".join(sorted(dynamic)) + ")")

    if len(uncovered) > BASELINE:
        print(f"assertions-covered=FAIL uncovered={len(uncovered)} "
              f"baseline={BASELINE}")
        return 1
    print("assertions-covered=ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
