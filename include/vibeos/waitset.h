#ifndef VIBEOS_WAITSET_H
#define VIBEOS_WAITSET_H

#include <stddef.h>
#include <stdint.h>

#include "vibeos/scheduler.h"

#define VIBEOS_WAITSET_MAX_EVENTS 32u

typedef struct vibeos_waitset {
    void *events[VIBEOS_WAITSET_MAX_EVENTS];
    size_t count;
} vibeos_waitset_t;

int vibeos_waitset_init(vibeos_waitset_t *waitset);
int vibeos_waitset_add(vibeos_waitset_t *waitset, void *event_ptr);
int vibeos_waitset_count(const vibeos_waitset_t *waitset, size_t *out_count);
int vibeos_waitset_wait(vibeos_waitset_t *waitset, uint64_t timeout_ticks, size_t *out_index);
int vibeos_waitset_wait_ex(vibeos_waitset_t *waitset, uint64_t timeout_ticks, size_t *out_index, vibeos_scheduler_t *sched, uint32_t cpu_id);

#endif
