#!/usr/bin/env python3
"""Merge the SARIF files clang's analyzer writes, one a source file, into one.

    scripts/dev/sarif-merge.py <directory of .sarif> <output.sarif>

Code scanning takes one run per tool and category; clang writes one file per
translation unit, each a run of its own. This makes a single run of them: the
results end to end, the rules once each, and every location named by its path
from the repository's root - clang writes absolute file:// URIs and indexes
into a per-file table of artifacts and rules, and both the indexes and the
tables are dropped, since a path and a rule id say the same thing without
needing to be renumbered.
"""
import glob
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def relative(uri):
    if uri.startswith("file://"):
        path = uri[len("file://"):]
        if path.startswith(ROOT + "/"):
            return path[len(ROOT) + 1:]
    return uri


def clean(node):
    """Drop table indexes, make every artifact URI repository-relative, and
    give every region lines code scanning accepts. clang writes a code flow's
    region with endLine 0 when the step has no extent, and the upload refuses
    the whole file for it - which is how the first finding in months failed to
    reach code scanning at all (2026-10-07)."""
    if isinstance(node, dict):
        if "uri" in node and isinstance(node["uri"], str):
            node["uri"] = relative(node["uri"])
            node.pop("index", None)
        node.pop("ruleIndex", None)
        region = node.get("region")
        if isinstance(region, dict):
            start = region.get("startLine")
            if not isinstance(start, int) or start < 1:
                region["startLine"] = start = 1
            if "endLine" in region and (not isinstance(region["endLine"], int) or region["endLine"] < start):
                region["endLine"] = start
        for v in node.values():
            clean(v)
    elif isinstance(node, list):
        for v in node:
            clean(v)


def known_findings():
    """(file, checker) pairs clang-analyze-known.txt lists, with how many of each."""
    known = {}
    path = os.path.join(ROOT, "scripts", "dev", "clang-analyze-known.txt")
    if os.path.isfile(path):
        for line in open(path, encoding="utf-8"):
            parts = line.strip().split(": ", 2)
            if len(parts) == 3 and not line.startswith("#"):
                known[(parts[0], parts[1])] = known.get((parts[0], parts[1]), 0) + 1
    return known


def main():
    src, out = sys.argv[1], sys.argv[2]
    known = known_findings()
    merged = None
    rules = {}
    results = []
    for path in sorted(glob.glob(os.path.join(src, "*.sarif"))):
        try:
            doc = json.load(open(path, encoding="utf-8"))
        except (OSError, ValueError):
            continue
        for run in doc.get("runs", []):
            if merged is None:
                merged = {"$schema": doc.get("$schema"), "version": doc.get("version", "2.1.0"),
                          "runs": [{"tool": run["tool"], "results": [], "columnKind": run.get("columnKind", "unicodeCodePoints")}]}
            for r in run.get("tool", {}).get("driver", {}).get("rules", []):
                rules.setdefault(r.get("id"), r)
            for r in run.get("results", []):
                clean(r)
                # A finding read and judged not a defect is left out here as it
                # is left out of the script's verdict: one that is written down
                # with its reason should not also sit in code scanning as open.
                try:
                    key = (r["locations"][0]["physicalLocation"]["artifactLocation"]["uri"], r.get("ruleId"))
                except (KeyError, IndexError):
                    key = None
                if key in known and known[key] > 0:
                    known[key] -= 1
                    continue
                results.append(r)
    if merged is None:
        # Nothing was read: the analyzer did not run, or wrote nothing. An
        # empty run uploaded would read as "no findings" and close every open
        # alert (external review, 2026-10-07) - so it is a failure instead.
        print("sarif-merge: no SARIF run in %s - the analyzer wrote nothing; refusing an empty upload" % src)
        return 1
    merged["runs"][0]["tool"]["driver"]["rules"] = [rules[k] for k in sorted(rules, key=str)]
    merged["runs"][0]["results"] = results
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        json.dump(merged, f, indent=1)
    print("sarif-merge: %d results from %d files -> %s" % (
        len(results), len(glob.glob(os.path.join(src, "*.sarif"))), out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
