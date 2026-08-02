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
- Mbed TLS 4.2.0 is pinned as a submodule and exposed through a narrow hosted adapter (`vibeos_tls_*`). The adapter is built into its own `vibeos_tls` target, linked only by the host tests: the freestanding kernel image links `vibeos_user_core`, so keeping the hosted crypto stack out of that library is what structurally prevents it from reaching the image. In 4.x the crypto implementation lives in TF-PSA-Crypto, a nested submodule building `tfpsacrypto`; the build asks which target exists rather than naming one, because CMake turns an unknown target name into a raw linker flag and the failure then surfaces at link time instead of configure time.
- The dependency is optional at build time. A tree without the submodule configures and builds with TLS absent, and the adapter reports `vibeos_tls_runtime_available() == 0`; the host test asserts the adapter's contract in both configurations. Builds that must ship TLS set `-DVIBEOS_REQUIRE_TLS=ON` and fail loudly if the submodule is missing, which is how the Linux CI matrix is configured.

### Wave 1 (runtime enforced)
- UDP queues multiple datagrams per socket, framed in the receive buffer; each `recvfrom` drains exactly one, oversized datagrams truncate without leaving a partial second delivery, and a full queue drops the newest and counts it.
- TCP holds out-of-order segments (four slots per socket) and releases them when the gap is filled, instead of discarding anything not at `rcv_nxt` and forcing wholesale retransmission. Out-of-order arrivals produce duplicate ACKs.
- Connections that are closing are reclaimed: TIME_WAIT and an unanswered FIN both arm a bounded deadline, so a peer that goes silent cannot pin a socket slot for the life of the system.
- DHCP leases have a lifetime: options 51/58/59 are honoured, the client renews at T1 and rebinds at T2, gives the address up at expiry and returns to discovery, and a NAK drops the lease immediately.
- DNS queries retry a bounded number of times and then fail, so a caller cannot wait forever on a server that never answers.
- Sockets are owned by their process: a task that exits with connections open has them closed as it is retired.

## Pending
- Concurrent DNS queries (the API still resolves one name at a time) and negative caching.
- Complete TCP error handling: RST generation for unknown connections, window probing, congestion control.
- Firewall policy, routing table and robust recovery semantics.
- Ring-3 TLS service integration: entropy source, trust store, TCP callbacks and QEMU TLS handshake validation are intentionally pending; the current adapter is not a guest TLS implementation.
- Packet-path performance instrumentation, queueing policy and concurrency hardening.

## Deferred: Waves 2-5 (not started)
These are recorded so the scope is explicit, not because work has begun. Status
stays `In Progress` until the Wave 5 gates pass, per the plan's own rule.

- **Wave 2 - IPv6, routing, firewall.** Extension-header parsing with anti-abuse limits, NDP, ICMPv6, Router Advertisement, SLAAC, Duplicate Address Detection, DHCPv6, dual-stack `AF_INET`/`AF_INET6` sockets with an explicit IPv4-mapped policy, a per-family route table with longest-prefix match and reachability, and a stateful default-deny firewall with audit events. On its own this is larger than the entire stack built so far.
- **Wave 3 - services, namespaces, switching.** A ring-3 `netd` owning configuration, DHCP/DNS/echo reference daemons, network namespaces covering interfaces/routes/socket visibility/DNS/firewall, veth pairs and a software switch, plus namespace and capability identity attached to every socket. Blocked on a more capable ring-3 IPC layer than exists today.
- **Wave 4 - TLS runtime.** The Mbed TLS adapter is hosted-only. A guest TLS client needs an OS entropy provider, a versioned trust store, TCP callbacks from ring 3 and handshake validation in QEMU. None of that exists; the current adapter is a dependency boundary and a version gate, nothing more.
- **Wave 5 - performance and release gates.** Bounded buffer pools with backpressure, multiqueue virtio-net with IRQ affinity, rate limiting and per-namespace quotas, repeatable benchmarks, continuous protocol fuzzing, and a 24h soak. These are largely process gates rather than code.

## Next checkpoint
- Add repeatable QEMU network integration tests, malformed-packet coverage and TCP lifecycle regression tests.
