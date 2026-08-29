#!/usr/bin/env python3
"""Collect crashes from a serial log into one GitHub issue.

The idea is the one every desktop OS has: when something dies, offer to send a
report, and keep the reports together so a pattern is visible. Three rules make
that useful here rather than noisy.

**The guest never sends anything.** VibeOS does not phone home. It writes a
crash record to its console and stops there; this runs on the host, on an
explicit command, on a log somebody chose to hand it. An operating system that
uploads on its own is a different kind of thing from one with a bug reporter,
and the difference is consent.

**One issue, never more.** Every report is a comment on a single collector
issue, found by its exact title. This script will not open a second one.

**Deduplicated by signature.** A crash is identified by the program, the
vector and the faulting instruction. A recurring crash edits its existing
comment - bumping a count and the last-seen date - instead of adding another.
That is the difference between a collector and a flood, and it is why the
signature is in the comment body rather than only in a hash somewhere.

Nothing is posted without --post. Without it the script prints exactly what it
would send, which is also how you check what a dump contains before it becomes
public: a crash record carries register values and a slice of the process
stack.

    python scripts/dev/report-crash.py qemu-cli-serial.log
    python scripts/dev/report-crash.py qemu-cli-serial.log --post

Authentication is a token in GITHUB_TOKEN or GH_TOKEN. In CI that is provided;
locally it needs a token with `issues: write`.
"""

import argparse
import hashlib
import json
import os
import re
import sys
import urllib.error
import urllib.request
from datetime import datetime, timezone

COLLECTOR_TITLE = "Crash collector: automated reports from the boot gate"
API = "https://api.github.com"

# A marker no human would type, so a comment can be found again without
# depending on how it is worded.
SIG_PREFIX = "<!-- vibeos-crash-signature: "

# Programs whose crashing *is* the test. svc-crash dereferences null on every
# single boot to prove the kernel kills the task instead of halting, so
# reporting it would put one guaranteed entry in the collector per run and
# teach everybody to skim past the newest comment - which is the failure mode
# a shared collector has to avoid above all others.
#
# Kept as a list of program names rather than signatures: a deliberate crash
# that moves to a different instruction is still deliberate.
EXPECTED_CRASHERS = (
    "SVC_CRSH.ELF",
)


def is_expected(crash):
    return any(name in crash["exe"] for name in EXPECTED_CRASHERS)



def parse_crashes(text):
    """Pull the crash records out of a serial log.

    The kernel prints a one-line notice when it records a fault and a full dump
    when `crash` is typed at the console. Both are matched: the notice always
    happens, the dump only if somebody asked.
    """
    crashes = []
    for m in re.finditer(
            r"\[CRASH\] recorded pid=0x([0-9a-f]+) sig=0x([0-9a-f]+) exe=(\S+)", text):
        crashes.append({
            "pid": int(m.group(1), 16),
            "sig": int(m.group(2), 16),
            "exe": m.group(3),
            "vector": None,
            "rip": None,
            "dump": "",
        })

    # The full dump, if the console was asked for one. Attach it to the crash
    # it belongs to by executable name.
    for m in re.finditer(
            r"\[CRASH\] total=0x[0-9a-f]+ pid=0x([0-9a-f]+) sig=0x[0-9a-f]+ exe=(\S+)\n"
            r"(.*?)\[CRASH\] end", text, re.S):
        exe, body = m.group(2), m.group(3)
        vec = re.search(r"vector=0x([0-9a-f]+)", body)
        rip = re.search(r"rip=0x([0-9a-f]+)", body)
        for c in crashes:
            if c["exe"] == exe and not c["dump"]:
                c["dump"] = body.strip()
                c["vector"] = int(vec.group(1), 16) if vec else None
                c["rip"] = int(rip.group(1), 16) if rip else None
                break

    # A kernel panic is not a process crash, and is worth collecting too.
    for m in re.finditer(r"FATAL: ([^\n]+)", text):
        crashes.append({"pid": 0, "sig": 0, "exe": "(kernel)",
                        "vector": None, "rip": None,
                        "dump": "panic: " + m.group(1).strip()})
    return crashes


def signature(crash):
    """What makes two crashes the same crash.

    The program, the vector and the faulting instruction. Deliberately not the
    pid, the timestamp or the stack contents - those differ every run, and a
    signature that includes them turns the collector into a firehose.
    """
    key = "%s|%s|%s" % (crash["exe"], crash["vector"], crash["rip"])
    return hashlib.sha256(key.encode()).hexdigest()[:12]


def api(method, path, token, body=None):
    req = urllib.request.Request(API + path, method=method)
    req.add_header("Authorization", "Bearer " + token)
    req.add_header("Accept", "application/vnd.github+json")
    req.add_header("User-Agent", "vibeos-crash-reporter")
    data = None
    if body is not None:
        data = json.dumps(body).encode()
        req.add_header("Content-Type", "application/json")
    with urllib.request.urlopen(req, data) as resp:
        return json.loads(resp.read().decode() or "null")


def find_collector(repo, token):
    issues = api("GET", f"/repos/{repo}/issues?state=all&per_page=100", token)
    for issue in issues or []:
        if issue.get("title") == COLLECTOR_TITLE:
            return issue["number"]
    return None


