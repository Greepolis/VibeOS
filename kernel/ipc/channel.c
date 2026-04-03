#include "vibeos/ipc.h"

void vibeos_channel_init(vibeos_channel_t *channel) {
    if (!channel) {
        return;
    }
    channel->head = 0;
    channel->tail = 0;
    channel->count = 0;
}

int vibeos_channel_send(vibeos_channel_t *channel, vibeos_message_t msg) {
    if (!channel || channel->count >= VIBEOS_CHANNEL_CAPACITY) {
        return -1;
    }
    channel->slots[channel->tail] = msg;
    channel->tail = (channel->tail + 1u) % VIBEOS_CHANNEL_CAPACITY;
    channel->count++;
    return 0;
}

int vibeos_channel_recv(vibeos_channel_t *channel, vibeos_message_t *out_msg) {
    if (!channel || !out_msg || channel->count == 0) {
        return -1;
    }
    *out_msg = channel->slots[channel->head];
    channel->head = (channel->head + 1u) % VIBEOS_CHANNEL_CAPACITY;
    channel->count--;
    return 0;
}

int vibeos_channel_send_with_handle(vibeos_channel_t *channel, uint32_t code, uint64_t payload, uint32_t handle, uint32_t rights) {
    vibeos_message_t msg;
    msg.code = code;
    msg.payload = payload;
    msg.transferred_handle = handle;
    msg.transferred_rights = rights;
    return vibeos_channel_send(channel, msg);
}

int vibeos_channel_recv_with_handle(vibeos_channel_t *channel, vibeos_message_t *out_msg, uint32_t *out_handle, uint32_t *out_rights) {
    if (!out_handle || !out_rights) {
        return -1;
    }
    if (vibeos_channel_recv(channel, out_msg) != 0) {
        return -1;
    }
    *out_handle = out_msg->transferred_handle;
    *out_rights = out_msg->transferred_rights;
    return 0;
}
