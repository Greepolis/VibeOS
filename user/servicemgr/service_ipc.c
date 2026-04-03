#include "vibeos/service_ipc.h"

int vibeos_service_msg_build(vibeos_service_msg_t *msg, uint32_t src_service, uint32_t dst_service, uint32_t msg_type, uint64_t payload) {
    if (!msg || src_service == 0 || dst_service == 0 || msg_type == 0) {
        return -1;
    }
    msg->version = VIBEOS_SERVICE_IPC_VERSION;
    msg->src_service = src_service;
    msg->dst_service = dst_service;
    msg->msg_type = msg_type;
    msg->payload = payload;
    return 0;
}

int vibeos_service_msg_validate(const vibeos_service_msg_t *msg) {
    if (!msg) {
        return -1;
    }
    if (msg->version != VIBEOS_SERVICE_IPC_VERSION) {
        return -1;
    }
    if (msg->src_service == 0 || msg->dst_service == 0 || msg->msg_type == 0) {
        return -1;
    }
    return 0;
}
