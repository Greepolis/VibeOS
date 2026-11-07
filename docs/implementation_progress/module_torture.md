# Nightly Module Torture (#201)

Status: Implemented on the coverage branch; acceptance depends on the PR and nightly gates.

The nightly coverage ratchet now has one job per previously uncovered module.
Each job runs under ASan and UBSan, prints replayable seeds, and includes longer
state-transition runs. The short host CTest entries make basic regressions
visible before the nightly budget is spent. The production mutations are run
in PR CI and nightly CI; a build failure is inconclusive, and every mutation
must make the relevant model test fail.

## Models

| Module | Production surface exercised | Independent oracle |
| --- | --- | --- |
| abi | kernel/abi/abi_linux.c syscall-number registry | Linear scan over the published registry rows, compared with the production binary lookup for every exact row and seeded present/absent numbers |
| ipc | kernel/ipc/channel.c, event.c, waitset.c | Plain FIFO array for channel contents and an independent event-selection model for FIFO, reverse, round-robin and priority policies |
| object | kernel/object/handle_table.c | Seeded handle ledger for allocation, quota failures, close, rights, object identity and per-type counts |
| proc | kernel/proc/process.c portable process/thread table | Independent parent, process-state and thread-lifecycle ledger, including child adoption and teardown |
| txn | kernel/txn/journal.c over kernel/fs/blockcache.c | Separate volatile and durable disk images; an injected target-write failure after the commit record is durable, followed by a fresh-cache recovery and byte-for-byte all-target verification |

## Scope notes

- proc is deliberately tested as the portable process model. kernel/proc
  is not the live x86-64 syscall path today; this test does not claim otherwise
  and does not relax the reachability fence documented by docs/core/phases.md.
- The journal test targets the previously untestable state: a committed
  descriptor whose checkpoint was interrupted. It fails one target write
  after the commit record has reached durable storage, discards all cache RAM,
  then requires recovery to write every target and retire the descriptor.
  This avoids the old withdrawn torture's invalid assumption that a completed
  commit() leaves a replayable transaction behind.
- The module annotation is attached to each real top-level nightly job. The
  coverage checker continues to reject annotations in steps, stale module
  names, and a baseline regression.
