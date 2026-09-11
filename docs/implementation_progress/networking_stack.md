# Networking Stack Progress

Status: In Progress (runtime IPv4 baseline plus portable route/firewall data-path enforcement verified)
Last review: 2026-08-26

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
- A portable routing/firewall policy contract (`include/vibeos/net_policy.h`, `kernel/net/net_policy.c`) now provides longest-prefix IPv4 route lookup and default-deny stateful flow admission; host tests cover route precedence, ingress denial, allowed reply flow and protocol denial.
- `vibeos_inet` can attach the portable policy to its IPv4 data path: transmit requires a matching route and egress rule, receive validates TCP/UDP endpoint ports before protocol dispatch, and an accepted egress flow admits the reverse reply. Host regression covers ingress denial and a per-port UDP allow through `sendto`; virtio-net and syscall control-plane wiring remain separate follow-up work.
- Stateful policy flows now have bounded idle garbage collection (`vibeos_net_policy_expire_flows`) and are reclaimed from the portable network poll path, preventing stale entries from exhausting the fixed flow table. Host coverage verifies an accepted reply is denied after expiry.
- Portable sockets now carry an explicit owner PID and expose owner-checked close operations; host coverage rejects cross-process close attempts. Syscall propagation and automatic process-exit cleanup remain pending.
- The x86_64 socket syscall assigns the current PID as owner, and process exit now invokes forced owner cleanup before retiring file descriptors, reclaiming all owned sockets without waiting for TCP close timers. Broader syscall capability checks remain pending.
- Route and firewall policy entries now support validated removal with bounded-table counters, allowing a control plane to replace configuration without rebooting or leaking slots; transactional updates and syscall capability gates remain pending.
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
- Firewall policy, routing table and robust recovery semantics beyond the portable IPv4 path.
- The routing/firewall contract is not yet wired into virtio-net or the socket syscalls; process ownership, rule audit events, flow expiry and IPv6 policy remain pending.
- Ring-3 TLS service integration: entropy source, trust store, TCP callbacks and QEMU TLS handshake validation are intentionally pending; the current adapter is not a guest TLS implementation.
- Packet-path performance instrumentation, queueing policy and concurrency hardening.

## Security findings

### M-005: a RST without a sequence check (fixed 2026-09-11)

`tcp_input` found the socket by address and ports and, on RST, closed it - with
no look at the sequence number. Anybody able to put a packet on the path could
end a connection without knowing where its byte stream was. And `tcp_lookup`
also matches a listening socket, so a RST to a listening port closed the
listener itself.

The rule now is RFC 5961 section 3.2, in `tcp_input_rst`:

- LISTEN: nothing to reset; ignored.
- SYN_SENT: acceptable only if it acknowledges our SYN. That is how a connect to
  a closed port is refused, so that case still ends at once.
- otherwise: `seq == rcv_nxt` resets; anywhere else in the receive window gets a
  challenge ACK naming `rcv_nxt` (a genuine peer answers with the exact RST);
  outside it is dropped silently. The window is the free receive buffer, and the
  offset is unsigned modular arithmetic, so a number just behind `rcv_nxt` wraps
  to a large offset and is dropped instead of read as in-window.

The RST decision also moved ahead of the send-window update: a segment that is
not believed changes nothing.

`test_inet_tcp_rst_needs_sequence` was written first and failed on the old code.
It asserts the state *and* what was sent at each step, because "still
established" alone passes a stack that ignores every RST. Host tests green,
`check.sh all` green, twelve boots: 11 pass, 1 fail - task_illegal_transition (running->running by hw_task_exit) in THREADS' exit_group stage, a scheduler-exit signature this change does not touch and seen for the first time today; recorded with the exit-window evidence as its likeliest cause, H-007's locked exit_group loop not yet ruled out.

What this does not cover: an attacker who can see the traffic reads `rcv_nxt`
off the wire. Sequence checks stop blind injection; on-path needs authentication
above TCP.

### H-008: the initial sequence number was the clock (fixed 2026-09-11)

`connect` used `0x1000 + now_ms` and a listening socket `0x2000 + now_ms`.
Whoever could forge a packet could also forge the ACK that completes a
handshake, and then knew the sequence number data would be accepted at - so a
blind attacker could both establish a connection and inject into it.

Now RFC 6528: `ISN = now_ms/4 + SipHash-2-4(local ip, remote ip, local port,
remote port; secret)`, in `tcp_isn`, used by both paths. The secret is one per
stack, `vibeos_inet_set_secret`, because this portable layer has no entropy of
its own; M-007 and H-009 will derive their identifiers from the same secret.

`test_inet_tcp_isn_depends_on_secret` was written first and failed on the old
code. It cannot assert randomness, so it asserts the properties that matter:
the same key, time and tuple give the same ISN (the test is about inputs, not
noise); a different key or a different tuple gives a different one; and it is
not the bare clock value. Both directions are tested.

**The secret is only as good as its source, and under the boot gate it is not
good.** The arch layer takes it from RDRAND when CPUID advertises it and
otherwise from the TSC mix that AT_RANDOM already uses, which that function's
comment calls what it is. QEMU's default TCG CPU has no RDRAND, and every gate
boot logs `[NET] stack secret from tsc mix (weak: no entropy source on this
cpu)`. The structure is right and a real source slots in at one call; the
kernel still has no entropy source, and that is its own open item.

Also not covered, as for any ISN scheme: an attacker on the path reads the ISN
off the wire.

Host tests green, `check.sh all` green, twelve boots: 12 pass, 0 fail.

### M-007: a DNS answer from anybody, for anything (fixed 2026-09-11)

