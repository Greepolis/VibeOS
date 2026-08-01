/* Ethernet / ARP / IPv4 / ICMP / UDP / TCP, with a DHCP client and a DNS
 * resolver on top.
 *
 * No hardware here: frames come in through vibeos_inet_input(), go out through
 * the transmit callback, and time advances through vibeos_inet_poll(). The same
 * object runs under host tests and behind the virtio-net driver on metal.
 *
 * Wire formats are read and written byte by byte rather than through packed
 * structs, so nothing depends on the compiler's struct layout or on the
 * alignment of a received frame.
 */

#include "vibeos/inet.h"

/* ---- byte order helpers -------------------------------------------------- */

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t rd32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void wr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFFu);
}

static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)((v >> 16) & 0xFFu);
    p[2] = (uint8_t)((v >> 8) & 0xFFu);
    p[3] = (uint8_t)(v & 0xFFu);
}

static void bcopy_n(uint8_t *dst, const uint8_t *src, uint32_t n) {
    uint32_t i;
    for (i = 0; i < n; i++) {
        dst[i] = src[i];
    }
}

static void bzero_n(uint8_t *dst, uint32_t n) {
    uint32_t i;
    for (i = 0; i < n; i++) {
        dst[i] = 0;
    }
}

static int beq_n(const uint8_t *a, const uint8_t *b, uint32_t n) {
    uint32_t i;
    for (i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

/* ---- checksums ----------------------------------------------------------- */

static uint32_t sum16(const uint8_t *d, uint32_t len, uint32_t acc) {
    uint32_t i;
    for (i = 0; i + 1u < len; i += 2u) {
        acc += ((uint32_t)d[i] << 8) | d[i + 1u];
    }
    if (i < len) {
        acc += (uint32_t)d[i] << 8;   /* odd trailing byte, zero padded */
    }
    return acc;
}

static uint16_t fold(uint32_t acc) {
    while (acc >> 16) {
        acc = (acc & 0xFFFFu) + (acc >> 16);
    }
    return (uint16_t)(~acc & 0xFFFFu);
}

uint16_t vibeos_inet_checksum(const void *data, uint32_t len) {
    return fold(sum16((const uint8_t *)data, len, 0));
}

/* Checksum over the TCP/UDP pseudo-header plus the segment. */
static uint16_t l4_checksum(uint32_t src, uint32_t dst, uint8_t proto,
                            const uint8_t *seg, uint32_t len) {
    uint32_t acc = 0;
    acc += (src >> 16) & 0xFFFFu;
    acc += src & 0xFFFFu;
    acc += (dst >> 16) & 0xFFFFu;
    acc += dst & 0xFFFFu;
    acc += proto;
    acc += len;
    acc = sum16(seg, len, acc);
    return fold(acc);
}

/* ---- protocol constants -------------------------------------------------- */

#define ETH_HDR 14u
#define ETH_TYPE_IP 0x0800u
#define ETH_TYPE_ARP 0x0806u

#define IP_HDR 20u
#define IP_PROTO_ICMP 1u
#define IP_PROTO_TCP 6u
#define IP_PROTO_UDP 17u

#define TCP_FIN 0x01u
#define TCP_SYN 0x02u
#define TCP_RST 0x04u
#define TCP_PSH 0x08u
#define TCP_ACK 0x10u

#define TCP_MSS 1024u
#define TCP_RTO_MIN 400u
#define TCP_RTO_MAX 6400u
#define TCP_MAX_RETRIES 6u

#define DHCP_LOCAL_PORT 68u
#define DHCP_SERVER_PORT 67u
#define DNS_LOCAL_PORT 0xC353u
#define DNS_PORT 53u

/* A queued UDP datagram is framed [len:2][src ip:4][src port:2] then payload,
 * so one receive buffer can hold several datagrams without a second array. */
#define UDP_FRAME_HDR 8u

static const uint8_t g_broadcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

/* ---- initialization ------------------------------------------------------ */

int vibeos_inet_init(vibeos_inet_t *net, const uint8_t mac[6],
                     vibeos_inet_tx_fn tx, void *tx_ctx) {
    uint32_t i;
    if (!net || !mac || !tx) {
        return -1;
    }
    bzero_n((uint8_t *)net, (uint32_t)sizeof(*net));
    bcopy_n(net->mac, mac, 6);
    net->tx = tx;
    net->tx_ctx = tx_ctx;
    net->next_ephemeral = 49152u;
    net->ip_id = 1u;
    for (i = 0; i < VIBEOS_INET_MAX_SOCKETS; i++) {
        net->sockets[i].parent = -1;
    }
    return 0;
}

void vibeos_inet_set_addr(vibeos_inet_t *net, uint32_t ip, uint32_t netmask,
                          uint32_t gateway, uint32_t dns) {
    if (!net) {
        return;
    }
    net->ip = ip;
    net->netmask = netmask;
    net->gateway = gateway;
    net->dns = dns;
}

uint32_t vibeos_inet_parse_ip(const char *s) {
    uint32_t v = 0;
    uint32_t part = 0;
    uint32_t seen = 0;
    uint32_t digits = 0;
    if (!s) {
        return 0;
    }
    for (;;) {
        char c = *s++;
        if (c >= '0' && c <= '9') {
            part = part * 10u + (uint32_t)(c - '0');
            if (part > 255u) {
                return 0;
            }
            digits++;
        } else if (c == '.' || c == 0) {
            if (digits == 0) {
                return 0;
            }
            v = (v << 8) | part;
            part = 0;
            digits = 0;
            seen++;
            if (c == 0) {
                break;
            }
            if (seen > 3u) {
                return 0;
            }
        } else {
            return 0;
        }
    }
    return (seen == 4u) ? v : 0;
}

/* ---- ARP ----------------------------------------------------------------- */

static void arp_insert(vibeos_inet_t *net, uint32_t ip, const uint8_t *mac) {
    uint32_t i;
    uint32_t victim = 0;
    for (i = 0; i < VIBEOS_INET_ARP_ENTRIES; i++) {
        if (net->arp[i].valid && net->arp[i].ip == ip) {
            bcopy_n(net->arp[i].mac, mac, 6);
            net->arp[i].expires_ms = net->now_ms + 60000ull;
            return;
        }
        if (!net->arp[i].valid) {
            victim = i;
        }
    }
    if (net->arp[victim].valid) {
        victim = 0; /* nothing free: evict the first slot */
    }
    net->arp[victim].ip = ip;
    bcopy_n(net->arp[victim].mac, mac, 6);
    net->arp[victim].valid = 1;
    net->arp[victim].expires_ms = net->now_ms + 60000ull;
}

static const uint8_t *arp_find(vibeos_inet_t *net, uint32_t ip) {
    uint32_t i;
    for (i = 0; i < VIBEOS_INET_ARP_ENTRIES; i++) {
        if (net->arp[i].valid && net->arp[i].ip == ip) {
            return net->arp[i].mac;
        }
    }
    return 0;
}

static int eth_send(vibeos_inet_t *net, const uint8_t *dst_mac, uint16_t type,
                    uint32_t payload_len) {
    uint8_t *f = net->scratch;
    if (payload_len + ETH_HDR > VIBEOS_INET_MTU) {
        return -1;
    }
    bcopy_n(f, dst_mac, 6);
    bcopy_n(f + 6, net->mac, 6);
    wr16(f + 12, type);
    net->tx_frames++;
    return net->tx(net->tx_ctx, f, payload_len + ETH_HDR);
}

static void arp_request(vibeos_inet_t *net, uint32_t target) {
    uint8_t *a = net->scratch + ETH_HDR;
    wr16(a + 0, 1u);            /* Ethernet          */
    wr16(a + 2, ETH_TYPE_IP);
    a[4] = 6;
    a[5] = 4;
    wr16(a + 6, 1u);            /* request           */
    bcopy_n(a + 8, net->mac, 6);
    wr32(a + 14, net->ip);
    bzero_n(a + 18, 6);
    wr32(a + 24, target);
    (void)eth_send(net, g_broadcast_mac, ETH_TYPE_ARP, 28u);
}

/* Next hop for a destination: the host itself if on-link, else the gateway. */
static uint32_t next_hop(const vibeos_inet_t *net, uint32_t dst) {
    if (net->netmask != 0u && ((dst ^ net->ip) & net->netmask) == 0u) {
        return dst;
    }
    if (dst == 0xFFFFFFFFu) {
        return dst;
    }
    return (net->gateway != 0u) ? net->gateway : dst;
}

/* ---- IPv4 output --------------------------------------------------------- */

/* Build an IPv4 packet in the scratch buffer and transmit it. The payload must
 * already be at scratch + ETH_HDR + IP_HDR. Returns -VIBEOS_INET_EAGAIN if the
 * destination MAC is not known yet (an ARP request is sent). */
/* The source address is an explicit argument rather than always net->ip.
 *
 * A DHCP client has to source from 0.0.0.0 before it holds a lease. Doing that
 * by blanking net->ip around the send and restoring it afterwards is a trap:
 * the restore writes back a value captured before the transmit, so anything
 * that assigns the lease while the send is in progress gets silently undone.
 * Passing the source down removes the mutable-global step entirely. */
static int ip_send_from(vibeos_inet_t *net, uint32_t src, uint32_t dst, uint8_t proto,
                        uint32_t payload_len) {
    uint8_t *ip = net->scratch + ETH_HDR;
    uint32_t hop;
    const uint8_t *mac;
    uint16_t total = (uint16_t)(IP_HDR + payload_len);

    ip[0] = 0x45;                       /* IPv4, 20-byte header */
    ip[1] = 0;
    wr16(ip + 2, total);
    wr16(ip + 4, net->ip_id++);
    wr16(ip + 6, 0x4000u);              /* don't fragment */
    ip[8] = 64;                         /* TTL */
    ip[9] = proto;
    wr16(ip + 10, 0);
    wr32(ip + 12, src);
    wr32(ip + 16, dst);
    wr16(ip + 10, vibeos_inet_checksum(ip, IP_HDR));

    if (dst == 0xFFFFFFFFu) {
        mac = g_broadcast_mac;
    } else {
        hop = next_hop(net, dst);
        mac = arp_find(net, hop);
        if (!mac) {
            arp_request(net, hop);
            return -VIBEOS_INET_EAGAIN;
        }
    }
    return eth_send(net, mac, ETH_TYPE_IP, total);
}

/* Ordinary traffic sources from the interface address. */
static int ip_send(vibeos_inet_t *net, uint32_t dst, uint8_t proto, uint32_t payload_len) {
    return ip_send_from(net, net->ip, dst, proto, payload_len);
}

/* ---- ICMP ---------------------------------------------------------------- */

static int icmp_echo_send(vibeos_inet_t *net, uint32_t dst, uint16_t id, uint16_t seq) {
    uint8_t *p = net->scratch + ETH_HDR + IP_HDR;
    uint32_t len = 8u + 16u;
    uint32_t i;
    p[0] = 8;   /* echo request */
    p[1] = 0;
    wr16(p + 2, 0);
    wr16(p + 4, id);
    wr16(p + 6, seq);
    for (i = 0; i < 16u; i++) {
        p[8 + i] = (uint8_t)('a' + (i % 26u));
    }
    wr16(p + 2, vibeos_inet_checksum(p, len));
    return ip_send(net, dst, IP_PROTO_ICMP, len);
}

int vibeos_inet_ping(vibeos_inet_t *net, uint32_t ip) {
    if (!net || ip == 0u) {
        return -VIBEOS_INET_EINVAL;
    }
    net->ping_id = (uint16_t)(net->ping_id + 1u);
    net->ping_seq = 1u;
    net->ping_peer = ip;
    net->ping_pending = 1;
    net->ping_replied = 0;
    net->ping_sent_ms = net->now_ms;
    net->ping_rtt_ms = 0;
    (void)icmp_echo_send(net, ip, net->ping_id, net->ping_seq);
    return 0;
}

int vibeos_inet_ping_result(const vibeos_inet_t *net, uint64_t *out_rtt_ms) {
    if (!net || !net->ping_replied) {
        return -VIBEOS_INET_EAGAIN;
    }
    if (out_rtt_ms) {
        *out_rtt_ms = net->ping_rtt_ms;
    }
    return 0;
}

static void icmp_input(vibeos_inet_t *net, uint32_t src, const uint8_t *p, uint32_t len) {
    if (len < 8u) {
        return;
    }
    if (p[0] == 8u) { /* echo request: answer it */
        uint8_t *o = net->scratch + ETH_HDR + IP_HDR;
        uint32_t n = (len > (VIBEOS_INET_MTU - ETH_HDR - IP_HDR)) ?
                     (VIBEOS_INET_MTU - ETH_HDR - IP_HDR) : len;
        uint32_t i;
        for (i = 0; i < n; i++) {
            o[i] = p[i];
        }
        o[0] = 0;   /* echo reply */
        wr16(o + 2, 0);
        wr16(o + 2, vibeos_inet_checksum(o, n));
        (void)ip_send(net, src, IP_PROTO_ICMP, n);
        return;
    }
    if (p[0] == 0u && net->ping_pending) { /* echo reply */
        if (rd16(p + 4) == net->ping_id && src == net->ping_peer) {
            net->ping_pending = 0;
            net->ping_replied = 1;
            net->ping_rtt_ms = net->now_ms - net->ping_sent_ms;
        }
    }
}

/* ---- socket table -------------------------------------------------------- */

static vibeos_inet_socket_t *sock_at(vibeos_inet_t *net, int s) {
    if (!net || s < 0 || (uint32_t)s >= VIBEOS_INET_MAX_SOCKETS) {
        return 0;
    }
    return net->sockets[s].used ? &net->sockets[s] : 0;
}

static int sock_alloc(vibeos_inet_t *net, int type) {
    uint32_t i;
    for (i = 0; i < VIBEOS_INET_MAX_SOCKETS; i++) {
        if (!net->sockets[i].used) {
            vibeos_inet_socket_t *s = &net->sockets[i];
            bzero_n((uint8_t *)s, (uint32_t)sizeof(*s));
            s->used = 1;
            s->type = (uint8_t)type;
            s->state = VIBEOS_TCP_CLOSED;
            s->parent = -1;
            s->snd_wnd = 4096u;
            s->rto_ms = TCP_RTO_MIN;
            return (int)i;
        }
    }
    return -VIBEOS_INET_ENOBUFS;
}

static uint16_t ephemeral(vibeos_inet_t *net) {
    uint16_t p = net->next_ephemeral++;
    if (net->next_ephemeral == 0u || net->next_ephemeral < 49152u) {
        net->next_ephemeral = 49152u;
    }
    return p;
}

int vibeos_inet_socket(vibeos_inet_t *net, int type) {
    if (!net || (type != VIBEOS_INET_SOCK_UDP && type != VIBEOS_INET_SOCK_TCP)) {
        return -VIBEOS_INET_EINVAL;
    }
    return sock_alloc(net, type);
}

int vibeos_inet_bind(vibeos_inet_t *net, int sock, uint16_t port) {
    vibeos_inet_socket_t *s = sock_at(net, sock);
    if (!s) {
        return -VIBEOS_INET_EINVAL;
    }
    s->local_port = port;
    return 0;
}

int vibeos_inet_socket_state(const vibeos_inet_t *net, int sock) {
    if (!net || sock < 0 || (uint32_t)sock >= VIBEOS_INET_MAX_SOCKETS) {
        return -VIBEOS_INET_EINVAL;
    }
    if (!net->sockets[sock].used) {
        return -VIBEOS_INET_EINVAL;
    }
    return (int)net->sockets[sock].state;
}

/* ---- UDP ----------------------------------------------------------------- */

static int udp_send_from(vibeos_inet_t *net, uint32_t src, uint32_t dst,
                         uint16_t sport, uint16_t dport,
                         const uint8_t *data, uint32_t len) {
    uint8_t *u = net->scratch + ETH_HDR + IP_HDR;
    uint32_t i;
    uint16_t ck;

    if (len + 8u + IP_HDR + ETH_HDR > VIBEOS_INET_MTU) {
        return -VIBEOS_INET_EINVAL;
    }
    wr16(u + 0, sport);
    wr16(u + 2, dport);
    wr16(u + 4, (uint16_t)(len + 8u));
    wr16(u + 6, 0);
    for (i = 0; i < len; i++) {
        u[8 + i] = data[i];
    }
    ck = l4_checksum(src, dst, IP_PROTO_UDP, u, len + 8u);
    if (ck == 0u) {
        ck = 0xFFFFu;   /* 0 means "no checksum" on the wire */
    }
    wr16(u + 6, ck);
    return ip_send_from(net, src, dst, IP_PROTO_UDP, len + 8u);
}

static int udp_send(vibeos_inet_t *net, uint32_t dst, uint16_t sport, uint16_t dport,
                    const uint8_t *data, uint32_t len) {
    return udp_send_from(net, net->ip, dst, sport, dport, data, len);
}

long vibeos_inet_sendto(vibeos_inet_t *net, int sock, const void *buf, uint32_t len,
                        uint32_t ip, uint16_t port) {
    vibeos_inet_socket_t *s = sock_at(net, sock);
    int r;
    if (!s || s->type != VIBEOS_INET_SOCK_UDP || !buf) {
        return -VIBEOS_INET_EINVAL;
    }
    if (s->local_port == 0u) {
        s->local_port = ephemeral(net);
    }
    r = udp_send(net, ip, s->local_port, port, (const uint8_t *)buf, len);
    if (r != 0) {
        return r;
    }
    return (long)len;
}

long vibeos_inet_recvfrom(vibeos_inet_t *net, int sock, void *buf, uint32_t len,
                          uint32_t *out_ip, uint16_t *out_port) {
    vibeos_inet_socket_t *s = sock_at(net, sock);
    uint32_t n, i;
    if (!s || !buf) {
        return -VIBEOS_INET_EINVAL;
    }
    if (s->rx_len < UDP_FRAME_HDR) {
        return -VIBEOS_INET_EAGAIN;
    }
    /* One call drains exactly one queued datagram, header and all. */
    {
        uint32_t dlen = rd16(s->rx);
        uint32_t src_ip = rd32(s->rx + 2);
        uint16_t src_port = rd16(s->rx + 6);
        uint32_t total = UDP_FRAME_HDR + dlen;
        uint32_t copied;

        if (total > s->rx_len) {
            s->rx_len = 0;             /* corrupt framing: drop the queue */
            return -VIBEOS_INET_EAGAIN;
        }
        /* A datagram larger than the caller's buffer is truncated, and the
         * remainder discarded - datagram semantics, not stream semantics. */
        copied = (dlen < len) ? dlen : len;
        for (i = 0; i < copied; i++) {
            ((uint8_t *)buf)[i] = s->rx[UDP_FRAME_HDR + i];
        }
        for (i = total; i < s->rx_len; i++) {
            s->rx[i - total] = s->rx[i];
        }
        s->rx_len -= total;
        s->last_src_ip = src_ip;
        s->last_src_port = src_port;
        if (out_ip) {
            *out_ip = src_ip;
        }
        if (out_port) {
            *out_port = src_port;
        }
        n = copied;
    }
    return (long)n;
}

/* ---- DHCP client --------------------------------------------------------- */

static uint32_t dhcp_build(vibeos_inet_t *net, uint8_t *b, uint8_t msg_type,
                           uint32_t requested_ip, uint32_t server_ip) {
    uint32_t o;
    bzero_n(b, 300u);
    b[0] = 1;                       /* BOOTREQUEST                     */
    b[1] = 1;                       /* Ethernet                        */
    b[2] = 6;                       /* hardware address length         */
    b[3] = 0;
    wr32(b + 4, net->dhcp_xid);
    wr16(b + 8, 0);
    wr16(b + 10, 0x8000u);          /* broadcast reply                 */
    bcopy_n(b + 28, net->mac, 6);
    wr32(b + 236, 0x63825363u);     /* magic cookie                    */

    o = 240u;
    b[o++] = 53; b[o++] = 1; b[o++] = msg_type;
    if (requested_ip != 0u) {
        b[o++] = 50; b[o++] = 4; wr32(b + o, requested_ip); o += 4u;
    }
    if (server_ip != 0u) {
        b[o++] = 54; b[o++] = 4; wr32(b + o, server_ip); o += 4u;
    }
    b[o++] = 55; b[o++] = 3; b[o++] = 1; b[o++] = 3; b[o++] = 6;  /* mask, router, dns */
    b[o++] = 255;                   /* end                             */
    while (o < 300u) {
        b[o++] = 0;
    }
    return o;
}

static void dhcp_send(vibeos_inet_t *net, uint8_t msg_type, uint32_t req, uint32_t srv) {
    uint8_t body[300];
    uint32_t n = dhcp_build(net, body, msg_type, req, srv);
    /* A client without a lease sources from 0.0.0.0. Passing that explicitly
     * leaves net->ip untouched, so a lease assigned while this send is in
     * flight cannot be undone by a restore. */
    (void)udp_send_from(net, 0u, 0xFFFFFFFFu, DHCP_LOCAL_PORT, DHCP_SERVER_PORT, body, n);
    net->dhcp_retry_ms = net->now_ms + 1000ull;
}

int vibeos_inet_dhcp_start(vibeos_inet_t *net) {
    if (!net) {
        return -VIBEOS_INET_EINVAL;
    }
    net->dhcp_xid = 0x56494245u ^ ((uint32_t)net->mac[4] << 8) ^ net->mac[5];
    net->dhcp_state = 1;
    net->ip = 0;
    net->netmask = 0;
    net->gateway = 0;
    dhcp_send(net, 1 /* DISCOVER */, 0, 0);
    return 0;
}

int vibeos_inet_dhcp_bound(const vibeos_inet_t *net) {
    return (net && net->dhcp_state == 3u) ? 1 : 0;
}

static void dhcp_input(vibeos_inet_t *net, const uint8_t *b, uint32_t len) {
    uint32_t o = 240u;
    uint8_t msg_type = 0;
    uint32_t mask = 0, router = 0, dns = 0, server = 0;
    uint32_t lease = 0, t1 = 0, t2 = 0;

    if (len < 241u || b[0] != 2u || rd32(b + 4) != net->dhcp_xid) {
        return;
    }
    if (rd32(b + 236) != 0x63825363u) {
        return;
    }
    while (o + 1u < len) {
        uint8_t code = b[o];
        uint8_t olen;
        if (code == 255u) {
            break;
        }
        if (code == 0u) {
            o++;
            continue;
        }
        olen = b[o + 1u];
        if (o + 2u + olen > len) {
            break;
        }
        switch (code) {
            case 1:  if (olen == 4u) { mask = rd32(b + o + 2u); } break;
            case 3:  if (olen >= 4u) { router = rd32(b + o + 2u); } break;
            case 6:  if (olen >= 4u) { dns = rd32(b + o + 2u); } break;
            case 51: if (olen == 4u) { lease = rd32(b + o + 2u); } break;
            case 53: if (olen == 1u) { msg_type = b[o + 2u]; } break;
            case 54: if (olen == 4u) { server = rd32(b + o + 2u); } break;
            case 58: if (olen == 4u) { t1 = rd32(b + o + 2u); } break;
            case 59: if (olen == 4u) { t2 = rd32(b + o + 2u); } break;
            default: break;
        }
        o += 2u + olen;
    }

    if (msg_type == 2u && net->dhcp_state == 1u) {          /* OFFER */
        net->dhcp_offer_ip = rd32(b + 16);                  /* yiaddr */
        net->dhcp_server = server;
        net->dhcp_state = 2;
        dhcp_send(net, 3 /* REQUEST */, net->dhcp_offer_ip, server);
    } else if (msg_type == 5u &&
               (net->dhcp_state == 2u || net->dhcp_state == 4u || net->dhcp_state == 5u)) {
        /* ACK: a fresh lease, or a renewal of the one we hold. */
        uint32_t yiaddr = rd32(b + 16);
        if (net->dhcp_state != 2u) {
            net->dhcp_renewals++;
        }
        if (yiaddr != 0u) {
            net->ip = yiaddr;
        }
        net->netmask = mask ? mask : 0xFFFFFF00u;
        if (router) {
            net->gateway = router;
        }
        if (dns) {
            net->dns = dns;
        } else if (router && net->dns == 0u) {
            net->dns = router;
        }
        if (server) {
            net->dhcp_server = server;
        }
        /* A lease is finite. Default to an hour when the server omits it, and
         * derive the standard renew/rebind points when it omits those too. */
        net->dhcp_lease_secs = lease ? lease : 3600u;
        if (t1 == 0u) {
            t1 = net->dhcp_lease_secs / 2u;
        }
        if (t2 == 0u) {
            t2 = (net->dhcp_lease_secs * 7u) / 8u;
        }
        net->dhcp_t1_ms = net->now_ms + (uint64_t)t1 * 1000ull;
        net->dhcp_t2_ms = net->now_ms + (uint64_t)t2 * 1000ull;
        net->dhcp_expire_ms = net->now_ms + (uint64_t)net->dhcp_lease_secs * 1000ull;
        net->dhcp_state = 3;
    } else if (msg_type == 6u && net->dhcp_state != 0u) {   /* NAK */
        /* The server refused: drop everything and start over rather than keep
         * using an address it does not agree we hold. */
        net->ip = 0;
        net->netmask = 0;
        net->gateway = 0;
        net->dhcp_offer_ip = 0;
        net->dhcp_state = 1;
        dhcp_send(net, 1 /* DISCOVER */, 0, 0);
    }
}

/* ---- DNS ----------------------------------------------------------------- */

static uint32_t dns_encode_name(uint8_t *out, const char *name) {
    uint32_t o = 0;
    uint32_t label_start = 0;
    uint32_t i = 0;
    for (;;) {
        char c = name[i];
        if (c == '.' || c == 0) {
            uint32_t label_len = i - label_start;
            if (label_len == 0u || label_len > 63u) {
                if (c == 0 && label_len == 0u) {
                    break;
                }
                return 0;
            }
            out[o++] = (uint8_t)label_len;
            {
                uint32_t k;
                for (k = 0; k < label_len; k++) {
                    out[o++] = (uint8_t)name[label_start + k];
                }
            }
            label_start = i + 1u;
            if (c == 0) {
                break;
            }
        }
        if (c == 0) {
            break;
        }
        i++;
        if (i > 200u) {
            return 0;
        }
    }
    out[o++] = 0;
    return o;
}

static int dns_name_equal(const char *a, const char *b) {
    uint32_t i;
    for (i = 0; i < 64u; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
        if (a[i] == 0) {
            return 1;
        }
    }
    return 0;
}

static void dns_cache_store(vibeos_inet_t *net, uint32_t ip, uint32_t ttl_seconds) {
    uint32_t i;
    uint32_t slot = 0;
    uint64_t ttl_ms;

    /* Keep cache residency bounded even when an upstream response advertises
     * an unrealistic TTL. */
    if (ttl_seconds == 0u) {
        return;
    }
    if (ttl_seconds > 86400u) {
        ttl_seconds = 86400u;
    }
    for (i = 0; i < VIBEOS_INET_DNS_CACHE_ENTRIES; i++) {
        if (net->dns_cache[i].valid && dns_name_equal(net->dns_cache[i].name, net->dns_name)) {
            slot = i;
            break;
        }
        if (!net->dns_cache[i].valid || net->dns_cache[i].expires_ms <= net->now_ms) {
            slot = i;
            break;
        }
    }
    ttl_ms = (uint64_t)ttl_seconds * 1000ull;
    for (i = 0; i < sizeof(net->dns_cache[slot].name) - 1u && net->dns_name[i]; i++) {
        net->dns_cache[slot].name[i] = net->dns_name[i];
    }
    net->dns_cache[slot].name[i] = 0;
    net->dns_cache[slot].ip = ip;
    net->dns_cache[slot].expires_ms = net->now_ms + ttl_ms;
    net->dns_cache[slot].valid = 1;
}

/* Build and transmit the query for the name currently being resolved. Split out
 * so a retry re-sends exactly the same question with the same id. */
static int dns_send_query(vibeos_inet_t *net) {
    uint8_t q[300];
    uint32_t n;

    wr16(q + 0, net->dns_id);
    wr16(q + 2, 0x0100u);   /* standard query, recursion desired */
    wr16(q + 4, 1u);        /* one question                      */
    wr16(q + 6, 0);
    wr16(q + 8, 0);
    wr16(q + 10, 0);
    n = dns_encode_name(q + 12, net->dns_name);
    if (n == 0u) {
        return -VIBEOS_INET_EINVAL;
    }
    n += 12u;
    wr16(q + n, 1u);        /* A     */
    wr16(q + n + 2u, 1u);   /* IN    */
    n += 4u;
    net->dns_retry_ms = net->now_ms + 1000ull;
    (void)udp_send(net, net->dns, DNS_LOCAL_PORT, DNS_PORT, q, n);
    return 0;
}

int vibeos_inet_resolve(vibeos_inet_t *net, const char *name) {
    uint32_t i;

    if (!net || !name || net->dns == 0u) {
        return -VIBEOS_INET_EINVAL;
    }
    for (i = 0; i < sizeof(net->dns_name) - 1u && name[i]; i++) {
        net->dns_name[i] = name[i];
    }
    net->dns_name[i] = 0;
    for (i = 0; i < VIBEOS_INET_DNS_CACHE_ENTRIES; i++) {
        vibeos_dns_cache_entry_t *entry = &net->dns_cache[i];
        if (entry->valid && entry->expires_ms > net->now_ms &&
            dns_name_equal(entry->name, net->dns_name)) {
            net->dns_pending = 0;
            net->dns_done = 1;
            net->dns_result = entry->ip;
            return 0;
        }
    }

    net->dns_id = (uint16_t)(net->dns_id + 0x1234u + 1u);
    net->dns_pending = 1;
    net->dns_done = 0;
    net->dns_result = 0;
    net->dns_retries = 0;

    if (dns_send_query(net) != 0) {
        net->dns_pending = 0;
        return -VIBEOS_INET_EINVAL;
    }
    return 0;
}

int vibeos_inet_resolve_result(const vibeos_inet_t *net, uint32_t *out_ip) {
    if (!net || !net->dns_done) {
        return -VIBEOS_INET_EAGAIN;
    }
    if (out_ip) {
        *out_ip = net->dns_result;
    }
    return net->dns_result ? 0 : -VIBEOS_INET_ETIMEDOUT;
}

/* Skip a (possibly compressed) name; returns the offset just past it. */
static uint32_t dns_skip_name(const uint8_t *b, uint32_t len, uint32_t o) {
    while (o < len) {
        uint8_t l = b[o];
        if (l == 0u) {
            return o + 1u;
        }
        if ((l & 0xC0u) == 0xC0u) {
            return o + 2u;   /* pointer: two bytes, name continues elsewhere */
        }
        o += 1u + l;
    }
    return len;
}

static void dns_input(vibeos_inet_t *net, const uint8_t *b, uint32_t len) {
    uint32_t qd, an, o, i;

    if (!net->dns_pending || len < 12u || rd16(b) != net->dns_id) {
        return;
    }
    qd = rd16(b + 4);
    an = rd16(b + 6);
    o = 12u;
    for (i = 0; i < qd && o < len; i++) {
        o = dns_skip_name(b, len, o);
        o += 4u;   /* qtype + qclass */
    }
    for (i = 0; i < an && o + 10u <= len; i++) {
        uint16_t rtype, rdlen;
        uint32_t ttl;
        o = dns_skip_name(b, len, o);
        if (o + 10u > len) {
            break;
        }
        rtype = rd16(b + o);
        ttl = rd32(b + o + 4u);
        rdlen = rd16(b + o + 8u);
        o += 10u;
        if (o + rdlen > len) {
            break;
        }
        if (rtype == 1u && rdlen == 4u) {
            net->dns_result = rd32(b + o);
            dns_cache_store(net, net->dns_result, ttl);
            net->dns_pending = 0;
            net->dns_done = 1;
            return;
        }
        o += rdlen;
    }
    /* Answered, but with nothing usable. */
    net->dns_pending = 0;
    net->dns_done = 1;
}

/* ---- UDP input ----------------------------------------------------------- */

static void udp_input(vibeos_inet_t *net, uint32_t src, uint32_t dst,
                      const uint8_t *u, uint32_t len) {
    uint16_t sport, dport;
    const uint8_t *data;
    uint32_t dlen;
    uint32_t i;

    if (len < 8u) {
        return;
    }
    sport = rd16(u + 0);
    dport = rd16(u + 2);
    dlen = rd16(u + 4);
    if (dlen < 8u || dlen > len) {
        return;
    }
    /* IPv4 permits a zero UDP checksum, but a supplied checksum is mandatory. */
    if (rd16(u + 6) != 0u && l4_checksum(src, dst, IP_PROTO_UDP, u, dlen) != 0u) {
        net->rx_dropped++;
        return;
    }
    data = u + 8;
    dlen -= 8u;

    if (dport == DHCP_LOCAL_PORT) {
        dhcp_input(net, data, dlen);
        return;
    }
    if (dport == DNS_LOCAL_PORT) {
        dns_input(net, data, dlen);
        return;
    }

    for (i = 0; i < VIBEOS_INET_MAX_SOCKETS; i++) {
        vibeos_inet_socket_t *s = &net->sockets[i];
        if (s->used && s->type == VIBEOS_INET_SOCK_UDP && s->local_port == dport) {
            uint32_t k;
            /* Queue the datagram behind any already waiting. A full queue drops
             * the newest and is counted: UDP may lose datagrams, but it must
             * never corrupt or silently overwrite one already delivered. */
            if (dlen + UDP_FRAME_HDR > VIBEOS_INET_RXBUF - s->rx_len) {
                s->udp_dropped++;
                net->rx_dropped++;
                return;
            }
            wr16(s->rx + s->rx_len, (uint16_t)dlen);
            wr32(s->rx + s->rx_len + 2u, src);
            wr16(s->rx + s->rx_len + 6u, sport);
            for (k = 0; k < dlen; k++) {
                s->rx[s->rx_len + UDP_FRAME_HDR + k] = data[k];
            }
            s->rx_len += UDP_FRAME_HDR + dlen;
            s->last_src_ip = src;
            s->last_src_port = sport;
            return;
        }
    }
    net->rx_dropped++;
}

/* ---- TCP ----------------------------------------------------------------- */

/* Emit one TCP segment. `data`/`dlen` is the payload; flags carry SYN/FIN/etc. */
static int tcp_send_seg(vibeos_inet_t *net, vibeos_inet_socket_t *s, uint8_t flags,
                        uint32_t seq, const uint8_t *data, uint32_t dlen) {
    uint8_t *t = net->scratch + ETH_HDR + IP_HDR;
    uint32_t i;
    uint32_t win = VIBEOS_INET_RXBUF - s->rx_len;

    if (dlen + 20u + IP_HDR + ETH_HDR > VIBEOS_INET_MTU) {
        return -VIBEOS_INET_EINVAL;
    }
    wr16(t + 0, s->local_port);
    wr16(t + 2, s->remote_port);
    wr32(t + 4, seq);
    wr32(t + 8, (flags & TCP_ACK) ? s->rcv_nxt : 0u);
    t[12] = 0x50;                       /* data offset = 5 words, no options */
    t[13] = flags;
    wr16(t + 14, (uint16_t)(win > 0xFFFFu ? 0xFFFFu : win));
    wr16(t + 16, 0);
    wr16(t + 18, 0);
    for (i = 0; i < dlen; i++) {
        t[20 + i] = data[i];
    }
    wr16(t + 16, l4_checksum(net->ip, s->remote_ip, IP_PROTO_TCP, t, 20u + dlen));
    return ip_send(net, s->remote_ip, IP_PROTO_TCP, 20u + dlen);
}

/* Send whatever is queued and unsent, and arm the retransmission timer. */
static void tcp_flush(vibeos_inet_t *net, vibeos_inet_socket_t *s) {
    uint32_t unsent = s->snd_una + s->tx_len - s->snd_nxt;
    uint32_t off, n;

    while (unsent > 0u) {
        n = (unsent > TCP_MSS) ? TCP_MSS : unsent;
        if (n > s->snd_wnd) {
            n = s->snd_wnd ? s->snd_wnd : 1u;
        }
        off = s->snd_nxt - s->snd_una;
        if (tcp_send_seg(net, s, TCP_ACK | TCP_PSH, s->snd_nxt, s->tx + off, n) != 0) {
            return;   /* ARP pending: the retransmit timer will try again */
        }
        s->snd_nxt += n;
        unsent -= n;
    }
    if (s->tx_len > 0u && s->rto_deadline_ms == 0u) {
        s->rto_deadline_ms = net->now_ms + s->rto_ms;
    }
}

int vibeos_inet_connect(vibeos_inet_t *net, int sock, uint32_t ip, uint16_t port) {
    vibeos_inet_socket_t *s = sock_at(net, sock);
    if (!s || s->type != VIBEOS_INET_SOCK_TCP) {
        return -VIBEOS_INET_EINVAL;
    }
    if (s->state != VIBEOS_TCP_CLOSED) {
        return -VIBEOS_INET_EINVAL;
    }
    if (s->local_port == 0u) {
        s->local_port = ephemeral(net);
    }
    s->remote_ip = ip;
    s->remote_port = port;
    s->snd_una = 0x1000u + (uint32_t)net->now_ms;   /* initial sequence number */
    s->snd_nxt = s->snd_una;
    s->state = VIBEOS_TCP_SYN_SENT;
    s->retries = 0;
    s->rto_ms = TCP_RTO_MIN;
    s->rto_deadline_ms = net->now_ms + s->rto_ms;
    (void)tcp_send_seg(net, s, TCP_SYN, s->snd_nxt, 0, 0);
    s->snd_nxt++;   /* SYN consumes one sequence number */
    return 0;
}

int vibeos_inet_listen(vibeos_inet_t *net, int sock) {
    vibeos_inet_socket_t *s = sock_at(net, sock);
    if (!s || s->type != VIBEOS_INET_SOCK_TCP || s->local_port == 0u) {
        return -VIBEOS_INET_EINVAL;
    }
    s->state = VIBEOS_TCP_LISTEN;
    s->backlog_len = 0;
    return 0;
}

int vibeos_inet_accept(vibeos_inet_t *net, int sock) {
    vibeos_inet_socket_t *s = sock_at(net, sock);
    int child;
    uint32_t i;
    if (!s || s->state != VIBEOS_TCP_LISTEN) {
        return -VIBEOS_INET_EINVAL;
    }
    if (s->backlog_len == 0u) {
        return -VIBEOS_INET_EAGAIN;
    }
    child = s->backlog[0];
    for (i = 1; i < s->backlog_len; i++) {
        s->backlog[i - 1u] = s->backlog[i];
    }
    s->backlog_len--;
    return child;
}

long vibeos_inet_send(vibeos_inet_t *net, int sock, const void *buf, uint32_t len) {
    vibeos_inet_socket_t *s = sock_at(net, sock);
    uint32_t room, n, i;

    if (!s || !buf) {
        return -VIBEOS_INET_EINVAL;
    }
    if (s->type != VIBEOS_INET_SOCK_TCP) {
        return -VIBEOS_INET_EINVAL;
    }
    if (s->reset) {
        return -VIBEOS_INET_ECONNRESET;
    }
    if (s->state != VIBEOS_TCP_ESTABLISHED && s->state != VIBEOS_TCP_CLOSE_WAIT) {
        return -VIBEOS_INET_ENOTCONN;
    }
    room = VIBEOS_INET_TXBUF - s->tx_len;
    if (room == 0u) {
        return -VIBEOS_INET_EAGAIN;
    }
    n = (len > room) ? room : len;
    for (i = 0; i < n; i++) {
        s->tx[s->tx_len + i] = ((const uint8_t *)buf)[i];
    }
    s->tx_len += n;
    tcp_flush(net, s);
    return (long)n;
}

long vibeos_inet_recv(vibeos_inet_t *net, int sock, void *buf, uint32_t len) {
    vibeos_inet_socket_t *s = sock_at(net, sock);
    uint32_t n, i;

    if (!s || !buf) {
        return -VIBEOS_INET_EINVAL;
    }
    if (s->rx_len == 0u) {
        if (s->reset) {
            return -VIBEOS_INET_ECONNRESET;
        }
        if (s->fin_received) {
            return 0;   /* orderly shutdown: end of stream */
        }
        return -VIBEOS_INET_EAGAIN;
    }
    n = (s->rx_len < len) ? s->rx_len : len;
    for (i = 0; i < n; i++) {
        ((uint8_t *)buf)[i] = s->rx[i];
    }
    for (i = n; i < s->rx_len; i++) {
        s->rx[i - n] = s->rx[i];
    }
    s->rx_len -= n;
    return (long)n;
}

int vibeos_inet_close(vibeos_inet_t *net, int sock) {
    vibeos_inet_socket_t *s = sock_at(net, sock);
    if (!s) {
        return -VIBEOS_INET_EINVAL;
    }
    if (s->type == VIBEOS_INET_SOCK_TCP &&
        (s->state == VIBEOS_TCP_ESTABLISHED || s->state == VIBEOS_TCP_CLOSE_WAIT)) {
        uint8_t was_close_wait = (s->state == VIBEOS_TCP_CLOSE_WAIT);
        (void)tcp_send_seg(net, s, TCP_ACK | TCP_FIN, s->snd_nxt, 0, 0);
        s->snd_nxt++;
        s->fin_sent = 1;
        s->state = was_close_wait ? VIBEOS_TCP_LAST_ACK : VIBEOS_TCP_FIN_WAIT_1;
        /* If the peer never finishes the exchange, the slot is still reclaimed
         * instead of being held for the life of the system. */
        s->close_deadline_ms = net->now_ms + VIBEOS_INET_TIME_WAIT_MS;
        return 0;
    }
    s->used = 0;
    s->state = VIBEOS_TCP_CLOSED;
    return 0;
}

/* Find the socket a segment belongs to: an exact four-tuple match first, then a
 * listening socket on the destination port. */
static vibeos_inet_socket_t *tcp_lookup(vibeos_inet_t *net, uint32_t src, uint16_t sport,
                                        uint16_t dport, int *out_index) {
    uint32_t i;
    for (i = 0; i < VIBEOS_INET_MAX_SOCKETS; i++) {
        vibeos_inet_socket_t *s = &net->sockets[i];
        if (s->used && s->type == VIBEOS_INET_SOCK_TCP && s->local_port == dport &&
            s->remote_port == sport && s->remote_ip == src &&
            s->state != VIBEOS_TCP_LISTEN) {
            if (out_index) {
                *out_index = (int)i;
            }
            return s;
        }
    }
    for (i = 0; i < VIBEOS_INET_MAX_SOCKETS; i++) {
        vibeos_inet_socket_t *s = &net->sockets[i];
        if (s->used && s->type == VIBEOS_INET_SOCK_TCP &&
            s->state == VIBEOS_TCP_LISTEN && s->local_port == dport) {
            if (out_index) {
                *out_index = (int)i;
            }
            return s;
        }
    }
    return 0;
}

/* Append in-order bytes to the receive buffer, as much as there is room for. */
static void tcp_deliver(vibeos_inet_socket_t *s, const uint8_t *data, uint32_t dlen) {
    uint32_t room = VIBEOS_INET_RXBUF - s->rx_len;
    uint32_t n = (dlen > room) ? room : dlen;
    uint32_t i;
    for (i = 0; i < n; i++) {
        s->rx[s->rx_len + i] = data[i];
    }
    s->rx_len += n;
    s->rcv_nxt += n;
}

/* Hold a segment that arrived ahead of the gap. Duplicates and anything too
 * large for a slot are dropped: the peer will retransmit. */
static void tcp_hold_ooo(vibeos_inet_socket_t *s, uint32_t seq,
                         const uint8_t *data, uint32_t dlen) {
    uint32_t i, k;
    if (dlen == 0u || dlen > VIBEOS_INET_TCP_MSS) {
        return;
    }
    for (i = 0; i < VIBEOS_INET_OOO_SLOTS; i++) {
        if (s->ooo[i].used && s->ooo[i].seq == seq) {
            return;   /* already held */
        }
    }
    for (i = 0; i < VIBEOS_INET_OOO_SLOTS; i++) {
        if (!s->ooo[i].used) {
            s->ooo[i].seq = seq;
            s->ooo[i].len = dlen;
            s->ooo[i].used = 1;
            for (k = 0; k < dlen; k++) {
                s->ooo[i].data[k] = data[k];
            }
            return;
        }
    }
}

/* Deliver every held segment that now starts exactly at rcv_nxt, repeatedly:
 * releasing one can make the next one contiguous too. */
static void tcp_drain_ooo(vibeos_inet_socket_t *s) {
    int progress = 1;
    while (progress) {
        uint32_t i;
        progress = 0;
        for (i = 0; i < VIBEOS_INET_OOO_SLOTS; i++) {
            if (!s->ooo[i].used) {
                continue;
            }
            if (s->ooo[i].seq == s->rcv_nxt) {
                tcp_deliver(s, s->ooo[i].data, s->ooo[i].len);
                s->ooo[i].used = 0;
                progress = 1;
            } else if ((int32_t)(s->ooo[i].seq - s->rcv_nxt) < 0) {
                s->ooo[i].used = 0;   /* now stale: already delivered */
            }
        }
    }
}

/* Drop acknowledged bytes off the front of the send buffer. */
static void tcp_ack_data(vibeos_inet_t *net, vibeos_inet_socket_t *s, uint32_t ack) {
    uint32_t acked = ack - s->snd_una;
    uint32_t i;

    if (acked == 0u || acked > s->tx_len + 2u) {
        return;   /* nothing new, or beyond anything we sent */
    }
    if (acked > s->tx_len) {
        acked = s->tx_len;   /* the rest acknowledged a SYN or FIN */
    }
    for (i = acked; i < s->tx_len; i++) {
        s->tx[i - acked] = s->tx[i];
    }
    s->tx_len -= acked;
    s->snd_una = ack;
    s->retries = 0;
    s->rto_ms = TCP_RTO_MIN;
    s->rto_deadline_ms = (s->tx_len > 0u) ? (net->now_ms + s->rto_ms) : 0u;
}

static void tcp_input(vibeos_inet_t *net, uint32_t src, uint32_t dst,
                      const uint8_t *t, uint32_t len) {
    uint16_t sport, dport;
    uint32_t seq, ack;
    uint8_t flags;
    uint32_t hdr, dlen;
    const uint8_t *data;
    vibeos_inet_socket_t *s;
    int idx = -1;

    if (len < 20u) {
        return;
    }
    sport = rd16(t + 0);
    dport = rd16(t + 2);
    seq = rd32(t + 4);
    ack = rd32(t + 8);
    hdr = (uint32_t)(t[12] >> 4) * 4u;
    flags = t[13];
    if (hdr < 20u || hdr > len) {
        return;
    }
    if (l4_checksum(src, dst, IP_PROTO_TCP, t, len) != 0u) {
        net->rx_dropped++;
        return;
    }
    data = t + hdr;
    dlen = len - hdr;

    s = tcp_lookup(net, src, sport, dport, &idx);
    if (!s) {
        net->rx_dropped++;
        return;
    }
    s->snd_wnd = rd16(t + 14);
    if (s->snd_wnd == 0u) {
        s->snd_wnd = 1u;   /* keep a one-byte probe possible */
    }

    if (flags & TCP_RST) {
        s->reset = 1;
        s->state = VIBEOS_TCP_CLOSED;
        return;
    }

    /* A SYN to a listening socket opens a new connection. */
    if (s->state == VIBEOS_TCP_LISTEN) {
        int c;
        vibeos_inet_socket_t *cs;
        if (!(flags & TCP_SYN) || s->backlog_len >= VIBEOS_INET_BACKLOG) {
            return;
        }
        c = sock_alloc(net, VIBEOS_INET_SOCK_TCP);
        if (c < 0) {
            return;
        }
        cs = &net->sockets[c];
        cs->local_port = dport;
        cs->remote_port = sport;
        cs->remote_ip = src;
        cs->rcv_nxt = seq + 1u;
        cs->snd_una = 0x2000u + (uint32_t)net->now_ms;
        cs->snd_nxt = cs->snd_una;
        cs->state = VIBEOS_TCP_SYN_RECEIVED;
        cs->parent = idx;
        (void)tcp_send_seg(net, cs, TCP_SYN | TCP_ACK, cs->snd_nxt, 0, 0);
        cs->snd_nxt++;
        cs->rto_deadline_ms = net->now_ms + cs->rto_ms;
        return;
    }

    switch (s->state) {
        case VIBEOS_TCP_SYN_SENT:
            if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK) && ack == s->snd_nxt) {
                s->rcv_nxt = seq + 1u;
                s->snd_una = ack;
                s->state = VIBEOS_TCP_ESTABLISHED;
                s->rto_deadline_ms = 0;
                s->retries = 0;
                (void)tcp_send_seg(net, s, TCP_ACK, s->snd_nxt, 0, 0);
                tcp_flush(net, s);
            }
            return;

        case VIBEOS_TCP_SYN_RECEIVED:
            if ((flags & TCP_ACK) && ack == s->snd_nxt) {
                s->snd_una = ack;
                s->state = VIBEOS_TCP_ESTABLISHED;
                s->rto_deadline_ms = 0;
                if (s->parent >= 0) {
                    vibeos_inet_socket_t *p = &net->sockets[s->parent];
                    if (p->used && p->backlog_len < VIBEOS_INET_BACKLOG) {
                        p->backlog[p->backlog_len++] = idx;
                    }
                }
            }
            break;

        case VIBEOS_TCP_FIN_WAIT_1:
            if ((flags & TCP_ACK) && ack == s->snd_nxt) {
                s->state = VIBEOS_TCP_FIN_WAIT_2;
            }
            break;

        case VIBEOS_TCP_LAST_ACK:
            if ((flags & TCP_ACK) && ack == s->snd_nxt) {
                s->state = VIBEOS_TCP_CLOSED;
                s->used = 0;
                return;
            }
            break;

        default:
            break;
    }

    if (flags & TCP_ACK) {
        tcp_ack_data(net, s, ack);
    }

    if (dlen > 0u) {
        if (seq == s->rcv_nxt) {
            tcp_deliver(s, data, dlen);
            tcp_drain_ooo(s);        /* a filled gap may release held segments */
        } else if ((int32_t)(seq - s->rcv_nxt) > 0) {
            tcp_hold_ooo(s, seq, data, dlen);   /* ahead of the gap: keep it */
        }
        /* Always acknowledge: in order this advances the peer, out of order it
         * is a duplicate ACK telling the peer which byte we still need. */
        (void)tcp_send_seg(net, s, TCP_ACK, s->snd_nxt, 0, 0);
    }

    if ((flags & TCP_FIN) && seq + dlen == s->rcv_nxt) {
        s->fin_received = 1;
        s->rcv_nxt++;
        (void)tcp_send_seg(net, s, TCP_ACK, s->snd_nxt, 0, 0);
        if (s->state == VIBEOS_TCP_ESTABLISHED) {
            s->state = VIBEOS_TCP_CLOSE_WAIT;
        } else if (s->state == VIBEOS_TCP_FIN_WAIT_2 || s->state == VIBEOS_TCP_FIN_WAIT_1) {
            s->state = VIBEOS_TCP_TIME_WAIT;
            /* Nothing else will arrive for this connection: arm reclamation so
             * the slot cannot be held forever. */
            s->close_deadline_ms = net->now_ms + VIBEOS_INET_TIME_WAIT_MS;
        }
    }

    tcp_flush(net, s);
}

