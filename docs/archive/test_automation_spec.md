# Test Automation Specification

## 1. Purpose

This document specifies the automated test system for VibeOS with a special requirement: test outputs must be useful both to human engineers and to an implementation agent working directly in the repository.

The automation must therefore provide:

- deterministic pass or fail results
- concise failure summaries
- structured machine-readable artifacts
- subsystem-specific diagnostics that reduce re-triage effort

## 2. Test automation goals

- validate bring-up milestones continuously
- catch regressions close to the change that caused them
- provide enough context to support autonomous remediation work
- keep emulator-based feedback fast enough for iterative kernel development

## 3. Test layers

| Layer | Purpose | Typical runtime target |
|---|---|---|
| L0 host-unit | pure logic and parsers on host machine | seconds |
| L1 image-build | validates build graph and boot artifact generation | minutes |
| L2 boot-smoke | validates boot to known checkpoints in QEMU | minutes |
| L3 kernel-subsystem | validates VM, scheduler, interrupts, IPC semantics | minutes |
| L4 service-integration | validates init, service manager, and core services | minutes |
| L5 compatibility-regression | validates foreign-runtime expectations | longer-running and selective |

## 4. Mandatory automation properties

- tests must be runnable non-interactively
- every run must emit both human-readable logs and machine-readable result artifacts
- failures must include a stable test identifier
- kernel and emulator failures must capture the last known boot or subsystem phase
- timeout failures must be distinguished from assertion failures and infrastructure failures

## 5. Feedback contract for implementation agents

Every automated test run must emit a structured summary file, recommended path:

- `artifacts/test-summary.json`

Recommended schema:

```json
{
  "run_id": "string",
  "git_revision": "string",
  "suite": "boot-smoke",
  "status": "pass|fail|infra_error|timeout",
  "duration_ms": 0,
  "failed_tests": [
    {
      "id": "BOOT.SMOKE.SERIAL.001",
      "title": "Kernel did not emit expected boot banner",
      "layer": "L2",
      "subsystem": "boot",
      "severity": "blocker|high|medium|low",
      "symptom": "Expected token BOOT_OK was missing from serial output",
      "likely_causes": [
        "serial init failed",
        "kernel did not reach post-paging stage"
      ],
      "suggested_entrypoints": [
        "boot/",
        "kernel/arch/",
        "kernel/core/"
      ],
      "artifacts": [
        "artifacts/serial.log",
        "artifacts/qemu-exit.txt"
      ]
    }
  ]
}
```

This contract is designed to let an implementation agent immediately identify:

- which subsystem probably failed
- whether the failure is code, environment, or timeout related
- which files and logs are the best next entrypoints

## 6. Human-readable output requirements

Alongside structured output, the test system must produce:

- concise console summary
- per-suite log
- serial log capture for emulator runs
- failure excerpts limited to the most relevant lines first

Recommended console format:

```text
[FAIL] BOOT.SMOKE.SERIAL.001 boot Kernel did not emit expected boot banner
  phase: early_boot
  symptom: token BOOT_OK missing from serial log
  next: inspect artifacts/serial.log and boot serial init path
```

## 7. Subsystem-specific diagnostics

### 7.1 Boot and early kernel

Required artifacts:

- serial log
- boot phase markers
- emulator exit reason
- panic signature if available

### 7.2 Memory management

Required artifacts:

- mapping operation trace
- last page-fault record
- allocator counters

### 7.3 Scheduler

Required artifacts:

- dispatch trace excerpt
- timer tick counters
- runnable-queue snapshot on failure where feasible

### 7.4 IPC

Required artifacts:

- channel queue depth counters
- message send or receive trace excerpt
- timeout source attribution

## 8. Test IDs and taxonomy

Test identifiers must be stable and structured:

`<SUBSYSTEM>.<SUITE>.<CATEGORY>.<NUMBER>`

Examples:

- `BOOT.SMOKE.SERIAL.001`
- `MM.VM.MAP.004`
- `SCHED.CORE.PREEMPT.002`
- `IPC.CHANNEL.RPC.003`

## 9. Result classification

Every failure must map to one of these statuses:

- `assertion_failure`
- `timeout`
- `panic`
- `build_failure`
- `infra_error`
- `flaky_suspected`

Every failure must also include severity guidance:

- `blocker`: no further bring-up progress likely
- `high`: milestone exit criteria blocked
- `medium`: feature behavior wrong but development can continue in parallel
- `low`: non-blocking regression or degraded diagnostics

## 10. Milestone-aligned suite plan

### M1 toolchain and build skeleton

Required automation:

- build graph smoke test
- artifact existence validation
- toolchain version capture

### M2 boot to kernel banner

Required automation:

- QEMU boot smoke
- serial token assertion
- timeout and hang detection

### M3 memory and interrupts online

Required automation:

- page-table setup smoke
- page-fault negative test
- timer interrupt assertion

### M4 scheduler and multitasking

Required automation:

- context-switch smoke
- timeslice expiry test
- multi-thread fairness sanity test

### M5 first user-space service

Required automation:

- init launch assertion
- first IPC handshake test
- supervised child start smoke

## 11. Feedback tuning for autonomous development

To maximize usefulness for autonomous remediation, the automation should:

- attach likely subsystem ownership to every failure
- suggest the most relevant file roots
- prefer phase markers over raw logs alone
- distinguish deterministic failures from infrastructure noise
- preserve the first failure cause even when cascading errors follow

The goal is not to replace engineering judgment, but to reduce search cost after each failed run.

## 12. Non-goals

- full replacement of manual kernel debugging
- heuristic auto-fixing by the test harness itself
- hiding raw logs behind summaries

## 13. Open implementation decisions

- exact artifact directory layout
- whether JSON, JSONL, or both are emitted
- whether the test harness is implemented in Python, CMake scripts, or another host tool
- how much in-kernel trace data is safe to keep enabled in debug automation builds