`dns_input` accepted any datagram to the fixed local port `0xC353` whose id
matched the pending query. `udp_input` did not pass it the sender, so it could
not have checked one; the question in the answer was skipped, not compared; and
the id advanced by `0x1235` from the same start every boot. An attacker who
could put one packet on the path answered first, and the address went into the
cache for its TTL.

Now an answer is believed only if it comes from the configured server, from port
53, has the response bit, carries the pending id, and has exactly one question
that is the name being resolved, type A, class IN (labels compared without
case). The id and the source port are drawn per query from SipHash over the
stack secret, the name, a query counter and the time; the port avoids any bound
UDP socket, whose datagrams the resolver would otherwise take. A blind spoofer
now has about 30 bits to hit instead of none - and, as the H-008 section says,
the secret under TCG comes from the weak source.

The test, `test_inet_dns_reply_must_match_query`, reads the id and port from the
query as captured on the wire, the way a server or an attacker sees them, and
checks the wrong sender, the wrong port and the wrong question separately
before the genuine answer. An older test answered to the fixed port and now
answers to the port the query came from.

**Two things went wrong on the way, and both are worth knowing.** The first
version of the test was red for the wrong reason. The DNS server it configured
was not the address `inet_seed_arp` teaches, so the one frame captured was the
ARP request for it; the "id" and "port" read from that frame were zero, every
reply went to port 0, and the test failed at the genuine answer on the old and
the new code alike. A step-numbered run showed it. The server is the gateway in
that test now, and the test checks that the captured frame is a UDP datagram to
port 53 before reading from it. Re-run on the unfixed code it fails where it
should - the wrong sender accepted, `id=0x1235 port=0xc353` - and passes on the
fixed code.

The second: restoring the fixed `inet.c` with a file copy kept the copy's older
timestamp, the build judged the object up to date, and the "fixed" run measured
the red code. The step numbers gave it away - the old identifiers were still
there. The sources were touched and the run repeated.

Host tests green, `check.sh all` green, twelve boots: 12 pass, 0 fail.

### H-009: a DHCP reply from anybody (fixed 2026-09-11)

The xid was `0x56494245` mixed with two bytes of the MAC, and `dhcp_input`
checked it, the magic cookie and the state - nothing else. Any host that could
put a datagram on the segment could answer the DISCOVER, send an ACK to a
renewing client out of nowhere, and set the address, gateway and DNS. Worse than
reported: a NAK from anybody, in any state, dropped the lease and restarted
discovery - one datagram took the machine off the network.

Now the xid comes from the stack secret, anew for each transaction, and a reply
must come from the server port, be a reply, and carry this client's hardware
address. An OFFER is taken only while discovering and only if it names its
server. The ACK to a REQUEST must come from the server chosen and give the
address offered; a renewal ACK must come from the server holding the lease
(any server while rebinding, as RFC 2131 allows) and give the address in use. A
NAK must come from the server being dealt with.

What no client can close without authentication, and this does not claim to:
an attacker on the same segment sees the broadcast DISCOVER - xid included - and
can answer first with an OFFER that passes every check. The change narrows the
attack to that race and removes it for anyone not on the segment.

`test_inet_dhcp_reply_must_match_transaction` reads the xid off the captured
DISCOVER. It was run red twice: as written, where it fails at the first check
because two stacks with different secrets produced the same xid; and with that
check switched off, where it still fails, on an OFFER addressed to another
client being accepted - so the transaction checks can fail on their own. Two
existing tests gained what RFC 2131 requires of a server and they had left out:
the client's hardware address in every reply, and the server identifier in the
ACK. They pass on the old code as well.

Host tests green, `check.sh all` green, and the gate's DHCP lease came up as
before (`ip=10.0.2.15 gw=10.0.2.2 dns=10.0.2.3`). Twelve boots: 11 pass, 1 fail
- the THREADS four-worker crash family, nothing to do with DHCP; recorded in
boot_repeatability.md because it recurred after the exit-window fix.

## Deferred: Waves 2-5 (not started)
These are recorded so the scope is explicit, not because work has begun. Status
stays `In Progress` until the Wave 5 gates pass, per the plan's own rule.

- **Wave 2 - IPv6, routing, firewall.** Extension-header parsing with anti-abuse limits, NDP, ICMPv6, Router Advertisement, SLAAC, Duplicate Address Detection, DHCPv6, dual-stack `AF_INET`/`AF_INET6` sockets with an explicit IPv4-mapped policy, a per-family route table with longest-prefix match and reachability, and a stateful default-deny firewall with audit events. On its own this is larger than the entire stack built so far.
- **Wave 3 - services, namespaces, switching.** A ring-3 `netd` owning configuration, DHCP/DNS/echo reference daemons, network namespaces covering interfaces/routes/socket visibility/DNS/firewall, veth pairs and a software switch, plus namespace and capability identity attached to every socket. Blocked on a more capable ring-3 IPC layer than exists today.
- **Wave 4 - TLS runtime.** The Mbed TLS adapter is hosted-only. A guest TLS client needs an OS entropy provider, a versioned trust store, TCP callbacks from ring 3 and handshake validation in QEMU. None of that exists; the current adapter is a dependency boundary and a version gate, nothing more.
- **Wave 5 - performance and release gates.** Bounded buffer pools with backpressure, multiqueue virtio-net with IRQ affinity, rate limiting and per-namespace quotas, repeatable benchmarks, continuous protocol fuzzing, and a 24h soak. These are largely process gates rather than code.

## Next checkpoint
- Wire the policy into the x86_64 virtio/socket control path, add rule audit events, then add repeatable QEMU network integration tests, malformed-packet coverage and TCP lifecycle regression tests.
