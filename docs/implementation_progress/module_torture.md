# Nightly Module Torture (#201)

Status: IPC, OBJECT, PROC and TXN have qualifying intensive models. ABI
remains in the nightly coverage debt until its model covers the module rather
than part of it.

The coverage checker has a baseline of one uncovered module. The baseline
is a hard upper bound, not a claim that every module has a qualifying test.
Each module removed from the debt must have an independent model and a
production mutation that demonstrates the model can detect a meaningful
regression.

## Covered models

| Module | Production surface exercised | Independent oracle |
| --- | --- | --- |
| ipc | `kernel/ipc/channel.c`, `event.c`, `waitset.c` | A plain FIFO array for channel contents; independently tracked event state; and separately derived expected order for FIFO, reverse, round-robin and priority waitset policies. The oracle checks behaviour through public operations and does not inspect `channel.count` or other implementation state. |
| object | `kernel/object/handle_table.c` | A seeded handle ledger for allocation, close, quota, rights, object identity and per-type counts. The quota varies by seed; a balanced allocation/close stream avoids permanent saturation; every historical handle is checked at each sample point, including every live handle and every closed-id rejection. |
| proc | `kernel/proc/process.c` | An independent PID/parent/thread lifecycle ledger; seeded spawn/terminate/reap cycles, child reparenting, tombstone visibility, process/thread counters, fixed-capacity exhaustion and slot reuse with monotonic PIDs. |
| txn | `kernel/txn/journal.c` | A durable-sector model that cuts power after every prefix of device writes in multiple persistence orders, remounts the real journal, and requires the commit record to select exactly all-old or all-new targets. An adversarial descriptor-retirement-first order detects removal of the checkpoint flush. The debt row asked for the pre-commit (phase-one) flush to be caught instead; that removal is not observable - the commit record carries the log blocks' checksum, so a commit that reaches the disk before its blocks fails it and recovery keeps the old state, which is a correct outcome (CLAUDE.md, "the thing being protected was not the thing doing the protecting"). Its sabotage was run and stays green for that reason, not for want of a test. |

The object test's complete historical-handle scan is deliberately performed at
sample points rather than every operation. The handle table has 1024 physical
slots, which are reused while public handle IDs remain monotonic.

## Still uncovered

| Module | Why it remains in the debt | Required evidence before coverage can be claimed |
| --- | --- | --- |
| abi | The current torture models a narrow Linux subset: regular-file offsets and bytes, `pipe2` queue/nonblocking behavior, descriptor flags/duplication/close ranges/EMFILE, vector and positional I/O, relative `openat` with create/exclusive/truncate/append, `chdir`/`getcwd` including cwd-relative lookup and `ERANGE`, `linkat` hard-link identity/link counts/cross-mount refusal, `symlinkat`/`readlinkat` target bytes and symlink following, anonymous private `mmap` held against an interval model of what should be mapped and with which protection - after every map, protect and unmap each page of the range, and the one past it, must read exactly where the model maps it and take a write exactly where the model says it is writable (a mutation that narrows the page before an `mprotect` range is caught only this way) - with `MAP_FIXED_NOREPLACE` and replacement of an existing page with `MAP_FIXED`, rejection of unaligned fixed addresses and missing mapping types, page-byte isolation, file-backed `MAP_PRIVATE` copy isolation, tmpfs `MAP_SHARED` aliasing in both directions, `mprotect` read-only enforcement and partial `munmap`, `renameat2(RENAME_NOREPLACE)`, a two-level nested directory tree with relative dirfds, nested `getdents64`/`newfstatat`, `ENOTEMPTY` refusal and recursive cleanup, one directory's `getdents64` entries, `ftruncate`, and `fstat`/`newfstatat` size and type. It still does not model most of `kernel/abi/linux/` or full directory-tree operations. A separate NT dispatcher suite (`nt_syscall`, not the Linux selector in `ksvc_fake`) checks handle close/duplication, `NtCreateFile` dispositions, per-handle access-right enforcement, and seeded file read/write sequences against independent bytes/offsets; its NT mutations (including the rights gate) run that suite. This is still only a narrow slice of `kernel/abi/nt/`, not NT module coverage. Fork coverage currently proves descriptor-table inheritance, per-process descriptor flags, shared open-description offsets, inherited pipe writers, `wait4(WNOHANG)` while the child is live, private-page COW isolation after a child write, and that the parent's `munmap` leaves the child's copy mapped. Child exit/reap and exit-time descriptor release are not modelled: `ksvc_fake`'s `ks_task_exit` only escapes to the test and does not drive the task through the zombie/reap lifecycle. The portable process table is also not reached. `brk`/`mremap` and the other memory handlers remain uncovered. Most of `kernel/abi/nt/` also remains uncovered beyond the separate handle/file slice above. The checker deliberately keeps `abi` as one uncovered module: neither the remaining Linux handlers nor the NT subtree is complete enough to split off as covered. The NT host suite is isolated so its results cannot be mistaken for Linux-dispatcher coverage. A module-level annotation would overstate what the tests prove. | Extend the independent model across the remaining path/tree operations, broader file types, child exit/reap and exit-time descriptor release through a fake task lifecycle, the portable process table if wired, `brk`/`mremap` and the other memory handlers, and the remaining NT object, process, memory and I/O handlers. NT is already exercised through its separate `nt_syscall` host suite; it must not be counted as Linux-dispatcher coverage, and its currently modelled handle/file slice does not qualify the NT subtree. Where the fake cannot exercise an area, document it as uncovered rather than counting it implicitly. Each newly claimed area needs a production mutation caught by the model. |

## Mutation evidence

IPC mutations include a waitset round-robin cursor that does not advance and
priority ties resolved in the wrong direction, in addition to the focused
channel/event/waitset controls. OBJECT includes a quota-boundary mutation
(`>=` changed to `>`) that must be caught when the live count equals quota.
The mutations run against only their corresponding model in the focused PR
workflow. ABI's `lseek`, shared-offset, `pread64`-position, `pipe2` nonblocking mode,
`dup3`/`fcntl` close-on-exec, `close_range`, `EMFILE`, `pwrite64` position,
`O_APPEND`, `ftruncate`, stat-size, `getdents64`, `renameat2(RENAME_NOREPLACE)`,
`chdir`, `linkat`, `symlinkat`, `readlinkat` and the legacy pathname wrappers, `mkdirat`, `unlinkat`, anonymous and file-backed private/shared `mmap` including fixed replacement and mapping-type validation, `mprotect`,
`munmap`, fork descriptor-table-copy and fork COW-page marking sabotages are run against the ABI syscall state machine, not against
the IPC/OBJECT executable. PROC's `in_use`-on-reap mutation must be caught
by the lifecycle test. TXN's retirement-first negative control must fail
when the checkpoint flush is removed.

The long randomized IPC, OBJECT, PROC and TXN runs remain in separate nightly jobs with
ASan/UBSan and replayable seeds. The ABI nightly job is retained as a
non-qualifying, scoped test while its independent model is expanded. The
focused workflow runs the short CTest models and their production mutations on
relevant changes. The nightly does not repeat the mutation gate.

