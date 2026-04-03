#include "vibeos/ipc.h"

void vibeos_event_init(vibeos_event_t *event) {
    if (event) {
        event->signaled = 0;
    }
}

void vibeos_event_signal(vibeos_event_t *event) {
    if (event) {
        event->signaled = 1;
    }
}

void vibeos_event_clear(vibeos_event_t *event) {
    if (event) {
        event->signaled = 0;
    }
}

int vibeos_event_is_signaled(const vibeos_event_t *event) {
    return event && event->signaled ? 1 : 0;
}
