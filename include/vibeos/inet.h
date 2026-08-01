#ifndef VIBEOS_INET_H
#define VIBEOS_INET_H

/* A real TCP/IP stack: Ethernet, ARP, IPv4, ICMP, UDP, TCP, plus a DHCP client
 * and a DNS resolver.
 *
 * The stack is portable: it never touches hardware. It is handed received
 * frames, it emits frames through a transmit callback, and it is driven forward
 * in time by a periodic poll. That keeps every protocol decision - checksums,
 * the ARP cache, the TCP state machine, retransmission - testable on the host
 * while the same code runs on metal behind a virtio-net driver.
 *
 * Addresses are host byte order in this API (a uint32_t IPv4 address reads
 * a.b.c.d from most significant byte down); wire byte order is applied inside.
 */

#include <stdint.h>
#include <stddef.h>

#define VIBEOS_INET_MAX_SOCKETS 16u
#define VIBEOS_INET_ARP_ENTRIES 8u
#define VIBEOS_INET_MTU 1514u        /* Ethernet frame incl. header, no FCS */
#define VIBEOS_INET_RXBUF 4096u      /* per-socket receive buffer            */
#define VIBEOS_INET_TXBUF 4096u      /* per-socket unacknowledged send data  */
#define VIBEOS_INET_BACKLOG 4u

/* Socket types. */
enum {
    VIBEOS_INET_SOCK_NONE = 0,
    VIBEOS_INET_SOCK_UDP = 1,
    VIBEOS_INET_SOCK_TCP = 2
};

/* TCP connection states (RFC 793). */
enum {
    VIBEOS_TCP_CLOSED = 0,
    VIBEOS_TCP_LISTEN,
    VIBEOS_TCP_SYN_SENT,
    VIBEOS_TCP_SYN_RECEIVED,
    VIBEOS_TCP_ESTABLISHED,
    VIBEOS_TCP_FIN_WAIT_1,
    VIBEOS_TCP_FIN_WAIT_2,
    VIBEOS_TCP_CLOSE_WAIT,
    VIBEOS_TCP_CLOSING,
    VIBEOS_TCP_LAST_ACK,
    VIBEOS_TCP_TIME_WAIT
};

/* Errors returned by the socket calls (negative). */
#define VIBEOS_INET_EAGAIN 1
#define VIBEOS_INET_EINVAL 2
#define VIBEOS_INET_ECONNRESET 3
#define VIBEOS_INET_ENOTCONN 4
#define VIBEOS_INET_ETIMEDOUT 5
#define VIBEOS_INET_ENOBUFS 6

typedef struct vibeos_arp_entry {
    uint32_t ip;
    uint8_t mac[6];
    uint8_t valid;
    uint64_t expires_ms;
} vibeos_arp_entry_t;

typedef struct vibeos_inet_socket {
    uint8_t type;            /* VIBEOS_INET_SOCK_*                          */
    uint8_t state;           /* TCP state; unused for UDP                   */
    uint8_t used;
    uint8_t reset;           /* peer sent RST                               */
    uint16_t local_port;
    uint16_t remote_port;
    uint32_t remote_ip;

    /* TCP sequence state. */
    uint32_t snd_una;        /* oldest unacknowledged byte                  */
    uint32_t snd_nxt;        /* next byte to send                           */
    uint32_t rcv_nxt;        /* next byte expected                          */
    uint16_t snd_wnd;        /* peer's advertised window                    */
    uint8_t fin_sent;
    uint8_t fin_received;

    /* Retransmission of the single outstanding send window. */
    uint64_t rto_deadline_ms;
    uint32_t rto_ms;
    uint32_t retries;

    uint8_t rx[VIBEOS_INET_RXBUF];
    uint32_t rx_len;
    uint8_t tx[VIBEOS_INET_TXBUF];
    uint32_t tx_len;         /* bytes queued but not yet acknowledged        */

    /* UDP keeps the sender of the last datagram so recvfrom can report it. */
    uint32_t last_src_ip;
    uint16_t last_src_port;

    /* Listening sockets hand completed connections to accept(). */
    int backlog[VIBEOS_INET_BACKLOG];
    uint32_t backlog_len;
    int parent;              /* index of the listening socket, or -1         */
} vibeos_inet_socket_t;

