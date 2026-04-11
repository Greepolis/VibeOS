#include "vibeos/service_ipc.h"

static int service_id_valid(uint32_t service_id) {
    return service_id >= VIBEOS_SERVICE_INIT && service_id <= VIBEOS_SERVICE_NET;
}

static int service_msg_type_valid(uint32_t msg_type) {
    return msg_type >= VIBEOS_SERVICE_MSG_HELLO && msg_type <= VIBEOS_SERVICE_MSG_ERROR;
}

int vibeos_service_msg_build(vibeos_service_msg_t *msg, uint32_t src_service, uint32_t dst_service, uint32_t msg_type, uint64_t payload) {
    if (!msg || src_service == 0 || dst_service == 0 || msg_type == 0) {
        return -1;
    }
    msg->version = VIBEOS_SERVICE_IPC_VERSION;
    msg->src_service = src_service;
    msg->dst_service = dst_service;
    msg->msg_type = msg_type;
    msg->correlation_id = 0;
    msg->status_code = 0;
    msg->payload = payload;
    return vibeos_service_msg_validate(msg);
}

int vibeos_service_msg_validate(const vibeos_service_msg_t *msg) {
    if (!msg) {
        return -1;
    }
    if (msg->version != VIBEOS_SERVICE_IPC_VERSION) {
        return -1;
    }
    if (!service_id_valid(msg->src_service) || !service_id_valid(msg->dst_service) || !service_msg_type_valid(msg->msg_type)) {
        return -1;
    }
    if ((msg->msg_type == VIBEOS_SERVICE_MSG_ACK || msg->msg_type == VIBEOS_SERVICE_MSG_ERROR) && msg->correlation_id == 0) {
        return -1;
    }
    return 0;
}

int vibeos_service_msg_set_reply(vibeos_service_msg_t *msg, uint32_t correlation_id, uint32_t status_code) {
    if (!msg || correlation_id == 0) {
        return -1;
    }
    msg->correlation_id = correlation_id;
    msg->status_code = status_code;
    return vibeos_service_msg_validate(msg);
}
