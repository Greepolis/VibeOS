#!/usr/bin/env python3
"""What a memory soak run measured, and whether it is acceptable.

One definition, used by scripts/dev/soak.sh and by the nightly. Two copies of
"what the soak asserts" is how two things come to disagree about the same fact,
which is a family of defect this project has spent several investigations on.

## The property

The frame loss across a boot must be a *fixed cost*, not a per-round leak.

Those are the same number in a single measurement and have opposite
consequences on a machine that stays up: twenty-six frames lost once at startup
is a wart, twenty-six frames lost per hundred rounds is a machine that dies
overnight. Only the comparison between two round counts separates them, which
is why nothing here reports a verdict from one log.

## What it refuses to conclude

A log where the stress service never printed its completion line gets no
verdict at all. The counts are still there and still plausible - two numbers
that partition - but they were sampled from a machine that was still forking,
and a plausible number from the wrong moment is worse than a missing one. The
kernel asserts the ordering too; this is the second reader of the same fact.
"""

import re
import sys


def read(path):
    text = open(path, "rb").read().decode("utf-8", "replace")
    out = {"path": path}

    m = re.search(r"STRESS_OK rounds=(\d+)", text)
    out["rounds"] = int(m.group(1)) if m else None

    s = re.search(r"FRAMES_AT_USERLAND_START=0x([0-9a-f]{16})", text)
    d = re.search(r"FRAMES_AT_USERLAND_DONE=0x([0-9a-f]{16}) "
                  r"cache_resident=0x([0-9a-f]{16})", text)
    if s and d:
        out["lost"] = int(s.group(1), 16) - (int(d.group(1), 16) +
                                             int(d.group(2), 16))
    else:
        out["lost"] = None

    # The ordering that makes the counts mean anything.
    out["premise"] = None
    if out["rounds"] is not None and d is not None:
        out["premise"] = text.index("STRESS_OK") < text.index(
            "FRAMES_AT_USERLAND_DONE")

    sw = re.search(r"\[MM\] SWAP slots=0x([0-9a-f]{16}) allocated=0x([0-9a-f]{16}) "
                   r"peak=0x([0-9a-f]{16})", text)
    if sw:
        out["swap_slots"] = int(sw.group(1), 16)
        out["swap_peak"] = int(sw.group(3), 16)
    else:
        out["swap_slots"] = out["swap_peak"] = None

    rc = re.search(r"\[MM\] RECLAIM scans=0x([0-9a-f]{16}) "
                   r"freed_clean=0x([0-9a-f]{16}) freed_anon=0x([0-9a-f]{16})",
                   text)
    out["freed_anon"] = int(rc.group(3), 16) if rc else None

    out["roundtrip"] = ("[MM] SWAP_ROUNDTRIP swap round trip OK" in text)

    # The counters that are defects rather than descriptions.
    out["mustbezero"] = []
    for line, names in (
        (r"\[MM\] SWAP MUSTBEZERO double_free=0x([0-9a-f]{16}) "
         r"out_of_range=0x([0-9a-f]{16}) slot_leaked=0x([0-9a-f]{16})",
         ("swap_double_free", "swap_out_of_range", "swap_slot_leaked")),
        (r"\[CONSOLE\] MUSTBEZERO bad_unlocks=0x([0-9a-f]{16}) "
         r"stuck=0x([0-9a-f]{16})",
         ("console_bad_unlocks", "console_stuck")),
    ):
        m = re.search(line, text)
        if m is None:
            out["mustbezero"].append((names[0].split("_")[0] + "_line", "missing"))
            continue
        for name, value in zip(names, m.groups()):
            if int(value, 16) != 0:
                out["mustbezero"].append((name, int(value, 16)))
    return out


def describe(r):
    return (f"{r['path']}: rounds={r['rounds']} lost={r['lost']} "
            f"swap_slots={r['swap_slots']} swap_peak={r['swap_peak']} "
            f"freed_anon={r['freed_anon']} "
            f"roundtrip={'OK' if r['roundtrip'] else 'NOT-OK'}")


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    # How much the loss may grow between the low and the high round count.
    #
    # Sixty-four, the same ceiling the boot gate uses for a single boot, and
    # for the same reason: it is there to stop the number *growing*, not to
    # bless it. A per-fork leak at twelve thousand rounds would be in the
    # thousands, so this catches the defect it exists for with room to spare.
    ceiling = 64
    for a in sys.argv[1:]:
        if a.startswith("--ceiling="):
            ceiling = int(a.split("=", 1)[1])

    if not args:
        print("usage: soak-report.py <serial.log> [<serial.log> ...] "
              "[--ceiling=N]", file=sys.stderr)
        return 2

    runs = [read(p) for p in args]
    bad = []
    for r in runs:
        print(describe(r))
        if r["lost"] is None:
            bad.append(f"{r['path']}: no frame accounting in the log")
        if r["rounds"] is None:
            bad.append(f"{r['path']}: the stress run did not finish")
        elif r["premise"] is False:
            bad.append(f"{r['path']}: counts sampled before userland finished")
        if not r["roundtrip"]:
            bad.append(f"{r['path']}: the swap round trip did not report OK")
        if r["swap_slots"] in (None, 0):
            bad.append(f"{r['path']}: no swap area was configured")
        for name, value in r["mustbezero"]:
            bad.append(f"{r['path']}: {name}={value}")

    # The comparison, which is the actual test.
    usable = [r for r in runs if r["lost"] is not None and r["rounds"]]
    if len(usable) >= 2:
        lo = min(usable, key=lambda r: r["rounds"])
        hi = max(usable, key=lambda r: r["rounds"])
        if hi["rounds"] > lo["rounds"]:
            grew = hi["lost"] - lo["lost"]
            print(f"comparison: {lo['rounds']} rounds lost {lo['lost']}, "
                  f"{hi['rounds']} rounds lost {hi['lost']}, grew by {grew}")
            if grew > ceiling:
                bad.append(f"the loss grows with the work: +{grew} frames from "
                           f"{lo['rounds']} to {hi['rounds']} rounds "
                           f"(ceiling {ceiling}) - that is a leak, not a "
                           f"fixed cost")
        else:
            print("comparison: both logs ran the same round count; "
                  "no fixed-cost-versus-leak verdict is possible")
    else:
        print("comparison: fewer than two usable logs; "
              "no fixed-cost-versus-leak verdict is possible")

    for b in bad:
        print("FAIL: " + b)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