/* Frame transmit hook, provided by the driver. Returns 0 on success. */
typedef int (*vibeos_inet_tx_fn)(void *ctx, const void *frame, uint32_t len);

typedef struct vibeos_inet {
    uint8_t mac[6];
    uint32_t ip;
    uint32_t netmask;
    uint32_t gateway;
    uint32_t dns;

    vibeos_inet_tx_fn tx;
    void *tx_ctx;

    uint64_t now_ms;
    uint16_t next_ephemeral;
    uint16_t ip_id;

    vibeos_arp_entry_t arp[VIBEOS_INET_ARP_ENTRIES];
    vibeos_inet_socket_t sockets[VIBEOS_INET_MAX_SOCKETS];

    /* One in-flight ICMP echo, so `ping` can report a round trip. */
    uint16_t ping_id;
    uint16_t ping_seq;
    uint32_t ping_peer;
    uint8_t ping_pending;
    uint8_t ping_replied;
    uint64_t ping_sent_ms;
    uint64_t ping_rtt_ms;

    /* DHCP client. */
    uint8_t dhcp_state;      /* 0 idle, 1 discovering, 2 requesting, 3 bound */
    uint32_t dhcp_xid;
    uint32_t dhcp_offer_ip;
    uint32_t dhcp_server;
    uint64_t dhcp_retry_ms;

    /* One in-flight DNS query. */
    uint16_t dns_id;
    uint8_t dns_pending;
    uint8_t dns_done;
    uint32_t dns_result;
    char dns_name[64];

    /* Counters, so the system can report what the link actually did. */
    uint64_t rx_frames;
    uint64_t tx_frames;
    uint64_t rx_dropped;
    uint64_t arp_replies;
    uint64_t tcp_retransmits;

    uint8_t scratch[VIBEOS_INET_MTU];
} vibeos_inet_t;

/* ---- lifecycle ---------------------------------------------------------- */

int vibeos_inet_init(vibeos_inet_t *net, const uint8_t mac[6],
                     vibeos_inet_tx_fn tx, void *tx_ctx);
void vibeos_inet_set_addr(vibeos_inet_t *net, uint32_t ip, uint32_t netmask,
                          uint32_t gateway, uint32_t dns);

/* Feed one received Ethernet frame. Returns 0 if it was consumed. */
int vibeos_inet_input(vibeos_inet_t *net, const void *frame, uint32_t len);

/* Advance time: ARP expiry, TCP retransmission, DHCP retries. */
void vibeos_inet_poll(vibeos_inet_t *net, uint64_t now_ms);

/* ---- sockets ------------------------------------------------------------ */

int vibeos_inet_socket(vibeos_inet_t *net, int type);
int vibeos_inet_bind(vibeos_inet_t *net, int sock, uint16_t port);
int vibeos_inet_connect(vibeos_inet_t *net, int sock, uint32_t ip, uint16_t port);
int vibeos_inet_listen(vibeos_inet_t *net, int sock);
int vibeos_inet_accept(vibeos_inet_t *net, int sock);
long vibeos_inet_send(vibeos_inet_t *net, int sock, const void *buf, uint32_t len);
long vibeos_inet_recv(vibeos_inet_t *net, int sock, void *buf, uint32_t len);
long vibeos_inet_sendto(vibeos_inet_t *net, int sock, const void *buf, uint32_t len,
                        uint32_t ip, uint16_t port);
long vibeos_inet_recvfrom(vibeos_inet_t *net, int sock, void *buf, uint32_t len,
                          uint32_t *out_ip, uint16_t *out_port);
int vibeos_inet_close(vibeos_inet_t *net, int sock);
int vibeos_inet_socket_state(const vibeos_inet_t *net, int sock);

/* ---- utilities ---------------------------------------------------------- */

int vibeos_inet_ping(vibeos_inet_t *net, uint32_t ip);
int vibeos_inet_ping_result(const vibeos_inet_t *net, uint64_t *out_rtt_ms);
int vibeos_inet_dhcp_start(vibeos_inet_t *net);
int vibeos_inet_dhcp_bound(const vibeos_inet_t *net);
int vibeos_inet_resolve(vibeos_inet_t *net, const char *name);
int vibeos_inet_resolve_result(const vibeos_inet_t *net, uint32_t *out_ip);

uint16_t vibeos_inet_checksum(const void *data, uint32_t len);
uint32_t vibeos_inet_parse_ip(const char *s);

#endif
