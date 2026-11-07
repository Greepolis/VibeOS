#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Which kernel modules have no intensive nightly test?

Every module gets one. The rule lives here as well as in project guidance
because a rule nobody can check is a rule that decays.

An intensive test is long, randomised, and checked against something other
than the code under test: many seeds against a reference model, a soak whose
length is the point, or a fuzzer. The memory manager compares every frame's
owner count against a plain-array model; the scheduler does the same for its
picker. Timekeeping has an independent event-count model, boundary probes and
production-source mutation checks. The ABI registry is compared with a linear
lookup oracle; IPC channels and waitset policies with queue/selection models;
object handles with a seeded ledger; the portable process/thread model with a
lifecycle ledger; and journal recovery with separate durable and volatile disk
images plus an interrupted-checkpoint replay.

The reason is that a subsystem asked whether it is correct answers from the
numbers it used to decide, so self-consistent defects survive. The model must
use a different representation and the test must be shown to fail when a
production invariant is broken.

A job declares coverage with a '# module: <names>' comment at job level in
.github/workflows/nightly.yml. A comment rather than a YAML key because GitHub
rejects unknown keys; a separate manifest would be a second source of truth.
Step-level annotations do not count.

The number may only go down. Eleven of thirteen modules had nothing when this
rule was introduced. The earlier journal tortures were removed after their
sabotages were caught on zero of thirty seeds; see
scripts/dev/cases/io-journal.txt. The replacement txn model creates a
committed-but-uncheckpointed transaction by failing a target write after the
commit record is durable, then reboots with an empty cache and requires a full
replay. A test that cannot fail is not evidence of coverage, and lowering the
baseline requires a test that discriminates.
"""

import argparse
import os
import re
import sys

# All 16 kernel modules now have a qualifying intensive nightly test.
# History: 11 of 13 modules were uncovered at introduction. Raised from 7 to
# 8 in I5c as an explicit debt decision, then lowered from 8 to 7 in L1 after
# kernel/fs gained its model torture; from 7 to 6 after kernel/time gained its
# independent model and production mutations; from 6 to 5 after kernel/exec
# gained its seeded parser fuzzers (#203); and from 5 to 0 after abi, ipc,
# object, proc and txn gained independent models, replayable nightly seeds and
# production-source mutation checks (#201). The baseline may only go down.
# The withdrawn journal tortures remain deliberately uncounted: their
# recovery/flush sabotages were caught on zero of thirty seeds and their second
# attempt could not produce a committed-but-uncheckpointed state. The new txn
# model does produce that state by failing a target write after durable commit,
# then discarding cache RAM and requiring a complete replay.
# The number may only go down when a new test proves it can detect a defect.
BASELINE = 0
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
NIGHTLY = os.path.join(ROOT, ".github", "workflows", "nightly.yml")
KERNEL = os.path.join(ROOT, "kernel")


def modules(kernel_dir=KERNEL):
    return sorted(name for name in os.listdir(kernel_dir)
                  if os.path.isdir(os.path.join(kernel_dir, name)))


def declared_modules(nightly_path=NIGHTLY):
    """Read module annotations only from actual top-level workflow jobs."""
    if not os.path.isfile(nightly_path):
        raise FileNotFoundError(nightly_path)
    result = set()
    in_jobs = False
    current_job = False
    with open(nightly_path, encoding="utf-8") as stream:
        for line in stream:
            if re.match(r"^jobs:\s*$", line):
                in_jobs, current_job = True, False
                continue
            if in_jobs and re.match(r"^[^ \s#][^:]*:\s*(?:#.*)?$", line):
                in_jobs, current_job = False, False
            if not in_jobs:
                continue
            if re.match(r"^  [A-Za-z0-9_-]+:\s*(?:#.*)?$", line):
                current_job = True
                continue
            if current_job and re.match(r"^ {4}#\s*module:", line):
                value = line.split("module:", 1)[1].strip()
                result.update(value.split())
    return result


def analyze(kernel_dir=KERNEL, nightly_path=NIGHTLY, baseline=BASELINE):
    present = set(modules(kernel_dir))
    declared = declared_modules(nightly_path)
    missing = sorted(present - declared)
    stale = sorted(declared - present)
    return {
        "modules": sorted(present),
        "declared": sorted(declared),
        "missing": missing,
        "stale": stale,
        "baseline": baseline,
        "ok": not stale and len(missing) <= baseline,
    }


def render(report):
    lines = [
        "Nightly intensive module coverage",
        "modules={modules} covered={covered} uncovered={uncovered} baseline={baseline}".format(
            modules=len(report["modules"]),
            covered=len(report["modules"]) - len(report["missing"]),
            uncovered=len(report["missing"]),
            baseline=report["baseline"]),
    ]
    lines.extend("  uncovered: kernel/" + name for name in report["missing"])
    lines.extend("  stale declaration: " + name for name in report["stale"])
    lines.append("nightly-coverage=" + ("ok" if report["ok"] else "FAIL"))
    return "\n".join(lines) + "\n"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", help="also write the exact report to this path")
    args = parser.parse_args(argv)
    try:
        report = analyze()
    except (OSError, UnicodeError) as error:
        print("nightly-coverage=ERROR " + str(error))
        return 2
    output = render(report)
    print(output, end="")
    if args.output:
        with open(args.output, "w", encoding="utf-8") as stream:
            stream.write(output)
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