/* ---- ARP + IPv4 input ---------------------------------------------------- */

static void arp_input(vibeos_inet_t *net, const uint8_t *a, uint32_t len) {
    uint16_t op;
    uint32_t spa, tpa;

    if (len < 28u || rd16(a + 2) != ETH_TYPE_IP || a[4] != 6u || a[5] != 4u) {
        return;
    }
    op = rd16(a + 6);
    spa = rd32(a + 14);
    tpa = rd32(a + 24);

    arp_insert(net, spa, a + 8);

    if (op == 1u && net->ip != 0u && tpa == net->ip) {   /* request for us */
        uint8_t *o = net->scratch + ETH_HDR;
        uint8_t sender_mac[6];
        bcopy_n(sender_mac, a + 8, 6);
        wr16(o + 0, 1u);
        wr16(o + 2, ETH_TYPE_IP);
        o[4] = 6;
        o[5] = 4;
        wr16(o + 6, 2u);                 /* reply */
        bcopy_n(o + 8, net->mac, 6);
        wr32(o + 14, net->ip);
        bcopy_n(o + 18, sender_mac, 6);
        wr32(o + 24, spa);
        net->arp_replies++;
        (void)eth_send(net, sender_mac, ETH_TYPE_ARP, 28u);
    }
}

static void ip_input(vibeos_inet_t *net, const uint8_t *ip, uint32_t len) {
    uint32_t hdr, total, src, dst;
    const uint8_t *payload;
    uint32_t plen;

    if (len < IP_HDR || (ip[0] >> 4) != 4u) {
        return;
    }
    hdr = (uint32_t)(ip[0] & 0x0Fu) * 4u;
    total = rd16(ip + 2);
    if (hdr < IP_HDR || total < hdr || total > len) {
        return;
    }
    if (vibeos_inet_checksum(ip, hdr) != 0u) {
        net->rx_dropped++;
        return;   /* corrupt header */
    }
    if ((rd16(ip + 6) & 0x1FFFu) != 0u) {
        net->rx_dropped++;
        return;   /* fragment: not supported */
    }
    src = rd32(ip + 12);
    dst = rd32(ip + 16);
    payload = ip + hdr;
    plen = total - hdr;

    /* Accept our address, broadcast, and anything while we have no lease yet
     * (a DHCP reply is addressed to an IP we do not own yet). */
    if (net->ip != 0u && dst != net->ip && dst != 0xFFFFFFFFu &&
        (net->netmask == 0u || dst != (net->ip | ~net->netmask))) {
        return;
    }

    switch (ip[9]) {
        case IP_PROTO_ICMP: icmp_input(net, src, payload, plen); break;
        case IP_PROTO_UDP:  udp_input(net, src, dst, payload, plen); break;
        case IP_PROTO_TCP:  tcp_input(net, src, dst, payload, plen); break;
        default: net->rx_dropped++; break;
    }
}

