#include "vibeos/net_policy.h"

static uint32_t prefix_mask(uint8_t length) {
    if (length == 0u) {
        return 0u;
    }
    return 0xFFFFFFFFu << (32u - length);
}

static int prefix_matches(uint32_t address, uint32_t prefix, uint8_t length) {
    uint32_t mask = prefix_mask(length);
    return (address & mask) == (prefix & mask);
}

int vibeos_net_policy_init(vibeos_net_policy_t *policy) {
    uint32_t i;
    if (!policy) {
        return -1;
    }
    for (i = 0; i < VIBEOS_NET_MAX_ROUTES; i++) {
        policy->routes[i].valid = 0;
    }
    for (i = 0; i < VIBEOS_NET_MAX_RULES; i++) {
        policy->rules[i].valid = 0;
    }
    for (i = 0; i < VIBEOS_NET_MAX_FLOWS; i++) {
        policy->flows[i].valid = 0;
    }
    policy->route_count = 0;
    policy->rule_count = 0;
    policy->flow_count = 0;
    policy->denied_packets = 0;
    return 0;
}

int vibeos_net_route_add(vibeos_net_policy_t *policy, uint32_t prefix, uint8_t prefix_len,
                         uint32_t gateway, uint8_t iface, uint32_t metric) {
    uint32_t i;
    if (!policy || prefix_len > 32u || iface == 0u) {
        return -1;
    }
    for (i = 0; i < VIBEOS_NET_MAX_ROUTES; i++) {
        if (!policy->routes[i].valid) {
            policy->routes[i].valid = 1;
            policy->routes[i].prefix = prefix & prefix_mask(prefix_len);
            policy->routes[i].prefix_len = prefix_len;
            policy->routes[i].gateway = gateway;
            policy->routes[i].iface = iface;
            policy->routes[i].metric = metric;
            policy->route_count++;
            return 0;
        }
    }
    return -1;
}

int vibeos_net_route_remove(vibeos_net_policy_t *policy, uint32_t index) {
    if (!policy || index >= VIBEOS_NET_MAX_ROUTES || !policy->routes[index].valid) {
        return -1;
    }
    policy->routes[index].valid = 0;
    if (policy->route_count != 0u) {
        policy->route_count--;
    }
    return 0;
}

int vibeos_net_route_lookup(const vibeos_net_policy_t *policy, uint32_t destination,
                            vibeos_net_route_t *out_route) {
    uint32_t i;
    int best = -1;
    if (!policy || !out_route) {
        return -1;
    }
    for (i = 0; i < VIBEOS_NET_MAX_ROUTES; i++) {
        const vibeos_net_route_t *route = &policy->routes[i];
        if (route->valid && prefix_matches(destination, route->prefix, route->prefix_len) &&
            (best < 0 || route->prefix_len > policy->routes[best].prefix_len ||
             (route->prefix_len == policy->routes[best].prefix_len && route->metric < policy->routes[best].metric))) {
            best = (int)i;
        }
    }
    if (best < 0) {
        return -1;
    }
    *out_route = policy->routes[best];
    return 0;
}

int vibeos_net_rule_add(vibeos_net_policy_t *policy, uint8_t action, uint8_t direction,
                        uint8_t protocol, uint16_t local_port, uint16_t remote_port,
                        uint32_t remote_prefix, uint8_t remote_prefix_len) {
    uint32_t i;
    if (!policy || (action != 0u && action != 1u) ||
        (direction != VIBEOS_NET_DIR_INGRESS && direction != VIBEOS_NET_DIR_EGRESS) ||
        (protocol != VIBEOS_NET_PROTO_ANY && protocol != VIBEOS_NET_PROTO_ICMP &&
         protocol != VIBEOS_NET_PROTO_TCP && protocol != VIBEOS_NET_PROTO_UDP) ||
        remote_prefix_len > 32u) {
        return -1;
    }
    for (i = 0; i < VIBEOS_NET_MAX_RULES; i++) {
        if (!policy->rules[i].valid) {
            vibeos_net_rule_t *rule = &policy->rules[i];
            rule->valid = 1;
            rule->action = action;
            rule->direction = direction;
            rule->protocol = protocol;
            rule->local_port = local_port;
            rule->remote_port = remote_port;
            rule->remote_prefix = remote_prefix & prefix_mask(remote_prefix_len);
            rule->remote_prefix_len = remote_prefix_len;
            policy->rule_count++;
            return 0;
        }
    }
    return -1;
}

