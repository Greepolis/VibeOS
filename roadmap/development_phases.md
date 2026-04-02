# Development Phases

## Phase 1: architecture and project foundation

Deliverables:

- system vision
- architectural decisions
- subsystem specifications
- repository structure
- development roadmap and milestones

Exit criteria:

- design documents reviewed for internal consistency
- initial implementation order agreed
- module boundaries documented

## Phase 2: bring-up infrastructure

Deliverables:

- build system bootstrap
- cross-compilation toolchain setup
- emulator and test harness
- boot image generation pipeline
- serial logging path

Exit criteria:

- buildable boot artifact
- emulator boot to controlled early stub

## Phase 3: kernel bootstrap

Deliverables:

- bootloader handoff
- early memory manager
- interrupt and exception setup
- basic physical and virtual memory
- kernel logging and panic path

Exit criteria:

- kernel boots in emulator
- page tables and interrupts working

## Phase 4: execution core

Deliverables:

- thread and process primitives
- scheduler
- timers
- IPC base mechanisms
- SMP startup

Exit criteria:

- multi-threaded user task execution
- stable context switching

## Phase 5: service-oriented base system

Deliverables:

- init and service manager
- driver manager
- filesystem service baseline
- shell and core utilities

Exit criteria:

- user-space services start reliably
- basic file and process operations available

## Phase 6: storage, networking, and security baseline

Deliverables:

- native filesystem prototype
- storage drivers
- TCP/IP baseline
- security tokens and sandbox primitives

Exit criteria:

- persistent storage
- networked userland workloads

## Phase 7: Linux compatibility foundation

Deliverables:

- ELF loader
- Linux personality runtime
- basic POSIX process, file, and signal compatibility

Exit criteria:

- selected Linux CLI applications run successfully

## Phase 8: Windows compatibility foundation

Deliverables:

- PE loader
- Windows subsystem services
- basic process, file, and console compatibility

Exit criteria:

- selected Windows console applications run successfully

## Phase 9: advanced compatibility and platform growth

Deliverables:

- container model
- lightweight VM path
- GUI and graphics stack groundwork
- ARM64 portability effort

Exit criteria:

- multi-environment workload execution
- second architecture plan validated