def render(crash, sig, count, first_seen, now):
    lines = [
        SIG_PREFIX + sig + " -->",
        f"### `{crash['exe']}` — signal {crash['sig']}",
        "",
        f"| | |",
        f"| --- | --- |",
        f"| signature | `{sig}` |",
        f"| times seen | **{count}** |",
        f"| first seen | {first_seen} |",
        f"| last seen | {now} |",
    ]
    if crash["vector"] is not None:
        lines.append(f"| vector | `0x{crash['vector']:x}` |")
    if crash["rip"] is not None:
        lines.append(f"| rip | `0x{crash['rip']:x}` |")
    if crash["dump"]:
        lines += ["", "<details><summary>crash record</summary>", "",
                  "```", crash["dump"], "```", "", "</details>"]
    lines += ["", "_Posted by `scripts/dev/report-crash.py`. The guest does "
              "not send anything; this ran on a host, on a log somebody chose "
              "to hand it._"]
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log", nargs="+", help="serial log(s) to read")
    ap.add_argument("--repo", default=os.environ.get("GITHUB_REPOSITORY",
                                                     "Greepolis/VibeOS"))
    ap.add_argument("--post", action="store_true",
                    help="actually send. Without this, nothing leaves the machine.")
    ap.add_argument("--include-expected", action="store_true",
                    help="report the crashes the boot provokes on purpose too "
                         "(svc-crash faults every run by design)")
    ap.add_argument("--max-new", type=int, default=3,
                    help="refuse to open more than this many new reports in one "
                         "run. A run that wants to say twenty new things is "
                         "usually one bug seen twenty ways.")
    args = ap.parse_args()

    crashes = []
    for path in args.log:
        try:
            text = open(path, encoding="utf-8", errors="replace").read().replace("\r", "")
        except OSError as exc:
            print(f"[CRASH-REPORT] cannot read {path}: {exc}")
            continue
        crashes += parse_crashes(text)

    if not args.include_expected:
        before = len(crashes)
        crashes = [c for c in crashes if not is_expected(c)]
        skipped = before - len(crashes)
        if skipped:
            print(f"[CRASH-REPORT] ignoring {skipped} crash(es) the boot "
                  "provokes deliberately (--include-expected to see them)")

    # Collapse repeats within this run before talking to anyone.
    by_sig = {}
    for c in crashes:
        by_sig.setdefault(signature(c), []).append(c)
    if not by_sig:
        print("[CRASH-REPORT] no crash records in those logs")
        return 0

    now = datetime.now(timezone.utc).strftime("%Y-%m-%d")
    print(f"[CRASH-REPORT] {len(by_sig)} distinct crash(es) in "
          f"{len(args.log)} log(s)")

    if not args.post:
        for sig, group in by_sig.items():
            print("-" * 60)
            print(render(group[0], sig, len(group), now, now))
        print("-" * 60)
        print("[CRASH-REPORT] dry run: nothing sent. Re-run with --post to "
              "publish to " + args.repo)
        return 0

    token = os.environ.get("GITHUB_TOKEN") or os.environ.get("GH_TOKEN")
    if not token:
        print("[CRASH-REPORT] no GITHUB_TOKEN/GH_TOKEN; refusing to post")
        return 2

    number = find_collector(args.repo, token)
    if number is None:
        issue = api("POST", f"/repos/{args.repo}/issues", token, {
            "title": COLLECTOR_TITLE,
            "body": ("Automated crash reports from the boot gate, one comment "
                     "per distinct crash, deduplicated by signature. A "
                     "recurring crash updates its own comment rather than "
                     "adding another.\n\nThe guest does not send anything: "
                     "reports are posted from a host by "
                     "`scripts/dev/report-crash.py`, on an explicit command."),
        })
        number = issue["number"]
        print(f"[CRASH-REPORT] opened the collector issue #{number}")

    comments = api("GET", f"/repos/{args.repo}/issues/{number}/comments?per_page=100",
                   token) or []
    existing = {}
    for c in comments:
        m = re.search(re.escape(SIG_PREFIX) + r"([0-9a-f]+) -->", c.get("body", ""))
        if m:
            existing[m.group(1)] = c

    new = 0
    for sig, group in by_sig.items():
        if sig in existing:
            old = existing[sig]
            seen = re.search(r"\| times seen \| \*\*(\d+)\*\* \|", old["body"])
            first = re.search(r"\| first seen \| ([0-9-]+) \|", old["body"])
            count = (int(seen.group(1)) if seen else 1) + len(group)
            api("PATCH", f"/repos/{args.repo}/issues/comments/{old['id']}", token,
                {"body": render(group[0], sig, count,
                                first.group(1) if first else now, now)})
            print(f"[CRASH-REPORT] {sig}: seen again ({count}), comment updated")
            continue
        if new >= args.max_new:
            print(f"[CRASH-REPORT] {sig}: new, but the cap of {args.max_new} "
                  "new reports for one run is reached - not posted")
            continue
        api("POST", f"/repos/{args.repo}/issues/{number}/comments", token,
            {"body": render(group[0], sig, len(group), now, now)})
        new += 1
        print(f"[CRASH-REPORT] {sig}: new, posted")

    print(f"[CRASH-REPORT] done: issue #{number}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
