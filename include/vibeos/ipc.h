#ifndef VIBEOS_IPC_H
#define VIBEOS_IPC_H

#include <stddef.h>
#include <stdint.h>

#define VIBEOS_CHANNEL_CAPACITY 64u

typedef struct vibeos_event {
    uint32_t signaled;
} vibeos_event_t;

typedef struct vibeos_message {
    uint32_t code;
    uint64_t payload;
} vibeos_message_t;

typedef struct vibeos_channel {
    vibeos_message_t slots[VIBEOS_CHANNEL_CAPACITY];
    size_t head;
    size_t tail;
    size_t count;
} vibeos_channel_t;

void vibeos_event_init(vibeos_event_t *event);
void vibeos_event_signal(vibeos_event_t *event);
void vibeos_event_clear(vibeos_event_t *event);
int vibeos_event_is_signaled(const vibeos_event_t *event);

void vibeos_channel_init(vibeos_channel_t *channel);
int vibeos_channel_send(vibeos_channel_t *channel, vibeos_message_t msg);
int vibeos_channel_recv(vibeos_channel_t *channel, vibeos_message_t *out_msg);

#endif
