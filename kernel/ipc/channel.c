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
