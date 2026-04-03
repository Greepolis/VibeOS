#include "vibeos/waitset.h"

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
