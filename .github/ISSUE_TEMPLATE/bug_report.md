---
name: Bug report
about: Something behaves differently from what it claims to do
title: ''
labels: bug
assignees: ''
---

## What happened

<!-- What you observed. Paste the serial log rather than describing it: this
     system's failures are usually visible in it and usually surprising. -->

## What you expected

## How to reproduce

- [ ] Which build: `gcc` / `clang`, `Debug` / `Release`
- [ ] Which gate or command
- [ ] Does it happen every time, or some of the time?

If it is intermittent, say roughly how often. Several of the worst bugs in this
kernel reproduced in about one boot out of three, and a single clean run tells
you almost nothing about them - `scripts/dev/repeat-boot.sh` exists for this.

## Serial log

<!-- The tail is usually enough; the whole thing if the failure is early. -->

```
```

## If the guest hung rather than crashed

A hang produces no output, so the log ends before the interesting part. Run
`scripts/dev/catch-hang.py <build-dir>` and paste what it reports - it asks the
QEMU monitor where every core is. Note that its symbol names are the nearest
preceding symbol, so on this codebase they can name the wrong function; treat
them as a hint, not an answer.
