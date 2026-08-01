/* Fuzz the network receive path.
 *
 * vibeos_inet_input() is the one function in the system that is fed bytes
 * chosen by whoever is on the other end of the wire: Ethernet, ARP, IPv4,
 * ICMP, UDP, TCP, DHCP and DNS parsing all hang off it. Everything else the
 * host tests cover is driven by us; this is the part an attacker drives.
 *
 * The stack is re-initialised for every input so a crash reproduces from the
 * single file that caused it, rather than depending on whatever the fuzzer
 * happened to feed earlier.
 *
 * Build and run:
 *   cmake -S . -B build-fuzz -DVIBEOS_BUILD_FUZZERS=ON -DCMAKE_C_COMPILER=clang
 *   ./build-fuzz/fuzz_inet_input -max_total_time=60
 */

#include <stdint.h>
#include <stddef.h>

#include "vibeos/inet.h"

static vibeos_inet_t g_net;

/* Frames the stack tries to send go nowhere; we are exercising receive. */
static int fuzz_tx(void *ctx, const void *frame, uint32_t len) {
    (void)ctx;
    (void)frame;
    (void)len;
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    static const uint8_t mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
    static uint8_t frame[VIBEOS_INET_MTU];
    size_t i;

    if (size < 14u || size > VIBEOS_INET_MTU) {
        return 0;   /* shorter than an Ethernet header: nothing to parse */
    }

    for (i = 0; i < size; i++) {
        frame[i] = data[i];
    }
    /* Address the frame to us.
     *
     * The receive path drops anything not sent to this MAC, which for a fuzzer
     * is six bytes of pure guessing that consumes the entire budget before any
     * protocol code is reached. An attacker on the same link knows the address,
     * so making the fuzzer spend its time past that check tests what actually
     * matters. Every fourth input is left as broadcast to keep that path live.
     */
    if ((data[0] & 3u) == 0u) {
        for (i = 0; i < 6u; i++) {
            frame[i] = 0xFFu;
        }
    } else {
        for (i = 0; i < 6u; i++) {
            frame[i] = mac[i];
        }
    }
    /* Repair the IPv4 header checksum for most inputs.
     *
     * A correct checksum is essentially unreachable by mutation, so without
     * this the fuzzer never gets past ip_input and ICMP, UDP and TCP are never
     * parsed at all. Computing it is free for a real sender. One input in four
     * is left untouched so the rejection path stays covered too. */
    if (size >= 14u + 20u && frame[12] == 0x08u && frame[13] == 0x00u &&
        (frame[14] >> 4) == 4u && (data[1] & 3u) != 0u) {
        uint8_t *ip = frame + 14;
        uint32_t ihl = (uint32_t)(ip[0] & 0x0Fu) * 4u;
        if (ihl >= 20u && (size_t)(14u + ihl) <= size) {
            uint16_t ck;
            ip[10] = 0;
            ip[11] = 0;
            ck = vibeos_inet_checksum(ip, ihl);
            ip[10] = (uint8_t)(ck >> 8);
            ip[11] = (uint8_t)(ck & 0xFFu);
        }
    }

    data = frame;

    vibeos_inet_init(&g_net, mac, fuzz_tx, 0);
    vibeos_inet_set_addr(&g_net, 0x0A00020Fu, 0xFFFFFF00u, 0x0A000202u, 0x0A000203u);

    /* Some paths only exist once a socket is listening or a lookup is in
     * flight, so open them before feeding the frame. */
    {
        int tcp = vibeos_inet_socket(&g_net, VIBEOS_INET_SOCK_TCP);
        int udp = vibeos_inet_socket(&g_net, VIBEOS_INET_SOCK_UDP);
        if (tcp >= 0) {
            (void)vibeos_inet_bind(&g_net, tcp, 8080);
            (void)vibeos_inet_listen(&g_net, tcp);
        }
        if (udp >= 0) {
            (void)vibeos_inet_bind(&g_net, udp, 4242);
        }
        (void)vibeos_inet_dhcp_start(&g_net);
        (void)vibeos_inet_resolve(&g_net, "example.test");
    }

    (void)vibeos_inet_input(&g_net, data, (uint32_t)size);

    /* Drive the timers too: retransmission, lease and cache expiry all run
     * against whatever state the frame just produced. */
    vibeos_inet_poll(&g_net, 1000);
    vibeos_inet_poll(&g_net, 120000);

    return 0;
}
