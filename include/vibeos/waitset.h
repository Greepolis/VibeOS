#ifndef VIBEOS_WAITSET_H
#define VIBEOS_WAITSET_H

#include <stddef.h>
#include <stdint.h>

#include "vibeos/scheduler.h"
#include "vibeos/timer.h"

struct vibeos_process_table;

#define VIBEOS_WAITSET_MAX_EVENTS 32u

typedef enum vibeos_waitset_wake_policy {
    VIBEOS_WAITSET_WAKE_FIFO = 0,
    VIBEOS_WAITSET_WAKE_REVERSE = 1,
    VIBEOS_WAITSET_WAKE_ROUND_ROBIN = 2,
    VIBEOS_WAITSET_WAKE_PRIORITY = 3
} vibeos_waitset_wake_policy_t;

typedef struct vibeos_waitset_ext_stats {
    uint64_t added;
    uint64_t removed;
    uint64_t wait_calls;
    uint64_t wait_wakes;
    uint64_t wait_timeouts;
    uint64_t ownership_denials;
    uint64_t disabled_skips;
    uint64_t priority_wakes;
    uint64_t wait_all_calls;
    uint64_t wait_all_success;
    uint64_t wait_all_timeouts;
    uint64_t wait_batch_calls;
    uint64_t wait_batch_wakes;
    uint64_t peek_calls;
    uint64_t peek_hits;
} vibeos_waitset_ext_stats_t;

typedef struct vibeos_waitset {
    void *events[VIBEOS_WAITSET_MAX_EVENTS];
    uint8_t event_enabled[VIBEOS_WAITSET_MAX_EVENTS];
    uint8_t event_priority[VIBEOS_WAITSET_MAX_EVENTS];
    size_t count;
    uint32_t owner_pid;
    uint32_t ownership_enforced;
    uint32_t active;
    uint32_t wake_policy;
    uint32_t rr_cursor;
    uint64_t stats_added;
    uint64_t stats_removed;
    uint64_t stats_wait_calls;
    uint64_t stats_wait_wakes;
    uint64_t stats_wait_timeouts;
    uint64_t stats_ownership_denials;
    uint64_t stats_disabled_skips;
    uint64_t stats_priority_wakes;
    uint64_t stats_wait_all_calls;
    uint64_t stats_wait_all_success;
    uint64_t stats_wait_all_timeouts;
    uint64_t stats_wait_batch_calls;
    uint64_t stats_wait_batch_wakes;
    uint64_t stats_peek_calls;
    uint64_t stats_peek_hits;
} vibeos_waitset_t;

int vibeos_waitset_init(vibeos_waitset_t *waitset);
int vibeos_waitset_init_owned(vibeos_waitset_t *waitset, uint32_t owner_pid);
int vibeos_waitset_add(vibeos_waitset_t *waitset, void *event_ptr);
int vibeos_waitset_add_with_priority(vibeos_waitset_t *waitset, void *event_ptr, uint8_t priority);
int vibeos_waitset_add_owned(vibeos_waitset_t *waitset, void *event_ptr, uint32_t caller_pid);
int vibeos_waitset_remove(vibeos_waitset_t *waitset, size_t index);
int vibeos_waitset_remove_event(vibeos_waitset_t *waitset, void *event_ptr);
int vibeos_waitset_reset(vibeos_waitset_t *waitset);
int vibeos_waitset_destroy(vibeos_waitset_t *waitset);
int vibeos_waitset_set_event_priority(vibeos_waitset_t *waitset, size_t index, uint8_t priority);
int vibeos_waitset_get_event_priority(const vibeos_waitset_t *waitset, size_t index, uint8_t *out_priority);
int vibeos_waitset_set_event_enabled(vibeos_waitset_t *waitset, size_t index, uint32_t enabled);
int vibeos_waitset_get_event_enabled(const vibeos_waitset_t *waitset, size_t index, uint32_t *out_enabled);
int vibeos_waitset_set_wake_policy(vibeos_waitset_t *waitset, vibeos_waitset_wake_policy_t policy);
int vibeos_waitset_get_wake_policy(const vibeos_waitset_t *waitset, vibeos_waitset_wake_policy_t *out_policy);
int vibeos_waitset_stats(vibeos_waitset_t *waitset, uint64_t *out_added, uint64_t *out_removed, uint64_t *out_wait_calls, uint64_t *out_wait_wakes, uint64_t *out_wait_timeouts, uint64_t *out_ownership_denials);
int vibeos_waitset_stats_ext(vibeos_waitset_t *waitset, vibeos_waitset_ext_stats_t *out_stats);
int vibeos_waitset_stats_reset(vibeos_waitset_t *waitset);
int vibeos_waitset_count(const vibeos_waitset_t *waitset, size_t *out_count);
int vibeos_waitset_owner(const vibeos_waitset_t *waitset, uint32_t *out_owner_pid, uint32_t *out_enforced);
int vibeos_waitset_wait(vibeos_waitset_t *waitset, uint64_t timeout_ticks, size_t *out_index);
int vibeos_waitset_wait_ex(vibeos_waitset_t *waitset, uint64_t timeout_ticks, size_t *out_index, vibeos_scheduler_t *sched, uint32_t cpu_id);
int vibeos_waitset_wait_timed(vibeos_waitset_t *waitset, vibeos_timer_t *timer, uint64_t timeout_ticks, size_t *out_index, vibeos_scheduler_t *sched, uint32_t cpu_id);
int vibeos_waitset_wait_all(vibeos_waitset_t *waitset, uint64_t timeout_ticks, uint32_t consume_signals);
int vibeos_waitset_peek_signaled(vibeos_waitset_t *waitset, size_t *out_indices, size_t max_indices, size_t *out_count);
int vibeos_waitset_wait_batch(vibeos_waitset_t *waitset, uint64_t timeout_ticks, size_t *out_indices, size_t max_indices, size_t *out_count);
int vibeos_waitset_wait_for_thread(vibeos_waitset_t *waitset, uint64_t timeout_ticks, size_t *out_index, vibeos_scheduler_t *sched, uint32_t cpu_id, struct vibeos_process_table *proc_table, uint32_t tid);
int vibeos_waitset_wait_timed_for_thread(vibeos_waitset_t *waitset, vibeos_timer_t *timer, uint64_t timeout_ticks, size_t *out_index, vibeos_scheduler_t *sched, uint32_t cpu_id, struct vibeos_process_table *proc_table, uint32_t tid);
int vibeos_waitset_wait_for_thread_on_cpu(vibeos_waitset_t *waitset, uint64_t timeout_ticks, size_t *out_index, vibeos_scheduler_t *sched, uint32_t cpu_id, struct vibeos_process_table *proc_table, uint32_t tid, uint32_t preferred_wake_cpu_id, uint32_t *out_wake_cpu_id);
int vibeos_waitset_wait_timed_for_thread_on_cpu(vibeos_waitset_t *waitset, vibeos_timer_t *timer, uint64_t timeout_ticks, size_t *out_index, vibeos_scheduler_t *sched, uint32_t cpu_id, struct vibeos_process_table *proc_table, uint32_t tid, uint32_t preferred_wake_cpu_id, uint32_t *out_wake_cpu_id);

#endif
