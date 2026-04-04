#include "vibeos/net.h"

static vibeos_socket_entry_t *find_socket(vibeos_net_runtime_t *rt, uint32_t socket_id) {
    uint32_t i;
    if (!rt || socket_id == 0) {
        return 0;
    }
    for (i = 0; i < VIBEOS_MAX_SOCKETS; i++) {
        if (rt->sockets[i].in_use && rt->sockets[i].id == socket_id) {
            return &rt->sockets[i];
        }
    }
    return 0;
}

int vibeos_net_runtime_init(vibeos_net_runtime_t *rt) {
    uint32_t i;
    if (!rt) {
        return -1;
    }
    rt->next_socket_id = 1;
    rt->total_tx_packets = 0;
    rt->total_rx_packets = 0;
    for (i = 0; i < VIBEOS_MAX_SOCKETS; i++) {
        rt->sockets[i].id = 0;
        rt->sockets[i].in_use = 0;
        rt->sockets[i].port = 0;
        rt->sockets[i].tx_packets = 0;
        rt->sockets[i].rx_packets = 0;
    }
    return 0;
}

int vibeos_socket_create(vibeos_net_runtime_t *rt, uint32_t *out_socket_id) {
    uint32_t i;
    if (!rt || !out_socket_id) {
        return -1;
    }
    for (i = 0; i < VIBEOS_MAX_SOCKETS; i++) {
        if (!rt->sockets[i].in_use) {
            rt->sockets[i].in_use = 1;
            rt->sockets[i].id = rt->next_socket_id++;
            rt->sockets[i].port = 0;
            rt->sockets[i].tx_packets = 0;
            rt->sockets[i].rx_packets = 0;
            *out_socket_id = rt->sockets[i].id;
            return 0;
        }
    }
    return -1;
}

int vibeos_socket_bind(vibeos_net_runtime_t *rt, uint32_t socket_id, uint16_t port) {
    uint32_t i;
    vibeos_socket_entry_t *entry = find_socket(rt, socket_id);
    if (!entry || port == 0) {
        return -1;
    }
    for (i = 0; i < VIBEOS_MAX_SOCKETS; i++) {
        if (rt->sockets[i].in_use && rt->sockets[i].id != socket_id && rt->sockets[i].port == port) {
            return -1;
        }
    }
    entry->port = port;
    return 0;
}

int vibeos_socket_bind_secure(vibeos_net_runtime_t *rt, uint32_t socket_id, uint16_t port, const vibeos_policy_state_t *policy, const vibeos_security_token_t *token) {
    if (!policy || !token) {
        return -1;
    }
    if (vibeos_policy_can_net_bind(policy, token->capability_mask) != VIBEOS_POLICY_ALLOW) {
        return -1;
    }
    return vibeos_socket_bind(rt, socket_id, port);
}

int vibeos_socket_send(vibeos_net_runtime_t *rt, uint32_t socket_id, const void *buf, size_t len) {
    vibeos_socket_entry_t *entry = find_socket(rt, socket_id);
    if (!entry || !buf || len == 0 || entry->port == 0) {
        return -1;
    }
    entry->tx_packets++;
    rt->total_tx_packets++;
    return 0;
}

int vibeos_socket_receive(vibeos_net_runtime_t *rt, uint32_t socket_id, void *buf, size_t len, size_t *out_received) {
    vibeos_socket_entry_t *entry = find_socket(rt, socket_id);
    uint8_t *bytes = (uint8_t *)buf;
    size_t i;
    if (!entry || !buf || len == 0 || !out_received || entry->port == 0) {
        return -1;
    }
    for (i = 0; i < len; i++) {
        bytes[i] = (uint8_t)('a' + (i % 26u));
    }
    *out_received = len;
    entry->rx_packets++;
    rt->total_rx_packets++;
    return 0;
}

int vibeos_socket_close(vibeos_net_runtime_t *rt, uint32_t socket_id) {
    vibeos_socket_entry_t *entry = find_socket(rt, socket_id);
    if (!entry) {
        return -1;
    }
    entry->in_use = 0;
    entry->id = 0;
    entry->port = 0;
    entry->tx_packets = 0;
    entry->rx_packets = 0;
    return 0;
}

int vibeos_net_stats(const vibeos_net_runtime_t *rt, uint64_t *out_total_tx_packets, uint64_t *out_total_rx_packets, uint32_t *out_open_sockets) {
    uint32_t i;
    uint32_t open = 0;
    if (!rt || !out_total_tx_packets || !out_total_rx_packets || !out_open_sockets) {
        return -1;
    }
    for (i = 0; i < VIBEOS_MAX_SOCKETS; i++) {
        if (rt->sockets[i].in_use) {
            open++;
        }
    }
    *out_total_tx_packets = rt->total_tx_packets;
    *out_total_rx_packets = rt->total_rx_packets;
    *out_open_sockets = open;
    return 0;
}
