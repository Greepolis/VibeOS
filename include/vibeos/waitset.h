#ifndef VIBEOS_WAITSET_H
#define VIBEOS_WAITSET_H

#include <stddef.h>
#include <stdint.h>

#include "vibeos/scheduler.h"
#include "vibeos/timer.h"

#define VIBEOS_WAITSET_MAX_EVENTS 32u

typedef struct vibeos_waitset {
    void *events[VIBEOS_WAITSET_MAX_EVENTS];
    size_t count;
    uint32_t owner_pid;
    uint32_t ownership_enforced;
    uint32_t active;
} vibeos_waitset_t;

int vibeos_waitset_init(vibeos_waitset_t *waitset);
int vibeos_waitset_init_owned(vibeos_waitset_t *waitset, uint32_t owner_pid);
int vibeos_waitset_add(vibeos_waitset_t *waitset, void *event_ptr);
int vibeos_waitset_add_owned(vibeos_waitset_t *waitset, void *event_ptr, uint32_t caller_pid);
int vibeos_waitset_remove(vibeos_waitset_t *waitset, size_t index);
int vibeos_waitset_reset(vibeos_waitset_t *waitset);
int vibeos_waitset_destroy(vibeos_waitset_t *waitset);
int vibeos_waitset_count(const vibeos_waitset_t *waitset, size_t *out_count);
int vibeos_waitset_owner(const vibeos_waitset_t *waitset, uint32_t *out_owner_pid, uint32_t *out_enforced);
int vibeos_waitset_wait(vibeos_waitset_t *waitset, uint64_t timeout_ticks, size_t *out_index);
int vibeos_waitset_wait_ex(vibeos_waitset_t *waitset, uint64_t timeout_ticks, size_t *out_index, vibeos_scheduler_t *sched, uint32_t cpu_id);
int vibeos_waitset_wait_timed(vibeos_waitset_t *waitset, vibeos_timer_t *timer, uint64_t timeout_ticks, size_t *out_index, vibeos_scheduler_t *sched, uint32_t cpu_id);

#endif
