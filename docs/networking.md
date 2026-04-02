# Networking Stack

## Goals

- modern TCP/IP capability
- modular protocol evolution
- low latency and good multicore throughput
- clean socket semantics for native and compatibility applications

## Stack structure

### Kernel responsibilities

- packet ingress or egress primitives
- network buffer management fast path
- interface event delivery
- low-level filtering hooks

### User-space or service responsibilities

- control plane
- configuration management
- policy enforcement
- advanced routing
- firewall management
- network namespaces or tenant policies in future phases

## Protocol roadmap

### Early support

- Ethernet
- IPv4
- IPv6
- ARP or neighbor discovery
- ICMP
- UDP
- TCP

### Later support

- TLS acceleration hooks
- QUIC-friendly interfaces
- virtual switching
- container networking

## Socket model

The native socket layer should be expressive enough to support:

- POSIX-like compatibility
- Windows socket compatibility through subsystem translation
- event-driven high-performance servers

## Performance direction

- per-CPU queues
- RSS-friendly receive flow steering
- zero-copy or reduced-copy buffers where safe
- future support for polling rings or submission queues if justified

## Security

- namespace-aware network access
- brokered raw socket access
- firewall and policy hooks
- sandbox integration for compatibility runtimes
