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
| abi | `kernel/abi/abi_linux.c` immutable syscall-number metadata registry (`vibeos_linux_syscall*`) | Linear scan over the published metadata rows, compared with the production binary lookup for every exact row and seeded present/absent numbers. This does **not** exercise the separate dynamic ABI row-registration/lookup API (`vibeos_abi_linux_register`, `vibeos_abi_linux_row`, or `linux_lookup`). |
| ipc | `kernel/ipc/channel.c`, `event.c`, `waitset.c` | Plain FIFO array for channel contents; an independently maintained `model_signaled[]` state updated from signal/clear operations and checked through the event API; and separate expected ordering for FIFO, reverse, round-robin and priority waitset policies. Targeted channel and waitset mutations are tested independently. |
| object | `kernel/object/handle_table.c` | Seeded handle ledger for allocation, quota failures, close, rights, object identity and per-type counts. Provenance/revocation lineage and all lifecycle event semantics are outside this torture's current scope. |
| proc | `kernel/proc/process.c` portable process/thread table | Independent parent, process-state and thread-lifecycle ledger, including child adoption and teardown. It does not claim coverage of token/permission, handle-transfer, audit-ring, or all process-control APIs. |
| txn | `kernel/txn/journal.c` over `kernel/fs/blockcache.c` | Separate volatile and durable disk images; an injected target-write failure after the commit record is durable, followed by a fresh-cache recovery and byte-for-byte all-target verification. Pre-commit crashes, malformed descriptors/checksums, recovery I/O failures, and capacity/duplicate-target boundaries are not covered by this scenario. |

## Scope notes

- proc is deliberately tested as the portable process model. kernel/proc
  is not the live x86-64 syscall path today; this test does not claim otherwise
  and does not relax the reachability fence documented by docs/core/phases.md.
- ABI coverage is intentionally limited to the immutable syscall metadata
  registry; dynamic ABI row registration and dispatch lookup need a separate
  model and mutation gate before that surface can be called covered.
- The journal test targets the previously untestable state: a committed
  descriptor whose checkpoint was interrupted. It fails one target write
  after the commit record has reached durable storage, discards all cache RAM,
  then requires recovery to write every target and retire the descriptor.
  This avoids the old withdrawn torture's invalid assumption that a completed
  commit() leaves a replayable transaction behind.
- The module annotation is attached to each real top-level nightly job. The
  coverage checker continues to reject annotations in steps, stale module
  names, and a baseline regression.
