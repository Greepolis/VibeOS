#include "vibeos/waitset.h"
#include "vibeos/ipc.h"

int vibeos_waitset_init(vibeos_waitset_t *waitset) {
    if (!waitset) {
        return -1;
    }
    waitset->count = 0;
    return 0;
}

int vibeos_waitset_add(vibeos_waitset_t *waitset, void *event_ptr) {
    if (!waitset || !event_ptr || waitset->count >= VIBEOS_WAITSET_MAX_EVENTS) {
        return -1;
    }
    waitset->events[waitset->count++] = event_ptr;
    return 0;
}

int vibeos_waitset_count(const vibeos_waitset_t *waitset, size_t *out_count) {
    if (!waitset || !out_count) {
        return -1;
    }
    *out_count = waitset->count;
    return 0;
}

int vibeos_waitset_wait(vibeos_waitset_t *waitset, uint64_t timeout_ticks, size_t *out_index) {
    uint64_t tick;
    size_t i;
    if (!waitset || !out_index) {
        return -1;
    }
    for (tick = 0; tick <= timeout_ticks; tick++) {
        for (i = 0; i < waitset->count; i++) {
            vibeos_event_t *ev = (vibeos_event_t *)waitset->events[i];
            if (ev && vibeos_event_is_signaled(ev)) {
                *out_index = i;
                return 0;
            }
        }
    }
    return -1;
}
