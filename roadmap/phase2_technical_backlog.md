# Phase 2 Technical Backlog

## Purpose

This backlog translates the software architecture specification into executable engineering work for the first implementation phase after documentation.

## Priority model

- `P0` blocks basic bring-up or architectural validation
- `P1` required for early kernel execution and first user-space transition
- `P2` important follow-on work after the first controlled boot path

## Epic A: Build and Toolchain Foundation

### A1. Cross-compilation baseline

- Priority: `P0`
- Deliverable: reproducible host-to-target build for bootloader, kernel, and image assembly
- Exit criteria:
- host and target flags are separated
- debug and release variants exist
- symbol files are emitted

### A2. Emulator workflow

- Priority: `P0`
- Deliverable: one-command QEMU boot path
- Exit criteria:
- serial output is captured automatically
- debug attach recipe is documented

## Epic B: Boot and Early Platform Bring-Up

### B1. Boot information contract

- Priority: `P0`
- Deliverable: versioned boot information structure consumed by the kernel
- Exit criteria:
- memory map, framebuffer or serial info, and firmware table pointers are passed correctly

### B2. Early console

- Priority: `P0`
- Deliverable: serial logging from the earliest reachable kernel point
- Exit criteria:
- panic path can write to serial
- log prefixes identify boot phase

### B3. Early paging enablement

- Priority: `P0`
- Deliverable: higher-half kernel mapping and stable early page tables
- Exit criteria:
- kernel executes after paging enablement
- direct map policy is implemented or stubbed with explicit bounds

### B4. SMP bootstrap skeleton

- Priority: `P1`
- Deliverable: bootstrap processor and one or more application processors initialize safely
- Exit criteria:
- per-CPU structures exist
- application processor startup is observable in logs

## Epic C: Kernel Bootstrap

### C1. Physical frame allocator

- Priority: `P0`
- Deliverable: early and runtime page-frame allocation path
- Exit criteria:
- allocator can reserve, allocate, and free page frames
- reserved and firmware regions remain protected

### C2. Interrupt and exception setup

- Priority: `P0`
- Deliverable: working trap, fault, and interrupt descriptor initialization
- Exit criteria:
- timer interrupt fires
- page fault path is diagnosable
- unhandled exceptions produce structured panic output

### C3. Kernel object table foundation

- Priority: `P1`
- Deliverable: minimal kernel object and handle infrastructure
- Exit criteria:
- process, thread, and event-like objects can be created
- rights masks are stored and checked

## Epic D: Scheduling

### D1. Per-CPU run queues

- Priority: `P1`
- Deliverable: schedulable queues for each CPU
- Exit criteria:
- runnable threads can be enqueued and dequeued deterministically
- idle path is explicit

### D2. Preemption and timer-driven dispatch

- Priority: `P1`
- Deliverable: timer-based rescheduling
- Exit criteria:
- timeslice expiry causes scheduler invocation
- dispatch events are logged in debug builds

### D3. SMP wake-up and balancing

- Priority: `P2`
- Deliverable: basic work migration and remote wake-up support
- Exit criteria:
- inter-processor wake-up path exists
- migration rules are documented

## Epic E: Virtual Memory

### E1. Address-space object

- Priority: `P1`
- Deliverable: creation and destruction of per-process address spaces
- Exit criteria:
- kernel can create an empty user address space
- page tables are isolated per process

### E2. Mapping API

- Priority: `P1`
- Deliverable: map, unmap, and protect primitives for memory objects
- Exit criteria:
- anonymous mappings work
- protection changes are enforced
- invalid mappings fail predictably

### E3. Copy-on-write groundwork

- Priority: `P2`
- Deliverable: shared page tracking sufficient for later fork-like compatibility modes
- Exit criteria:
- metadata structures for shared writable pages are defined
- fault path can distinguish copy-on-write cases

## Epic F: IPC Bootstrap

### F1. Event primitive

- Priority: `P1`
- Deliverable: minimal waitable signaling object
- Exit criteria:
- kernel and later user tasks can block and wake on events

### F2. Channel primitive

- Priority: `P1`
- Deliverable: bounded message channel with small-message support
- Exit criteria:
- synchronous request-response works
- queue overflow behavior is explicit

## Epic G: First User-Space Transition

### G1. Init launch path

- Priority: `P1`
- Deliverable: kernel launches first user-space task in a controlled address space
- Exit criteria:
- transition is visible in logs
- failed launch returns to diagnosable panic or recovery path

### G2. Service manager skeleton

- Priority: `P2`
- Deliverable: minimal supervised service host
- Exit criteria:
- one child service can be started and observed

## Epic H: Test Automation Foundation

### H1. Structured result contract

- Priority: `P0`
- Deliverable: machine-readable test summary artifact for every automated run
- Exit criteria:
- `artifacts/test-summary.json` is emitted
- pass, fail, timeout, and infra-error are distinguished
- every failed test includes stable ID, subsystem, and artifact references

### H2. Boot smoke automation

- Priority: `P0`
- Deliverable: QEMU-driven smoke test for the earliest boot milestone
- Exit criteria:
- serial log is captured automatically
- expected boot tokens are asserted
- hangs are classified as timeouts rather than generic failures

### H3. Agent feedback profile

- Priority: `P1`
- Deliverable: test output mode optimized for autonomous or assisted remediation
- Exit criteria:
- failure summaries include likely causes
- failure summaries include suggested repository entrypoints
- the first failure cause is preserved in the summary

### H4. Subsystem diagnostics hooks

- Priority: `P1`
- Deliverable: debug-build hooks for VM, scheduler, and IPC diagnostics
- Exit criteria:
- page-fault metadata can be captured
- scheduler dispatch trace can be sampled
- channel-related timeout context is available

## Cross-cutting constraints

- every `P0` and `P1` item must have at least one automated validation path
- no subsystem may silently expand kernel-space scope beyond the ADR set
- debug output format must remain stable enough for automated parsing