int vibeos_inet_input(vibeos_inet_t *net, const void *frame, uint32_t len) {
    const uint8_t *f = (const uint8_t *)frame;
    uint16_t type;

    if (!net || !frame || len < ETH_HDR) {
        return -1;
    }
    net->rx_frames++;

    /* Ours, broadcast, or multicast; anything else is not for this host. */
    if (!beq_n(f, net->mac, 6) && !beq_n(f, g_broadcast_mac, 6) && (f[0] & 1u) == 0u) {
        net->rx_dropped++;
        return -1;
    }

    type = rd16(f + 12);
    if (type == ETH_TYPE_ARP) {
        arp_input(net, f + ETH_HDR, len - ETH_HDR);
        return 0;
    }
    if (type == ETH_TYPE_IP) {
        ip_input(net, f + ETH_HDR, len - ETH_HDR);
        return 0;
    }
    net->rx_dropped++;
    return -1;
}

/* ---- timers -------------------------------------------------------------- */

void vibeos_inet_poll(vibeos_inet_t *net, uint64_t now_ms) {
    uint32_t i;

    if (!net) {
        return;
    }
    net->now_ms = now_ms;

    for (i = 0; i < VIBEOS_INET_ARP_ENTRIES; i++) {
        if (net->arp[i].valid && now_ms > net->arp[i].expires_ms) {
            net->arp[i].valid = 0;
        }
    }

    if (net->dhcp_state == 1u || net->dhcp_state == 2u) {
        if (now_ms >= net->dhcp_retry_ms) {
            if (net->dhcp_state == 1u) {
                dhcp_send(net, 1, 0, 0);
            } else {
                dhcp_send(net, 3, net->dhcp_offer_ip, net->dhcp_server);
            }
        }
    } else if (net->dhcp_state >= 3u) {
        /* A lease has a lifetime. Renew at T1, rebind at T2, and give the
         * address up when it expires rather than keep using one we no longer
         * hold. */
        if (now_ms >= net->dhcp_expire_ms) {
            net->ip = 0;
            net->netmask = 0;
            net->gateway = 0;
            net->dhcp_offer_ip = 0;
            net->dhcp_state = 1;
            dhcp_send(net, 1 /* DISCOVER */, 0, 0);
        } else if (now_ms >= net->dhcp_t2_ms && net->dhcp_state != 5u) {
            net->dhcp_state = 5;              /* rebind: broadcast to anyone  */
            dhcp_send(net, 3, net->ip, 0);
        } else if (now_ms >= net->dhcp_t1_ms && net->dhcp_state == 3u) {
            net->dhcp_state = 4;              /* renew with our own server    */
            dhcp_send(net, 3, net->ip, net->dhcp_server);
        } else if ((net->dhcp_state == 4u || net->dhcp_state == 5u) &&
                   now_ms >= net->dhcp_retry_ms) {
            dhcp_send(net, 3, net->ip,
                      (net->dhcp_state == 4u) ? net->dhcp_server : 0u);
        }
    }

    /* Reclaim connections that finished closing, and time out DNS queries. */
    for (i = 0; i < VIBEOS_INET_MAX_SOCKETS; i++) {
        vibeos_inet_socket_t *s = &net->sockets[i];
        if (s->used && s->close_deadline_ms != 0u && now_ms >= s->close_deadline_ms) {
            s->used = 0;
            s->state = VIBEOS_TCP_CLOSED;
            s->close_deadline_ms = 0;
        }
    }

    if (net->dns_pending && now_ms >= net->dns_retry_ms) {
        if (net->dns_retries < 3u) {
            net->dns_retries++;
            dns_send_query(net);
        } else {
            /* Give up rather than leave a caller waiting forever. */
            net->dns_pending = 0;
            net->dns_done = 1;
            net->dns_result = 0;
            net->dns_timeouts++;
        }
    }

    if (net->ping_pending && now_ms - net->ping_sent_ms > 500ull) {
        if (net->ping_seq < 5u) {
            net->ping_seq++;
            (void)icmp_echo_send(net, net->ping_peer, net->ping_id, net->ping_seq);
            net->ping_sent_ms = now_ms;
        } else {
            net->ping_pending = 0;
        }
    }

    for (i = 0; i < VIBEOS_INET_MAX_SOCKETS; i++) {
        vibeos_inet_socket_t *s = &net->sockets[i];
        if (!s->used || s->type != VIBEOS_INET_SOCK_TCP) {
            continue;
        }
        if (s->rto_deadline_ms == 0u || now_ms < s->rto_deadline_ms) {
            continue;
        }
        if (s->retries >= TCP_MAX_RETRIES) {
            s->state = VIBEOS_TCP_CLOSED;
            s->reset = 1;
            s->rto_deadline_ms = 0;
            continue;
        }
        s->retries++;
        net->tcp_retransmits++;
        s->rto_ms = (s->rto_ms * 2u > TCP_RTO_MAX) ? TCP_RTO_MAX : s->rto_ms * 2u;
        s->rto_deadline_ms = now_ms + s->rto_ms;

        if (s->state == VIBEOS_TCP_SYN_SENT) {
            (void)tcp_send_seg(net, s, TCP_SYN, s->snd_una, 0, 0);
        } else if (s->state == VIBEOS_TCP_SYN_RECEIVED) {
            (void)tcp_send_seg(net, s, TCP_SYN | TCP_ACK, s->snd_una, 0, 0);
        } else if (s->tx_len > 0u) {
            uint32_t n = (s->tx_len > TCP_MSS) ? TCP_MSS : s->tx_len;
            (void)tcp_send_seg(net, s, TCP_ACK | TCP_PSH, s->snd_una, s->tx, n);
        } else if (s->fin_sent) {
            (void)tcp_send_seg(net, s, TCP_ACK | TCP_FIN, s->snd_nxt - 1u, 0, 0);
        } else {
            s->rto_deadline_ms = 0;
        }
    }
}