int vibeos_net_rule_remove(vibeos_net_policy_t *policy, uint32_t index) {
    if (!policy || index >= VIBEOS_NET_MAX_RULES || !policy->rules[index].valid) {
        return -1;
    }
    policy->rules[index].valid = 0;
    if (policy->rule_count != 0u) {
        policy->rule_count--;
    }
    return 0;
}

static int flow_matches(const vibeos_net_flow_t *flow, uint8_t protocol, uint32_t local_ip,
                        uint16_t local_port, uint32_t remote_ip, uint16_t remote_port) {
    return flow->valid && flow->protocol == protocol &&
        ((flow->local_ip == local_ip && flow->local_port == local_port &&
          flow->remote_ip == remote_ip && flow->remote_port == remote_port) ||
         (flow->local_ip == remote_ip && flow->local_port == remote_port &&
          flow->remote_ip == local_ip && flow->remote_port == local_port));
}

int vibeos_net_policy_check(vibeos_net_policy_t *policy, uint8_t direction, uint8_t protocol,
                            uint32_t local_ip, uint16_t local_port, uint32_t remote_ip,
                            uint16_t remote_port, uint64_t now_ms) {
    uint32_t i;
    int matched = -1;
    if (!policy || (direction != VIBEOS_NET_DIR_INGRESS && direction != VIBEOS_NET_DIR_EGRESS)) {
        return -1;
    }
    if (direction == VIBEOS_NET_DIR_INGRESS) {
        for (i = 0; i < VIBEOS_NET_MAX_FLOWS; i++) {
            if (flow_matches(&policy->flows[i], protocol, local_ip, local_port, remote_ip, remote_port)) {
                policy->flows[i].last_seen_ms = now_ms;
                return 1;
            }
        }
    }
    for (i = 0; i < VIBEOS_NET_MAX_RULES; i++) {
        const vibeos_net_rule_t *rule = &policy->rules[i];
        if (rule->valid && rule->direction == direction &&
            (rule->protocol == VIBEOS_NET_PROTO_ANY || rule->protocol == protocol) &&
            (rule->local_port == 0u || rule->local_port == local_port) &&
            (rule->remote_port == 0u || rule->remote_port == remote_port) &&
            prefix_matches(remote_ip, rule->remote_prefix, rule->remote_prefix_len)) {
            matched = rule->action ? 1 : 0;
            break;
        }
    }
    if (matched != 1) {
        policy->denied_packets++;
        return 0;
    }
    if (direction == VIBEOS_NET_DIR_EGRESS) {
        for (i = 0; i < VIBEOS_NET_MAX_FLOWS; i++) {
            if (!policy->flows[i].valid) {
                policy->flows[i].valid = 1;
                policy->flows[i].protocol = protocol;
                policy->flows[i].local_ip = local_ip;
                policy->flows[i].local_port = local_port;
                policy->flows[i].remote_ip = remote_ip;
                policy->flows[i].remote_port = remote_port;
                policy->flows[i].last_seen_ms = now_ms;
                policy->flow_count++;
                break;
            }
        }
    }
    return 1;
}

uint32_t vibeos_net_policy_expire_flows(vibeos_net_policy_t *policy, uint64_t now_ms,
                                        uint64_t idle_ms) {
    uint32_t i;
    uint32_t expired = 0;
    if (!policy) {
        return 0;
    }
    for (i = 0; i < VIBEOS_NET_MAX_FLOWS; i++) {
        vibeos_net_flow_t *flow = &policy->flows[i];
        if (flow->valid && now_ms >= flow->last_seen_ms &&
            now_ms - flow->last_seen_ms >= idle_ms) {
            flow->valid = 0;
            if (policy->flow_count != 0u) {
                policy->flow_count--;
            }
            expired++;
        }
    }
    return expired;
}
