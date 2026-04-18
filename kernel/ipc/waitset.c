#include "vibeos/waitset.h"
#include "vibeos/ipc.h"
#include "vibeos/proc.h"

static int waitset_find_signaled(vibeos_waitset_t *waitset, size_t *out_index) {
    size_t i;
    if (!waitset || !out_index || waitset->count == 0) {
        return -1;
    }
    if (waitset->wake_policy == VIBEOS_WAITSET_WAKE_REVERSE) {
        for (i = waitset->count; i > 0; i--) {
            vibeos_event_t *ev = (vibeos_event_t *)waitset->events[i - 1];
            if (ev && vibeos_event_is_signaled(ev)) {
                *out_index = i - 1;
                return 0;
            }
        }
        return -1;
    }
    if (waitset->wake_policy == VIBEOS_WAITSET_WAKE_ROUND_ROBIN) {
        for (i = 0; i < waitset->count; i++) {
            size_t idx = ((size_t)waitset->rr_cursor + i) % waitset->count;
            vibeos_event_t *ev = (vibeos_event_t *)waitset->events[idx];
            if (ev && vibeos_event_is_signaled(ev)) {
                *out_index = idx;
                waitset->rr_cursor = (uint32_t)((idx + 1u) % waitset->count);
                return 0;
            }
        }
        return -1;
    }
    for (i = 0; i < waitset->count; i++) {
        vibeos_event_t *ev = (vibeos_event_t *)waitset->events[i];
        if (ev && vibeos_event_is_signaled(ev)) {
            *out_index = i;
            return 0;
        }
    }
    return -1;
}

int vibeos_waitset_init(vibeos_waitset_t *waitset) {
    size_t i;
    if (!waitset) {
        return -1;
    }
    for (i = 0; i < VIBEOS_WAITSET_MAX_EVENTS; i++) {
        waitset->events[i] = 0;
    }
    waitset->count = 0;
    waitset->owner_pid = 0;
    waitset->ownership_enforced = 0;
    waitset->active = 1;
    waitset->wake_policy = VIBEOS_WAITSET_WAKE_FIFO;
    waitset->rr_cursor = 0;
    waitset->stats_added = 0;
    waitset->stats_removed = 0;
    waitset->stats_wait_calls = 0;
    waitset->stats_wait_wakes = 0;
    waitset->stats_wait_timeouts = 0;
    waitset->stats_ownership_denials = 0;
    return 0;
}

int vibeos_waitset_init_owned(vibeos_waitset_t *waitset, uint32_t owner_pid) {
    size_t i;
    if (!waitset || owner_pid == 0) {
        return -1;
    }
    for (i = 0; i < VIBEOS_WAITSET_MAX_EVENTS; i++) {
        waitset->events[i] = 0;
    }
    waitset->count = 0;
    waitset->owner_pid = owner_pid;
    waitset->ownership_enforced = 1;
    waitset->active = 1;
    waitset->wake_policy = VIBEOS_WAITSET_WAKE_FIFO;
    waitset->rr_cursor = 0;
    waitset->stats_added = 0;
    waitset->stats_removed = 0;
    waitset->stats_wait_calls = 0;
    waitset->stats_wait_wakes = 0;
    waitset->stats_wait_timeouts = 0;
    waitset->stats_ownership_denials = 0;
    return 0;
}

int vibeos_waitset_add(vibeos_waitset_t *waitset, void *event_ptr) {
    if (!waitset || !waitset->active || !event_ptr || waitset->count >= VIBEOS_WAITSET_MAX_EVENTS) {
        return -1;
    }
    waitset->events[waitset->count++] = event_ptr;
    if (waitset->rr_cursor >= waitset->count) {
        waitset->rr_cursor = 0;
    }
    waitset->stats_added++;
    return 0;
}

int vibeos_waitset_add_owned(vibeos_waitset_t *waitset, void *event_ptr, uint32_t caller_pid) {
    if (!waitset || !waitset->active || !event_ptr || waitset->count >= VIBEOS_WAITSET_MAX_EVENTS || caller_pid == 0) {
        return -1;
    }
    if (waitset->ownership_enforced && waitset->owner_pid != caller_pid) {
        waitset->stats_ownership_denials++;
        return -1;
    }
    waitset->events[waitset->count++] = event_ptr;
    if (waitset->rr_cursor >= waitset->count) {
        waitset->rr_cursor = 0;
    }
    waitset->stats_added++;
    return 0;
}

int vibeos_waitset_remove(vibeos_waitset_t *waitset, size_t index) {
    size_t i;
    if (!waitset || !waitset->active || index >= waitset->count) {
        return -1;
    }
    for (i = index; i + 1 < waitset->count; i++) {
        waitset->events[i] = waitset->events[i + 1];
    }
    waitset->events[waitset->count - 1] = 0;
    waitset->count--;
    if (waitset->count == 0) {
        waitset->rr_cursor = 0;
    } else if (waitset->rr_cursor >= waitset->count) {
        waitset->rr_cursor %= (uint32_t)waitset->count;
    }
    waitset->stats_removed++;
    return 0;
}

