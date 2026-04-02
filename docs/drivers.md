# Device Drivers

## Driver model goals

- isolate faults from the kernel whenever practical
- standardize device discovery and lifecycle
- support high-performance paths for networking and storage
- keep hardware-specific logic out of generic subsystems

## Driver architecture

### Driver manager

A dedicated driver manager service is responsible for:

- device enumeration
- driver matching and loading
- lifecycle supervision
- permission brokerage for driver-host processes

### Driver hosts

Preferred model:

- user-space driver hosts for most devices
- specialized privileged drivers only for boot-critical or ultra-low-latency hardware paths

### Device classes

- storage
- network
- display
- input
- USB and bus controllers
- audio
- virtual devices

## Hardware abstraction

The kernel exposes safe primitives for:

- interrupt delivery
- MMIO mapping
- DMA setup through mediated interfaces
- device memory objects
- event notification

This keeps the kernel in control of trust boundaries even when drivers live in user space.

## Initial priorities

- APIC or interrupt controller support
- timer devices
- serial console
- framebuffer or simple display output
- AHCI or NVMe storage
- PCIe enumeration
- basic network interface

## Reliability model

- supervised restart for user-space drivers
- versioned driver-service protocols
- structured fault and telemetry reporting
