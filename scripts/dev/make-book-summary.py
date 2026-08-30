#!/usr/bin/env python3
"""Generate the table of contents mdBook needs, from the docs that exist.

    python3 scripts/dev/make-book-summary.py [docs-dir]

mdBook requires a SUMMARY.md listing every page, and a hand-written one is a
second place to remember: a document added without touching it is simply
absent from the published site, silently. So it is generated, from the
directory, every time the site is built.

Titles come from each file's first heading rather than its filename, because
the filenames are terse and the headings are what somebody browsing wants to
read.
"""
import os
import sys

# Ordered because a table of contents is a reading order, not a directory
# listing. Anything not named here follows, alphabetically.
PREFERRED = [
    "vision.md",
    "architecture.md",
    "software_architecture_specification.md",
    "boot.md",
    "kernel_design.md",
    "memory_management.md",
    "scheduler.md",
    "syscalls.md",
    "ipc.md",
    "filesystem.md",
    "storage_plan.md",
    "networking.md",
    "drivers.md",
    "graphics.md",
    "process_semantics.md",
    "security.md",
    "compatibility.md",
    "userland.md",
    "toolchain.md",
    "development_tools.md",
    "testing_strategy.md",
    "test_automation_spec.md",
    "test_feedback_profiles.md",
]


def title_of(path):
    try:
        with open(path, encoding="utf-8") as fh:
            for line in fh:
                if line.startswith("# "):
                    return line[2:].strip()
    except OSError:
        pass
    base = os.path.basename(path)
    return os.path.splitext(base)[0].replace("_", " ").title()


def main():
    docs = sys.argv[1] if len(sys.argv) > 1 else "docs"
    top = sorted(f for f in os.listdir(docs)
                 if f.endswith(".md") and f != "SUMMARY.md")

    ordered = [f for f in PREFERRED if f in top]
    ordered += [f for f in top if f not in ordered]

    lines = ["# Summary", ""]
    for f in ordered:
        lines.append(f"- [{title_of(os.path.join(docs, f))}]({f})")

    progress = os.path.join(docs, "implementation_progress")
    if os.path.isdir(progress):
        lines.append("")
        lines.append("# Implementation progress")
        lines.append("")
        for f in sorted(os.listdir(progress)):
            if f.endswith(".md"):
                rel = f"implementation_progress/{f}"
                lines.append(f"- [{title_of(os.path.join(docs, rel))}]({rel})")

    # docs/mm/ - the memory management plan, split one concern per file. The
    # README is listed first because it is the index; the rest follow it in a
    # deliberate reading order rather than alphabetically, because "phases"
    # after "observability" is a sequence and P0 before architecture is not.
    mm = os.path.join(docs, "mm")
    if os.path.isdir(mm):
        order = ["README.md", "architecture.md", "observability.md",
                 "maintainability.md", "phases.md", "decisions.md"]
        present = [f for f in order if os.path.exists(os.path.join(mm, f))]
        present += sorted(f for f in os.listdir(mm)
                          if f.endswith(".md") and f not in order)
        if present:
            lines.append("")
            lines.append("# Memory management plan")
            lines.append("")
            for f in present:
                rel = f"mm/{f}"
                lines.append(f"- [{title_of(os.path.join(docs, rel))}]({rel})")

    adrs = os.path.join(docs, "adrs")
    if os.path.isdir(adrs):
        entries = sorted(f for f in os.listdir(adrs) if f.endswith(".md"))
        if entries:
            lines.append("")
            lines.append("# Architecture decisions")
            lines.append("")
            for f in entries:
                rel = f"adrs/{f}"
                lines.append(f"- [{title_of(os.path.join(docs, rel))}]({rel})")

    out = os.path.join(docs, "SUMMARY.md")
    with open(out, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(lines) + "\n")
    print(f"{out}: {len(lines)} lines")
    return 0


if __name__ == "__main__":
    sys.exit(main())
