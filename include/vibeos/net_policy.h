#ifndef VIBEOS_NET_POLICY_H
#define VIBEOS_NET_POLICY_H

#include <stdint.h>

#define VIBEOS_NET_MAX_ROUTES 16u
#define VIBEOS_NET_MAX_RULES 32u
#define VIBEOS_NET_MAX_FLOWS 32u

enum {
    VIBEOS_NET_DIR_INGRESS = 1,
    VIBEOS_NET_DIR_EGRESS = 2
};

enum {
    VIBEOS_NET_PROTO_ANY = 0,
    VIBEOS_NET_PROTO_ICMP = 1,
    VIBEOS_NET_PROTO_TCP = 6,
    VIBEOS_NET_PROTO_UDP = 17
};

typedef struct vibeos_net_route {
    uint32_t prefix;
    uint32_t gateway;
    uint32_t metric;
    uint8_t prefix_len;
    uint8_t iface;
    uint8_t valid;
} vibeos_net_route_t;

typedef struct vibeos_net_rule {
    uint8_t valid;
    uint8_t action;
    uint8_t direction;
    uint8_t protocol;
    uint16_t local_port;
    uint16_t remote_port;
    uint32_t remote_prefix;
    uint8_t remote_prefix_len;
} vibeos_net_rule_t;

typedef struct vibeos_net_flow {
    uint8_t valid;
    uint8_t protocol;
    uint16_t local_port;
    uint16_t remote_port;
    uint32_t local_ip;
    uint32_t remote_ip;
    uint64_t last_seen_ms;
} vibeos_net_flow_t;

typedef struct vibeos_net_policy {
    vibeos_net_route_t routes[VIBEOS_NET_MAX_ROUTES];
    vibeos_net_rule_t rules[VIBEOS_NET_MAX_RULES];
    vibeos_net_flow_t flows[VIBEOS_NET_MAX_FLOWS];
    uint32_t route_count;
    uint32_t rule_count;
    uint32_t flow_count;
    uint64_t denied_packets;
} vibeos_net_policy_t;

int vibeos_net_policy_init(vibeos_net_policy_t *policy);
int vibeos_net_route_add(vibeos_net_policy_t *policy, uint32_t prefix, uint8_t prefix_len,
                         uint32_t gateway, uint8_t iface, uint32_t metric);
int vibeos_net_route_remove(vibeos_net_policy_t *policy, uint32_t index);
int vibeos_net_route_lookup(const vibeos_net_policy_t *policy, uint32_t destination,
                            vibeos_net_route_t *out_route);
int vibeos_net_rule_add(vibeos_net_policy_t *policy, uint8_t action, uint8_t direction,
                        uint8_t protocol, uint16_t local_port, uint16_t remote_port,
                        uint32_t remote_prefix, uint8_t remote_prefix_len);
int vibeos_net_rule_remove(vibeos_net_policy_t *policy, uint32_t index);
int vibeos_net_policy_check(vibeos_net_policy_t *policy, uint8_t direction, uint8_t protocol,
                            uint32_t local_ip, uint16_t local_port, uint32_t remote_ip,
                            uint16_t remote_port, uint64_t now_ms);
uint32_t vibeos_net_policy_expire_flows(vibeos_net_policy_t *policy, uint64_t now_ms,
                                        uint64_t idle_ms);

#endif