int vibeos_waitset_reset(vibeos_waitset_t *waitset) {
    size_t i;
    size_t removed_count;
    if (!waitset || !waitset->active) {
        return -1;
    }
    removed_count = waitset->count;
    for (i = 0; i < VIBEOS_WAITSET_MAX_EVENTS; i++) {
        waitset->events[i] = 0;
    }
    waitset->count = 0;
    waitset->rr_cursor = 0;
    waitset->stats_removed += (uint64_t)removed_count;
    return 0;
}

int vibeos_waitset_destroy(vibeos_waitset_t *waitset) {
    if (!waitset || !waitset->active) {
        return -1;
    }
    if (vibeos_waitset_reset(waitset) != 0) {
        return -1;
    }
    waitset->owner_pid = 0;
    waitset->ownership_enforced = 0;
    waitset->wake_policy = VIBEOS_WAITSET_WAKE_FIFO;
    waitset->rr_cursor = 0;
    waitset->stats_added = 0;
    waitset->stats_removed = 0;
    waitset->stats_wait_calls = 0;
    waitset->stats_wait_wakes = 0;
    waitset->stats_wait_timeouts = 0;
    waitset->stats_ownership_denials = 0;
    waitset->active = 0;
    return 0;
}

int vibeos_waitset_set_wake_policy(vibeos_waitset_t *waitset, vibeos_waitset_wake_policy_t policy) {
    if (!waitset || !waitset->active) {
        return -1;
    }
    if (policy != VIBEOS_WAITSET_WAKE_FIFO &&
        policy != VIBEOS_WAITSET_WAKE_REVERSE &&
        policy != VIBEOS_WAITSET_WAKE_ROUND_ROBIN) {
        return -1;
    }
    waitset->wake_policy = (uint32_t)policy;
    return 0;
}

int vibeos_waitset_get_wake_policy(const vibeos_waitset_t *waitset, vibeos_waitset_wake_policy_t *out_policy) {
    if (!waitset || !waitset->active || !out_policy) {
        return -1;
    }
    *out_policy = (vibeos_waitset_wake_policy_t)waitset->wake_policy;
    return 0;
}

int vibeos_waitset_stats(vibeos_waitset_t *waitset, uint64_t *out_added, uint64_t *out_removed, uint64_t *out_wait_calls, uint64_t *out_wait_wakes, uint64_t *out_wait_timeouts, uint64_t *out_ownership_denials) {
    if (!waitset || !waitset->active || !out_added || !out_removed || !out_wait_calls || !out_wait_wakes || !out_wait_timeouts || !out_ownership_denials) {
        return -1;
    }
    *out_added = waitset->stats_added;
    *out_removed = waitset->stats_removed;
    *out_wait_calls = waitset->stats_wait_calls;
    *out_wait_wakes = waitset->stats_wait_wakes;
    *out_wait_timeouts = waitset->stats_wait_timeouts;
    *out_ownership_denials = waitset->stats_ownership_denials;
    return 0;
}

int vibeos_waitset_stats_reset(vibeos_waitset_t *waitset) {
    if (!waitset || !waitset->active) {
        return -1;
    }
    waitset->stats_added = 0;
    waitset->stats_removed = 0;
    waitset->stats_wait_calls = 0;
    waitset->stats_wait_wakes = 0;
    waitset->stats_wait_timeouts = 0;
    waitset->stats_ownership_denials = 0;
    return 0;
}

int vibeos_waitset_count(const vibeos_waitset_t *waitset, size_t *out_count) {
    if (!waitset || !waitset->active || !out_count) {
        return -1;
    }
    *out_count = waitset->count;
    return 0;
}

