#!/usr/bin/env python3
"""Break a thing on purpose and confirm a named test goes red.

    scripts/dev/sabotage.py <source-file> <cases-file> [build-dir]

A test that has never failed has not been shown to work. This project has now
produced several tests that passed while proving nothing about the check they
named - a hole test that read a block which happened to be zeroed, a checksum
test whose corruption was rejected for a different reason, a bounds test whose
input failed an earlier check. Every one was found by sabotaging the code and
noticing the suite stayed green.

Cases file format - block-structured, because the interesting anchors span
several lines and a line-oriented format silently mangles them:

    ### short label
    --- old
    the exact text to remove
    +++ new
    the text to put in its place, may be empty

The backup is written next to the source, not to /tmp. Two sabotage runs were
killed by a timeout while a guard was removed, and both times /tmp had been
cleared by a WSL restart, leaving the source half-restored with nothing to
restore from. Removing some guards hangs the test suite rather than failing it,
so being killed by a timeout is not a hypothetical.
"""

import os
import subprocess
import sys


def parse_cases(text):
    cases, label, old, new, mode = [], None, [], [], None
    for line in text.splitlines():
        if line.startswith('#') and not line.startswith('### '):
            # A comment, anywhere, including after the last case.
            #
            # Every case file here ends with a paragraph explaining what each
            # case actually caught, and those lines were being appended to the
            # final case's replacement text - so the last case of every file
            # was substituting prose into C. It came back as a compile failure,
            # which with a custom verifier scored as *red*: the case looked
            # like it was working, and was testing nothing at all. That is the
            # exact failure this whole tool exists to catch, in the tool.
            #
            # A replacement that genuinely needs a preprocessor directive can
            # indent it; nothing here does.
            continue
        if line.startswith('### '):
            if label is not None:
                cases.append((label, '\n'.join(old), '\n'.join(new)))
            label, old, new, mode = line[4:].strip(), [], [], None
        elif line.strip() == '--- old':
            mode = 'old'
        elif line.strip() == '+++ new':
            mode = 'new'
        elif mode == 'old':
            old.append(line)
        elif mode == 'new':
            new.append(line)
    if label is not None:
        cases.append((label, '\n'.join(old), '\n'.join(new)))
    return cases


def build_and_test(build_dir, verify=None):
    """Returns 'pass', 'red', 'hung' or 'nobuild'.

    `verify` runs a shell command instead of the kernel test binary, for guards
    that live outside the C code - a generated VM appliance, say, whose checks
    are a script over build artifacts. The verdicts mean the same thing either
    way: non-zero is red.
    """
    if verify is None and subprocess.run(["cmake", "--build", build_dir, "-j4"],
                                         capture_output=True).returncode != 0:
        return 'nobuild', ''
    try:
        if verify is not None:
            r = subprocess.run(verify, shell=True, capture_output=True,
                               text=True, timeout=300)
            if r.returncode == 0:
                return 'pass', r.stdout
            # A verifier that could not build has not scored the case. Without
            # this, a sabotage that does not compile is indistinguishable from
            # one the tests caught - and "every case red" is indistinguishable
            # from "every case working", which is how eight cases once passed
            # having proved nothing.
            if 'BUILD_FAILED' in r.stdout or 'BUILD_FAILED' in r.stderr:
                return 'nobuild', ''
            return 'red', r.stdout.strip()
        r = subprocess.run(["./%s/vibeos_kernel_tests" % build_dir],
                           capture_output=True, text=True, timeout=120)
    except subprocess.TimeoutExpired:
        return 'hung', ''
    if r.returncode == 0:
        return 'pass', r.stdout
    fails = [l for l in r.stdout.splitlines() if l.startswith('FAIL')]
    return 'red', '\n'.join(fails[:3])


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    src, cases_path = sys.argv[1], sys.argv[2]
    build_dir = sys.argv[3] if len(sys.argv) > 3 else "build-gcc-Release"
    verify = os.environ.get("SABOTAGE_VERIFY")

    with open(cases_path, encoding='utf-8') as fh:
        cases = parse_cases(fh.read())
    if not cases:
        print("no cases parsed - check the ### / --- old / +++ new markers")
        return 2

    with open(src, encoding='utf-8') as fh:
        original = fh.read()
    backup = src + '.sabotage-backup'
    with open(backup, 'w', encoding='utf-8', newline='') as fh:
        fh.write(original)

    weak = []
    try:
        for label, old, new in cases:
            print("--- %s" % label)
            if old not in original:
                print("    anchor not found; this case tests nothing")
                weak.append(label)
                continue
            with open(src, 'w', encoding='utf-8', newline='') as fh:
                fh.write(original.replace(old, new, 1))
            verdict, detail = build_and_test(build_dir, verify)
            if verdict == 'red':
                for line in detail.splitlines():
                    print("    " + line)
            elif verdict == 'hung':
                print("    HUNG - removing this guard hangs the suite rather "
                      "than failing it, which is itself the reason it exists")
            elif verdict == 'nobuild':
                print("    did not compile; this case proves nothing")
                weak.append(label)
            else:
                print("    NOT RED - no test covers this")
                weak.append(label)
            with open(src, 'w', encoding='utf-8', newline='') as fh:
                fh.write(original)
    finally:
        with open(src, 'w', encoding='utf-8', newline='') as fh:
            fh.write(original)
        os.remove(backup)

    verdict, _ = build_and_test(build_dir)
    print("--- restored: %s" % verdict)
    if weak:
        print("cases that proved nothing: " + ", ".join(weak))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
