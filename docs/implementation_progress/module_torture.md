# Nightly Module Torture (#201)

Status: IPC and OBJECT are covered by this change. ABI, PROC and TXN remain
in the nightly coverage debt until their tests discriminate the properties
that matter.

The coverage checker has a baseline of three uncovered modules. The baseline
is a hard upper bound, not a claim that every module has a qualifying test.
Each module removed from the debt must have an independent model and a
production mutation that demonstrates the model can detect a meaningful
regression.

## Covered models

| Module | Production surface exercised | Independent oracle |
| --- | --- | --- |
| ipc | `kernel/ipc/channel.c`, `event.c`, `waitset.c` | A plain FIFO array for channel contents; independently tracked event state; and separately derived expected order for FIFO, reverse, round-robin and priority waitset policies. The oracle checks behaviour through public operations and does not inspect `channel.count` or other implementation state. |
| object | `kernel/object/handle_table.c` | A seeded handle ledger for allocation, close, quota, rights, object identity and per-type counts. The quota varies by seed; a balanced allocation/close stream avoids permanent saturation; every historical handle is checked at each sample point, including every live handle and every closed-id rejection. |

The object test's complete historical-handle scan is deliberately performed at
sample points rather than every operation. The handle table has 1024 physical
slots, which are reused while public handle IDs remain monotonic.

## Still uncovered

| Module | Why it remains in the debt | Required evidence before coverage can be claimed |
| --- | --- | --- |
| abi | The existing test compares immutable syscall metadata lookup with a linear scan. It does not model dynamic row registration, dispatch, or Linux/NT handlers. Random probes over a constant table are weaker than an exhaustive check. | Exhaustive metadata boundary checks are useful but do not cover the handlers. Add seeded syscall sequences through `tests/kernel/ksvc_fake.c` and compare descriptors/paths/results with an independent model, or keep the module uncovered. |
| proc | `kernel/proc/process.c` is not reachable from the live x86-64 syscall path and contains the known process-slot reclamation defect: termination leaves process entries `in_use`. The previous torture asserted that this defect was expected behaviour, so a future fix would make the test fail for the wrong reason. | First decide whether this portable subsystem is retained, removed, or wired. If retained, model process reaping and slot reuse as the intended contract; do not encode the current leak as correct behaviour. Keep the reachability fence in `docs/core/phases.md`. |
| txn | The existing device model made all volatile writes durable in one flush and injected only a checkpoint target-write failure after commit. It therefore did not discriminate removal or reordering of the pre-commit flush. | Simulate a power cut after each individual device write (and relevant flush boundary), reboot from the durable image, and assert the durable state is all-old or all-new. The sweep must catch removal of the phase-one flush. |

## Mutation evidence

IPC mutations include a waitset round-robin cursor that does not advance and
priority ties resolved in the wrong direction, in addition to the focused
channel/event/waitset controls. OBJECT includes a quota-boundary mutation
(`>=` changed to `>`) that must be caught when the live count equals quota.
The mutations run against only their corresponding module mode in the focused
PR workflow.

The long randomized IPC and OBJECT runs remain in separate nightly jobs with
ASan/UBSan and replayable seeds. The focused workflow runs the short CTest
models and their production mutations on relevant changes. The nightly does
not repeat the mutation gate.

