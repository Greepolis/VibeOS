#ifndef VIBEOS_SERVICE_IPC_H
#define VIBEOS_SERVICE_IPC_H

#include <stdint.h>

#define VIBEOS_SERVICE_IPC_VERSION 1u

typedef enum vibeos_service_id {
    VIBEOS_SERVICE_INIT = 1,
    VIBEOS_SERVICE_SERVICEMGR = 2,
    VIBEOS_SERVICE_DEVMGR = 3,
    VIBEOS_SERVICE_FS = 4,
    VIBEOS_SERVICE_NET = 5
} vibeos_service_id_t;

typedef enum vibeos_service_msg_type {
    VIBEOS_SERVICE_MSG_HELLO = 1,
    VIBEOS_SERVICE_MSG_START = 2,
    VIBEOS_SERVICE_MSG_STATUS = 3,
    VIBEOS_SERVICE_MSG_ACK = 4,
    VIBEOS_SERVICE_MSG_ERROR = 5
} vibeos_service_msg_type_t;

typedef struct vibeos_service_msg {
    uint32_t version;
    uint32_t src_service;
    uint32_t dst_service;
    uint32_t msg_type;
    uint32_t correlation_id;
    uint32_t status_code;
    uint64_t payload;
} vibeos_service_msg_t;

int vibeos_service_msg_build(vibeos_service_msg_t *msg, uint32_t src_service, uint32_t dst_service, uint32_t msg_type, uint64_t payload);
int vibeos_service_msg_validate(const vibeos_service_msg_t *msg);
int vibeos_service_msg_set_reply(vibeos_service_msg_t *msg, uint32_t correlation_id, uint32_t status_code);

#endif
