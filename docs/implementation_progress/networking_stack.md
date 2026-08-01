# Networking Stack Progress

Status: In Progress (runtime virtio-net TCP/IP baseline verified)
Last review: 2026-08-01

## Implemented
- Network runtime/service scaffolding in `user/net/network_service.c`.
- Socket lifecycle primitives in `user/net/socket.c`.
- Basic policy-aware bind checks integrated with security policy layer.
- Service lifecycle integration under service manager control path.
- Deterministic packet-path simulation API (`vibeos_net_simulate_path`) with latency/drop counters.
- Extended network telemetry snapshot (`vibeos_net_stats_ext`) including simulated ticks and drops.
- A runtime TCP/IP baseline in `kernel/net/inet.c` is connected to the virtio-net driver, with Ethernet, ARP, IPv4, ICMP, UDP and TCP handling for the QEMU path.
- User networking programs and the QEMU CLI smoke path exercise the on-metal network route rather than only the host simulation model.

## Pending
- DHCP/DNS, complete TCP state/error handling, firewall policy and robust recovery semantics.
- Packet-path performance instrumentation, queueing policy and concurrency hardening.

## Next checkpoint
- Add repeatable QEMU network integration tests, malformed-packet coverage and TCP lifecycle regression tests.