int vibeos_waitset_owner(const vibeos_waitset_t *waitset, uint32_t *out_owner_pid, uint32_t *out_enforced) {
    if (!waitset || !waitset->active || !out_owner_pid || !out_enforced) {
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
    if (!waitset || !waitset->active || !out_index) {
        return -1;
    }
    waitset->stats_wait_calls++;
    for (tick = 0; tick <= timeout_ticks; tick++) {
        if (waitset_find_signaled(waitset, out_index) == 0) {
            if (sched) {
                (void)vibeos_sched_note_wait_wake(sched, cpu_id);
            }
            waitset->stats_wait_wakes++;
            return 0;
        }
    }
    if (sched) {
        (void)vibeos_sched_note_wait_timeout(sched, cpu_id);
    }
    waitset->stats_wait_timeouts++;
    return -1;
}

int vibeos_waitset_wait_timed(vibeos_waitset_t *waitset, vibeos_timer_t *timer, uint64_t timeout_ticks, size_t *out_index, vibeos_scheduler_t *sched, uint32_t cpu_id) {
    uint64_t start_ticks;
    if (!waitset || !waitset->active || !timer || !out_index) {
        return -1;
    }
    waitset->stats_wait_calls++;
    start_ticks = vibeos_timer_ticks(timer);
    for (;;) {
        if (waitset_find_signaled(waitset, out_index) == 0) {
            if (sched) {
                (void)vibeos_sched_note_wait_wake(sched, cpu_id);
            }
            waitset->stats_wait_wakes++;
            return 0;
        }
        if ((vibeos_timer_ticks(timer) - start_ticks) >= timeout_ticks) {
            if (sched) {
                (void)vibeos_sched_note_wait_timeout(sched, cpu_id);
            }
            waitset->stats_wait_timeouts++;
            return -1;
        }
        vibeos_timer_tick(timer);
    }
}

static int waitset_thread_wait_begin(vibeos_scheduler_t *sched, struct vibeos_process_table *proc_table, uint32_t tid) {
    if (!proc_table || tid == 0) {
        return -1;
    }
    if (vibeos_proc_thread_wait_begin(proc_table, tid) != 0) {
        return -1;
    }
    if (sched && vibeos_sched_wait_begin(sched, tid, 0) != 0) {
        (void)vibeos_proc_thread_wait_end(proc_table, tid);
        return -1;
    }
    return 0;
}

static int waitset_thread_wait_end(vibeos_scheduler_t *sched, struct vibeos_process_table *proc_table, uint32_t tid, uint32_t preferred_wake_cpu_id, uint32_t *out_wake_cpu_id) {
    if (!proc_table || tid == 0) {
        return -1;
    }
    if (sched && vibeos_sched_wait_end(sched, tid, preferred_wake_cpu_id, out_wake_cpu_id) != 0) {
        return -1;
    }
    if (vibeos_proc_thread_wait_end(proc_table, tid) != 0) {
        if (sched) {
            (void)vibeos_sched_wait_begin(sched, tid, 0);
        }
        return -1;
    }
    return 0;
}

int vibeos_waitset_wait_for_thread_on_cpu(vibeos_waitset_t *waitset, uint64_t timeout_ticks, size_t *out_index, vibeos_scheduler_t *sched, uint32_t cpu_id, struct vibeos_process_table *proc_table, uint32_t tid, uint32_t preferred_wake_cpu_id, uint32_t *out_wake_cpu_id) {
    int rc;
    if (!waitset || !out_index || !proc_table || tid == 0) {
        return -1;
    }
    if (waitset_thread_wait_begin(sched, proc_table, tid) != 0) {
        return -1;
    }
    rc = vibeos_waitset_wait_ex(waitset, timeout_ticks, out_index, sched, cpu_id);
    if (waitset_thread_wait_end(sched, proc_table, tid, preferred_wake_cpu_id, out_wake_cpu_id) != 0) {
        return -1;
    }
    return rc;
}

int vibeos_waitset_wait_timed_for_thread_on_cpu(vibeos_waitset_t *waitset, vibeos_timer_t *timer, uint64_t timeout_ticks, size_t *out_index, vibeos_scheduler_t *sched, uint32_t cpu_id, struct vibeos_process_table *proc_table, uint32_t tid, uint32_t preferred_wake_cpu_id, uint32_t *out_wake_cpu_id) {
    int rc;
    if (!waitset || !timer || !out_index || !proc_table || tid == 0) {
        return -1;
    }
    if (waitset_thread_wait_begin(sched, proc_table, tid) != 0) {
        return -1;
    }
    rc = vibeos_waitset_wait_timed(waitset, timer, timeout_ticks, out_index, sched, cpu_id);
    if (waitset_thread_wait_end(sched, proc_table, tid, preferred_wake_cpu_id, out_wake_cpu_id) != 0) {
        return -1;
    }
    return rc;
}

int vibeos_waitset_wait_for_thread(vibeos_waitset_t *waitset, uint64_t timeout_ticks, size_t *out_index, vibeos_scheduler_t *sched, uint32_t cpu_id, struct vibeos_process_table *proc_table, uint32_t tid) {
    return vibeos_waitset_wait_for_thread_on_cpu(waitset, timeout_ticks, out_index, sched, cpu_id, proc_table, tid, cpu_id, 0);
}

int vibeos_waitset_wait_timed_for_thread(vibeos_waitset_t *waitset, vibeos_timer_t *timer, uint64_t timeout_ticks, size_t *out_index, vibeos_scheduler_t *sched, uint32_t cpu_id, struct vibeos_process_table *proc_table, uint32_t tid) {
    return vibeos_waitset_wait_timed_for_thread_on_cpu(waitset, timer, timeout_ticks, out_index, sched, cpu_id, proc_table, tid, cpu_id, 0);
}
