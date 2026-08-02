---
name: Feature or capability
about: Something the system should be able to do and cannot
title: ''
labels: enhancement
assignees: ''
---

## What should it be able to do

## Why that matters now

<!-- What becomes possible with it that is not possible without it. The
     project has a long list of things it does not do yet; what moves is
     whatever unblocks something real. -->

## How would we know it works

<!-- The most useful part. A capability with no way to demonstrate it tends to
     get built, declared done, and quietly not work. Prefer something a gate
     can check: a program that runs, a value that arrives, output that appears.

     For Linux compatibility specifically, run the real binary under strace
     first (`scripts/dev/trace-linux-binary.sh`) - the syscalls a program needs
     are a property of that program, not something to reason out. -->

## Related area

- [ ] Boot and firmware handoff
- [ ] Memory and paging
- [ ] Scheduler and processes
- [ ] Linux system-call compatibility
- [ ] Filesystem and storage
- [ ] Networking
- [ ] Build, CI, or tooling
