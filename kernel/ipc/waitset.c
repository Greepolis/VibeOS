#include "vibeos/waitset.h"
#include "vibeos/ipc.h"

int vibeos_waitset_init(vibeos_waitset_t *waitset) {
    if (!waitset) {
        return -1;
    }
    waitset->count = 0;
    waitset->owner_pid = 0;
    waitset->ownership_enforced = 0;
    return 0;
}

int vibeos_waitset_init_owned(vibeos_waitset_t *waitset, uint32_t owner_pid) {
    if (!waitset || owner_pid == 0) {
        return -1;
    }
    waitset->count = 0;
    waitset->owner_pid = owner_pid;
    waitset->ownership_enforced = 1;
    return 0;
}

int vibeos_waitset_add(vibeos_waitset_t *waitset, void *event_ptr) {
    if (!waitset || !event_ptr || waitset->count >= VIBEOS_WAITSET_MAX_EVENTS) {
        return -1;
    }
    waitset->events[waitset->count++] = event_ptr;
    return 0;
}

int vibeos_waitset_add_owned(vibeos_waitset_t *waitset, void *event_ptr, uint32_t caller_pid) {
    if (!waitset || !event_ptr || waitset->count >= VIBEOS_WAITSET_MAX_EVENTS || caller_pid == 0) {
        return -1;
    }
    if (waitset->ownership_enforced && waitset->owner_pid != caller_pid) {
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

int vibeos_waitset_owner(const vibeos_waitset_t *waitset, uint32_t *out_owner_pid, uint32_t *out_enforced) {
    if (!waitset || !out_owner_pid || !out_enforced) {
        return -1;
    }
    *out_owner_pid = waitset->owner_pid;
    *out_enforced = waitset->ownership_enforced;
    return 0;
}

int vibeos_waitset_wait(vibeos_waitset_t *waitset, uint64_t timeout_ticks, size_t *out_index) {
    return vibeos_waitset_wait_ex(waitset, timeout_ticks, out_index, 0, 0);
}

int vibeos_waitset_wait_ex(vibeos_waitset_t *waitset, uint64_t timeout_ticks, size_t *out_index, vibeos_scheduler_t *sched, uint32_t cpu_id) {
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
                if (sched) {
                    (void)vibeos_sched_note_wait_wake(sched, cpu_id);
                }
                return 0;
            }
        }
    }
    if (sched) {
        (void)vibeos_sched_note_wait_timeout(sched, cpu_id);
    }
    return -1;
}

int vibeos_waitset_wait_timed(vibeos_waitset_t *waitset, vibeos_timer_t *timer, uint64_t timeout_ticks, size_t *out_index, vibeos_scheduler_t *sched, uint32_t cpu_id) {
    uint64_t start_ticks;
    size_t i;
    if (!waitset || !timer || !out_index) {
        return -1;
    }
    start_ticks = vibeos_timer_ticks(timer);
    for (;;) {
        for (i = 0; i < waitset->count; i++) {
            vibeos_event_t *ev = (vibeos_event_t *)waitset->events[i];
            if (ev && vibeos_event_is_signaled(ev)) {
                *out_index = i;
                if (sched) {
                    (void)vibeos_sched_note_wait_wake(sched, cpu_id);
                }
                return 0;
            }
        }
        if ((vibeos_timer_ticks(timer) - start_ticks) >= timeout_ticks) {
            if (sched) {
                (void)vibeos_sched_note_wait_timeout(sched, cpu_id);
            }
            return -1;
        }
        vibeos_timer_tick(timer);
    }
}
