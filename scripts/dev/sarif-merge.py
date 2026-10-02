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
    """Drop table indexes and make every artifact URI repository-relative."""
    if isinstance(node, dict):
        if "uri" in node and isinstance(node["uri"], str):
            node["uri"] = relative(node["uri"])
            node.pop("index", None)
        node.pop("ruleIndex", None)
        for v in node.values():
            clean(v)
    elif isinstance(node, list):
        for v in node:
            clean(v)


def main():
    src, out = sys.argv[1], sys.argv[2]
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
                results.append(r)
    if merged is None:
        merged = {"version": "2.1.0", "runs": [{"tool": {"driver": {"name": "clang"}}, "results": []}]}
    merged["runs"][0]["tool"]["driver"]["rules"] = [rules[k] for k in sorted(rules, key=str)]
    merged["runs"][0]["results"] = results
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        json.dump(merged, f, indent=1)
    print("sarif-merge: %d results from %d files -> %s" % (
        len(results), len(glob.glob(os.path.join(src, "*.sarif"))), out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
