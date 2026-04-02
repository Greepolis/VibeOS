# Userland

## Goals

- minimal but practical base system
- service-oriented architecture aligned with the kernel model
- strong tooling for bring-up, diagnostics, and automation

## Core userland components

### Init and service manager

- boots essential services
- enforces dependency ordering
- restarts supervised services
- exposes health and status APIs

Early phases should keep `init` and the long-lived service manager separate so that bootstrap concerns do not hard-code later supervision policy into the first process.

### System services

- device manager
- filesystem manager
- network manager
- security policy service
- session manager
- logging and tracing services
- compatibility subsystem hosts

### Command-line environment

- native shell
- standard utilities
- process inspection tools
- filesystem and networking tools
- tracing and crash-debug tools

## Library strategy

- small native C runtime and system library
- C++ support for selected user-space components
- stable userspace API layered over the native syscall ABI

## Desktop and GUI direction

GUI is not part of the earliest milestones, but the architecture should anticipate:

- compositor service
- display server or protocol layer
- sandbox-friendly application model
- remote and virtual display support

## Developer experience priorities

- cross-toolchain support
- deterministic builds
- emulator-friendly boot flows
- serial and structured logs
- conformance and regression test harnesses
