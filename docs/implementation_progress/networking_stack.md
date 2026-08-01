# Networking Stack Progress

Status: In Progress (runtime IPv4 baseline and DHCP/DNS regressions verified)
Last review: 2026-08-01

## Implemented
- Network runtime/service scaffolding in `user/net/network_service.c`.
- Socket lifecycle primitives in `user/net/socket.c`.
- Basic policy-aware bind checks integrated with security policy layer.
- Service lifecycle integration under service manager control path.
- Deterministic packet-path simulation API (`vibeos_net_simulate_path`) with latency/drop counters.
- Extended network telemetry snapshot (`vibeos_net_stats_ext`) including simulated ticks and drops.
- A runtime TCP/IP baseline in `kernel/net/inet.c` is connected to the virtio-net driver, with Ethernet, ARP, IPv4, ICMP, UDP, TCP, DHCPv4 and DNS handling for the QEMU path.
- User networking programs and the QEMU CLI smoke path exercise the on-metal network route rather than only the host simulation model.
- The receive path rejects invalid TCP checksums and non-zero invalid UDP checksums before dispatching payloads; TCP handshake transitions require an acknowledgement for the transmitted sequence number.
- Host regressions cover DHCP OFFER/ACK processing, DNS A response parsing, TCP retransmission and close paths, plus malformed L4 checksum rejection.
- DNS keeps a bounded eight-entry positive cache with TTL clamping; cache hits are resolved without a new packet transmission.
- Mbed TLS 3.6.5 is pinned as a submodule and exposed through a narrow hosted adapter (`vibeos_tls_*`). The adapter is built into its own `vibeos_tls` target, linked only by the host tests: the freestanding kernel image links `vibeos_user_core`, so keeping the hosted crypto stack out of that library is what structurally prevents it from reaching the image.
- The dependency is optional at build time. A tree without the submodule configures and builds with TLS absent, and the adapter reports `vibeos_tls_runtime_available() == 0`; the host test asserts the adapter's contract in both configurations. Builds that must ship TLS set `-DVIBEOS_REQUIRE_TLS=ON` and fail loudly if the submodule is missing, which is how the Linux CI matrix is configured.

## Pending
- DHCP lease renew/rebind/expiry, concurrent DNS queries and negative cache, complete TCP state/error handling, firewall policy and robust recovery semantics.
- Ring-3 TLS service integration: entropy source, trust store, TCP callbacks and QEMU TLS handshake validation are intentionally pending; the current adapter is not a guest TLS implementation.
- Packet-path performance instrumentation, queueing policy and concurrency hardening.

## Next checkpoint
- Add repeatable QEMU network integration tests, malformed-packet coverage and TCP lifecycle regression tests.
