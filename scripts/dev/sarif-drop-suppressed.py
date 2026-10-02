#!/usr/bin/env python3
"""Leave out of a SARIF file the results its own tool marked as suppressed.

    scripts/dev/sarif-drop-suppressed.py <file.sarif>

Semgrep honours a `# nosemgrep` comment in its verdict and still writes the
finding to SARIF, with a `suppressions` entry beside it. Code scanning does not
read that entry: three findings marked in the source, each with its reason,
stayed open there (alerts 240-242). The file is rewritten in place without
them, and the count is printed so that a run which dropped something says so.
"""
import json
import sys


def main():
    path = sys.argv[1]
    with open(path, encoding="utf-8") as f:
        doc = json.load(f)
    dropped = 0
    for run in doc.get("runs", []):
        kept = [r for r in run.get("results", []) if not r.get("suppressions")]
        dropped += len(run.get("results", [])) - len(kept)
        run["results"] = kept
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(doc, f, indent=1)
    print("sarif-drop-suppressed: %d suppressed results left out of %s" % (dropped, path))
    return 0


if __name__ == "__main__":
    sys.exit(main())
