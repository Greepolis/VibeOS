#ifndef VIBEOS_NET_H
#define VIBEOS_NET_H

#include <stddef.h>
#include <stdint.h>

#include "vibeos/policy.h"
#include "vibeos/security_model.h"

#define VIBEOS_MAX_SOCKETS 64u

typedef struct vibeos_socket_entry {
    uint32_t id;
    uint32_t in_use;
    uint16_t port;
} vibeos_socket_entry_t;

typedef struct vibeos_net_runtime {
    vibeos_socket_entry_t sockets[VIBEOS_MAX_SOCKETS];
    uint32_t next_socket_id;
} vibeos_net_runtime_t;

int vibeos_net_runtime_init(vibeos_net_runtime_t *rt);
int vibeos_socket_create(vibeos_net_runtime_t *rt, uint32_t *out_socket_id);
int vibeos_socket_bind(vibeos_net_runtime_t *rt, uint32_t socket_id, uint16_t port);
int vibeos_socket_bind_secure(vibeos_net_runtime_t *rt, uint32_t socket_id, uint16_t port, const vibeos_policy_state_t *policy, const vibeos_security_token_t *token);
int vibeos_socket_send(vibeos_net_runtime_t *rt, uint32_t socket_id, const void *buf, size_t len);
int vibeos_socket_close(vibeos_net_runtime_t *rt, uint32_t socket_id);

#endif
