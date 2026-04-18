#ifndef VIBEOS_SYSCALL_ABI_H
#define VIBEOS_SYSCALL_ABI_H

#include <stdint.h>

#include "vibeos/syscall.h"

/*
 * Syscall ABI v0 mapping.
 *
 * Notes:
 * - The frame layout remains fixed (id, arg0, arg1, arg2, result).
 * - Argument semantics are syscall-group-specific and are centralized here.
 */

static inline void vibeos_syscall_frame_reset(vibeos_syscall_frame_t *f) {
    if (!f) {
        return;
    }
    f->id = 0;
    f->arg0 = 0;
    f->arg1 = 0;
    f->arg2 = 0;
    f->result = 0;
}

/* Common */
static inline uint32_t vibeos_syscall_handle_arg(const vibeos_syscall_frame_t *f) {
    return f ? (uint32_t)f->arg0 : 0;
}

/* Caller identity lane for handle-scoped operations in ABI v0. */
static inline uint32_t vibeos_syscall_caller_pid(const vibeos_syscall_frame_t *f) {
    return f ? (uint32_t)f->arg2 : 0;
}

/* Handle API */
static inline void vibeos_syscall_make_handle_alloc(vibeos_syscall_frame_t *f, uint32_t rights, uint32_t caller_pid) {
    if (!f) {
        return;
    }
    f->id = VIBEOS_SYSCALL_HANDLE_ALLOC;
    f->arg0 = rights;
    f->arg1 = 0;
    f->arg2 = caller_pid;
}

static inline void vibeos_syscall_make_handle_close(vibeos_syscall_frame_t *f, uint32_t handle, uint32_t caller_pid) {
    if (!f) {
        return;
    }
    f->id = VIBEOS_SYSCALL_HANDLE_CLOSE;
    f->arg0 = handle;
    f->arg1 = 0;
    f->arg2 = caller_pid;
}

/* Event API */
static inline void vibeos_syscall_make_event_signal(vibeos_syscall_frame_t *f, uint32_t handle) {
    if (!f) {
        return;
    }
    f->id = VIBEOS_SYSCALL_EVENT_SIGNAL;
    f->arg0 = handle;
    f->arg1 = 0;
    f->arg2 = 0;
}

/* Process API */
static inline void vibeos_syscall_make_process_spawn(vibeos_syscall_frame_t *f, uint32_t parent_pid) {
    if (!f) {
        return;
    }
    f->id = VIBEOS_SYSCALL_PROCESS_SPAWN;
    f->arg0 = parent_pid;
    f->arg1 = 0;
    f->arg2 = 0;
}

static inline void vibeos_syscall_make_process_spawn_as(vibeos_syscall_frame_t *f, uint32_t parent_pid, uint32_t caller_pid) {
    if (!f) {
        return;
    }
    f->id = VIBEOS_SYSCALL_PROCESS_SPAWN;
    f->arg0 = parent_pid;
    f->arg1 = 0;
    f->arg2 = caller_pid;
}

static inline void vibeos_syscall_make_process_state_get(vibeos_syscall_frame_t *f, uint32_t pid, uint32_t caller_pid) {
    if (!f) {
        return;
    }
    f->id = VIBEOS_SYSCALL_PROCESS_STATE_GET;
    f->arg0 = pid;
    f->arg1 = 0;
    f->arg2 = caller_pid;
}

static inline void vibeos_syscall_make_process_state_set(vibeos_syscall_frame_t *f, uint32_t pid, uint32_t state, uint32_t caller_pid) {
    if (!f) {
        return;
    }
    f->id = VIBEOS_SYSCALL_PROCESS_STATE_SET;
    f->arg0 = pid;
    f->arg1 = state;
    f->arg2 = caller_pid;
}

static inline void vibeos_syscall_make_process_terminate(vibeos_syscall_frame_t *f, uint32_t pid, uint32_t caller_pid) {
    if (!f) {
        return;
    }
    f->id = VIBEOS_SYSCALL_PROCESS_TERMINATE;
    f->arg0 = pid;
    f->arg1 = 0;
    f->arg2 = caller_pid;
}

static inline void vibeos_syscall_make_process_count_get(vibeos_syscall_frame_t *f) {
    if (!f) {
        return;
    }
    f->id = VIBEOS_SYSCALL_PROCESS_COUNT_GET;
    f->arg0 = 0;
    f->arg1 = 0;
    f->arg2 = 0;
}

static inline void vibeos_syscall_make_thread_count_get(vibeos_syscall_frame_t *f) {
    if (!f) {
        return;
    }
    f->id = VIBEOS_SYSCALL_THREAD_COUNT_GET;
    f->arg0 = 0;
    f->arg1 = 0;
    f->arg2 = 0;
}

static inline void vibeos_syscall_make_process_live_count_get(vibeos_syscall_frame_t *f) {
    if (!f) {
        return;
    }
    f->id = VIBEOS_SYSCALL_PROCESS_LIVE_COUNT_GET;
    f->arg0 = 0;
    f->arg1 = 0;
    f->arg2 = 0;
}

static inline void vibeos_syscall_make_process_terminated_count_get(vibeos_syscall_frame_t *f) {
    if (!f) {
        return;
    }
    f->id = VIBEOS_SYSCALL_PROCESS_TERMINATED_COUNT_GET;
    f->arg0 = 0;
    f->arg1 = 0;
    f->arg2 = 0;
}

static inline void vibeos_syscall_make_process_state_count_get(vibeos_syscall_frame_t *f, uint32_t state) {
    if (!f) {
        return;
    }
    f->id = VIBEOS_SYSCALL_PROCESS_STATE_COUNT_GET;
    f->arg0 = state;
    f->arg1 = 0;
    f->arg2 = 0;
}

static inline void vibeos_syscall_make_process_state_summary_get(vibeos_syscall_frame_t *f) {
    if (!f) {
        return;
    }
    f->id = VIBEOS_SYSCALL_PROCESS_STATE_SUMMARY_GET;
    f->arg0 = 0;
    f->arg1 = 0;
    f->arg2 = 0;
}

static inline void vibeos_syscall_make_thread_create(vibeos_syscall_frame_t *f, uint32_t pid) {
    if (!f) {
        return;
    }
    f->id = VIBEOS_SYSCALL_THREAD_CREATE;
    f->arg0 = pid;
    f->arg1 = 0;
    f->arg2 = 0;
}

static inline void vibeos_syscall_make_thread_state_get(vibeos_syscall_frame_t *f, uint32_t tid, uint32_t caller_pid) {
    if (!f) {
        return;
    }
    f->id = VIBEOS_SYSCALL_THREAD_STATE_GET;
    f->arg0 = tid;
    f->arg1 = 0;
    f->arg2 = caller_pid;
}

static inline void vibeos_syscall_make_thread_state_set(vibeos_syscall_frame_t *f, uint32_t tid, uint32_t state, uint32_t caller_pid) {
    if (!f) {
        return;
    }
    f->id = VIBEOS_SYSCALL_THREAD_STATE_SET;
    f->arg0 = tid;
    f->arg1 = state;
    f->arg2 = caller_pid;
}

static inline void vibeos_syscall_make_thread_exit(vibeos_syscall_frame_t *f, uint32_t tid, uint32_t caller_pid) {
    if (!f) {
        return;
    }
    f->id = VIBEOS_SYSCALL_THREAD_EXIT;
    f->arg0 = tid;
    f->arg1 = 0;
    f->arg2 = caller_pid;
}

static inline void vibeos_syscall_make_thread_state_count_get(vibeos_syscall_frame_t *f, uint32_t state) {
    if (!f) {
        return;
    }
    f->id = VIBEOS_SYSCALL_THREAD_STATE_COUNT_GET;
    f->arg0 = state;
    f->arg1 = 0;
    f->arg2 = 0;
}

static inline void vibeos_syscall_make_thread_state_summary_get(vibeos_syscall_frame_t *f) {
    if (!f) {
        return;
    }
    f->id = VIBEOS_SYSCALL_THREAD_STATE_SUMMARY_GET;
    f->arg0 = 0;
    f->arg1 = 0;
    f->arg2 = 0;
}

static inline void vibeos_syscall_make_process_thread_state_count_get(vibeos_syscall_frame_t *f, uint32_t target_pid, uint32_t thread_state, uint32_t caller_pid) {
    if (!f) {
        return;
    }
    f->id = VIBEOS_SYSCALL_PROCESS_THREAD_STATE_COUNT_GET;
    f->arg0 = target_pid;
    f->arg1 = thread_state;
    f->arg2 = caller_pid;
}

static inline void vibeos_syscall_make_process_runnable_threads_get(vibeos_syscall_frame_t *f, uint32_t target_pid, uint32_t caller_pid) {
    if (!f) {
        return;
    }
    f->id = VIBEOS_SYSCALL_PROCESS_RUNNABLE_THREADS_GET;
    f->arg0 = target_pid;
    f->arg1 = 0;
    f->arg2 = caller_pid;
}

static inline void vibeos_syscall_make_process_blocked_threads_get(vibeos_syscall_frame_t *f, uint32_t target_pid, uint32_t caller_pid) {
    if (!f) {
        return;
    }
    f->id = VIBEOS_SYSCALL_PROCESS_BLOCKED_THREADS_GET;
    f->arg0 = target_pid;
    f->arg1 = 0;
    f->arg2 = caller_pid;
}

static inline void vibeos_syscall_make_proc_transition_counters_get(vibeos_syscall_frame_t *f) {
    if (!f) {
        return;
    }
    f->id = VIBEOS_SYSCALL_PROC_TRANSITION_COUNTERS_GET;
    f->arg0 = 0;
    f->arg1 = 0;
    f->arg2 = 0;
}

static inline void vibeos_syscall_make_proc_transition_counters_reset(vibeos_syscall_frame_t *f, uint32_t caller_pid) {
    if (!f) {
        return;
    }
    f->id = VIBEOS_SYSCALL_PROC_TRANSITION_COUNTERS_RESET;
    f->arg0 = 0;
    f->arg1 = 0;
    f->arg2 = caller_pid;
}

/* VM API */
static inline void vibeos_syscall_make_vm_map(vibeos_syscall_frame_t *f, uint64_t va, uint64_t pa, uint64_t len) {
    if (!f) {
        return;
    }
    f->id = VIBEOS_SYSCALL_VM_MAP;
    f->arg0 = va;
    f->arg1 = pa;
    f->arg2 = len;
}

static inline void vibeos_syscall_make_vm_unmap(vibeos_syscall_frame_t *f, uint64_t va, uint64_t len) {
    if (!f) {
        return;
    }
    f->id = VIBEOS_SYSCALL_VM_UNMAP;
    f->arg0 = va;
    f->arg1 = len;
    f->arg2 = 0;
}

static inline void vibeos_syscall_make_vm_protect(vibeos_syscall_frame_t *f, uint64_t va, uint64_t len, uint32_t perms) {
    if (!f) {
        return;
    }
    f->id = VIBEOS_SYSCALL_VM_PROTECT;
    f->arg0 = va;
    f->arg1 = len;
    f->arg2 = perms;
}

/* Process audit API */
static inline void vibeos_syscall_make_proc_audit_count(vibeos_syscall_frame_t *f, uint32_t caller_pid) {
    if (!f) {
        return;
    }
    f->id = VIBEOS_SYSCALL_PROC_AUDIT_COUNT;
    f->arg0 = 0;
    f->arg1 = 0;
    f->arg2 = caller_pid;
}

static inline void vibeos_syscall_make_proc_audit_get(vibeos_syscall_frame_t *f, uint32_t index, uint32_t caller_pid) {
    if (!f) {
        return;
    }
    f->id = VIBEOS_SYSCALL_PROC_AUDIT_GET;
    f->arg0 = index;
    f->arg1 = 0;
    f->arg2 = caller_pid;
}

static inline uint32_t vibeos_syscall_audit_event_action(const vibeos_syscall_frame_t *f) {
    return f ? (uint32_t)f->arg0 : 0;
}

static inline uint32_t vibeos_syscall_audit_event_success(const vibeos_syscall_frame_t *f) {
    return f ? (uint32_t)f->arg1 : 0;
}

static inline uint32_t vibeos_syscall_audit_event_revoked_count(const vibeos_syscall_frame_t *f) {
    return f ? (uint32_t)f->arg2 : 0;
}

static inline void vibeos_syscall_make_proc_audit_policy_set(vibeos_syscall_frame_t *f, uint32_t policy, uint32_t caller_pid) {
    if (!f) {
        return;
    }
    f->id = VIBEOS_SYSCALL_PROC_AUDIT_POLICY_SET;
    f->arg0 = policy;
    f->arg1 = 0;
    f->arg2 = caller_pid;
}

static inline void vibeos_syscall_make_proc_audit_policy_get(vibeos_syscall_frame_t *f, uint32_t caller_pid) {
    if (!f) {
        return;
    }
    f->id = VIBEOS_SYSCALL_PROC_AUDIT_POLICY_GET;
    f->arg0 = 0;
    f->arg1 = 0;
    f->arg2 = caller_pid;
}

static inline void vibeos_syscall_make_proc_audit_dropped(vibeos_syscall_frame_t *f, uint32_t caller_pid) {
    if (!f) {
        return;
    }
    f->id = VIBEOS_SYSCALL_PROC_AUDIT_DROPPED;
    f->arg0 = 0;
    f->arg1 = 0;
    f->arg2 = caller_pid;
}

static inline void vibeos_syscall_make_proc_audit_count_action(vibeos_syscall_frame_t *f, uint32_t action, uint32_t caller_pid) {
    if (!f) {
        return;
    }
    f->id = VIBEOS_SYSCALL_PROC_AUDIT_COUNT_ACTION;
    f->arg0 = action;
    f->arg1 = 0;
    f->arg2 = caller_pid;
}

static inline void vibeos_syscall_make_proc_audit_count_success(vibeos_syscall_frame_t *f, uint32_t success_value, uint32_t caller_pid) {
    if (!f) {
        return;
    }
    f->id = VIBEOS_SYSCALL_PROC_AUDIT_COUNT_SUCCESS;
    f->arg0 = success_value;
    f->arg1 = 0;
    f->arg2 = caller_pid;
}

static inline void vibeos_syscall_make_proc_audit_summary(vibeos_syscall_frame_t *f, uint32_t caller_pid) {
    if (!f) {
        return;
    }
    f->id = VIBEOS_SYSCALL_PROC_AUDIT_SUMMARY;
    f->arg0 = 0;
    f->arg1 = 0;
    f->arg2 = caller_pid;
}

static inline void vibeos_syscall_make_sched_runnable_get(vibeos_syscall_frame_t *f) {
    if (!f) {
        return;
    }
    f->id = VIBEOS_SYSCALL_SCHED_RUNNABLE_GET;
    f->arg0 = 0;
    f->arg1 = 0;
    f->arg2 = 0;
}

static inline void vibeos_syscall_make_sched_runqueue_depth_get(vibeos_syscall_frame_t *f, uint32_t cpu_id) {
    if (!f) {
        return;
    }
    f->id = VIBEOS_SYSCALL_SCHED_RUNQUEUE_DEPTH_GET;
    f->arg0 = cpu_id;
    f->arg1 = 0;
    f->arg2 = 0;
}

static inline void vibeos_syscall_make_sched_cpu_count_get(vibeos_syscall_frame_t *f) {
    if (!f) {
        return;
    }
    f->id = VIBEOS_SYSCALL_SCHED_CPU_COUNT_GET;
    f->arg0 = 0;
    f->arg1 = 0;
    f->arg2 = 0;
}

static inline void vibeos_syscall_make_sched_preemptions_get(vibeos_syscall_frame_t *f, uint32_t cpu_id) {
    if (!f) {
        return;
    }
    f->id = VIBEOS_SYSCALL_SCHED_PREEMPTIONS_GET;
    f->arg0 = cpu_id;
    f->arg1 = 0;
    f->arg2 = 0;
}

static inline void vibeos_syscall_make_sched_wait_timeouts_get(vibeos_syscall_frame_t *f, uint32_t cpu_id) {
    if (!f) {
        return;
    }
    f->id = VIBEOS_SYSCALL_SCHED_WAIT_TIMEOUTS_GET;
    f->arg0 = cpu_id;
    f->arg1 = 0;
    f->arg2 = 0;
}

static inline void vibeos_syscall_make_sched_wait_wakes_get(vibeos_syscall_frame_t *f, uint32_t cpu_id) {
    if (!f) {
        return;
    }
    f->id = VIBEOS_SYSCALL_SCHED_WAIT_WAKES_GET;
    f->arg0 = cpu_id;
    f->arg1 = 0;
    f->arg2 = 0;
}

static inline void vibeos_syscall_make_sched_preemptions_total_get(vibeos_syscall_frame_t *f) {
    if (!f) {
        return;
    }
    f->id = VIBEOS_SYSCALL_SCHED_PREEMPTIONS_TOTAL_GET;
    f->arg0 = 0;
    f->arg1 = 0;
    f->arg2 = 0;
}

static inline void vibeos_syscall_make_sched_wait_timeouts_total_get(vibeos_syscall_frame_t *f) {
    if (!f) {
        return;
    }
    f->id = VIBEOS_SYSCALL_SCHED_WAIT_TIMEOUTS_TOTAL_GET;
    f->arg0 = 0;
    f->arg1 = 0;
    f->arg2 = 0;
}

static inline void vibeos_syscall_make_sched_wait_wakes_total_get(vibeos_syscall_frame_t *f) {
    if (!f) {
        return;
    }
    f->id = VIBEOS_SYSCALL_SCHED_WAIT_WAKES_TOTAL_GET;
    f->arg0 = 0;
    f->arg1 = 0;
    f->arg2 = 0;
}

static inline void vibeos_syscall_make_sched_counter_summary_get(vibeos_syscall_frame_t *f) {
    if (!f) {
        return;
    }
    f->id = VIBEOS_SYSCALL_SCHED_COUNTER_SUMMARY_GET;
    f->arg0 = 0;
    f->arg1 = 0;
    f->arg2 = 0;
}

static inline void vibeos_syscall_make_sched_counters_reset(vibeos_syscall_frame_t *f, uint32_t caller_pid) {
    if (!f) {
        return;
    }
    f->id = VIBEOS_SYSCALL_SCHED_COUNTERS_RESET;
    f->arg0 = 0;
    f->arg1 = 0;
    f->arg2 = caller_pid;
}

static inline void vibeos_syscall_make_sched_tracked_threads_get(vibeos_syscall_frame_t *f) {
    if (!f) {
        return;
    }
    f->id = VIBEOS_SYSCALL_SCHED_TRACKED_THREADS_GET;
    f->arg0 = 0;
    f->arg1 = 0;
    f->arg2 = 0;
}

static inline void vibeos_syscall_make_sched_blocked_threads_get(vibeos_syscall_frame_t *f) {
    if (!f) {
        return;
    }
    f->id = VIBEOS_SYSCALL_SCHED_BLOCKED_THREADS_GET;
    f->arg0 = 0;
    f->arg1 = 0;
    f->arg2 = 0;
}

static inline void vibeos_syscall_make_sched_wait_transition_summary_get(vibeos_syscall_frame_t *f) {
    if (!f) {
        return;
    }
    f->id = VIBEOS_SYSCALL_SCHED_WAIT_TRANSITION_SUMMARY_GET;
    f->arg0 = 0;
    f->arg1 = 0;
    f->arg2 = 0;
}

static inline void vibeos_syscall_make_sched_thread_runtime_get(vibeos_syscall_frame_t *f, uint32_t tid) {
    if (!f) {
        return;
    }
    f->id = VIBEOS_SYSCALL_SCHED_THREAD_RUNTIME_GET;
    f->arg0 = tid;
    f->arg1 = 0;
    f->arg2 = 0;
}

static inline void vibeos_syscall_make_sched_thread_affinity_set(vibeos_syscall_frame_t *f, uint32_t tid, uint32_t affinity_low, uint32_t affinity_high, uint32_t caller_pid) {
    if (!f) {
        return;
    }
    f->id = VIBEOS_SYSCALL_SCHED_THREAD_AFFINITY_SET;
    f->arg0 = tid;
    f->arg1 = ((uint64_t)affinity_high << 32) | affinity_low;
    f->arg2 = caller_pid;
}

static inline void vibeos_syscall_make_sched_thread_affinity_get(vibeos_syscall_frame_t *f, uint32_t tid) {
    if (!f) {
        return;
    }
    f->id = VIBEOS_SYSCALL_SCHED_THREAD_AFFINITY_GET;
    f->arg0 = tid;
    f->arg1 = 0;
    f->arg2 = 0;
}

static inline void vibeos_syscall_make_sched_thread_nice_set(vibeos_syscall_frame_t *f, uint32_t tid, int32_t nice_level, uint32_t caller_pid) {
    if (!f) {
        return;
    }
    f->id = VIBEOS_SYSCALL_SCHED_THREAD_NICE_SET;
    f->arg0 = tid;
    f->arg1 = (uint32_t)nice_level;
    f->arg2 = caller_pid;
}

static inline void vibeos_syscall_make_sched_thread_nice_get(vibeos_syscall_frame_t *f, uint32_t tid) {
    if (!f) {
        return;
    }
    f->id = VIBEOS_SYSCALL_SCHED_THREAD_NICE_GET;
    f->arg0 = tid;
    f->arg1 = 0;
    f->arg2 = 0;
}

static inline void vibeos_syscall_make_sched_rebalance(vibeos_syscall_frame_t *f, uint32_t max_moves, uint32_t caller_pid) {
    if (!f) {
        return;
    }
    f->id = VIBEOS_SYSCALL_SCHED_REBALANCE;
    f->arg0 = max_moves;
    f->arg1 = 0;
    f->arg2 = caller_pid;
}

static inline void vibeos_syscall_make_sched_starvation_tick(vibeos_syscall_frame_t *f, uint32_t cpu_id) {
    if (!f) {
        return;
    }
    f->id = VIBEOS_SYSCALL_SCHED_STARVATION_TICK;
    f->arg0 = cpu_id;
    f->arg1 = 0;
    f->arg2 = 0;
}

static inline void vibeos_syscall_make_sched_qos_summary_get(vibeos_syscall_frame_t *f) {
    if (!f) {
        return;
    }
    f->id = VIBEOS_SYSCALL_SCHED_QOS_SUMMARY_GET;
    f->arg0 = 0;
    f->arg1 = 0;
    f->arg2 = 0;
}

static inline void vibeos_syscall_make_sched_boost_starving(vibeos_syscall_frame_t *f, uint32_t starvation_threshold, uint32_t boost_ticks, uint32_t max_timeslice, uint32_t caller_pid) {
    if (!f) {
        return;
    }
    f->id = VIBEOS_SYSCALL_SCHED_BOOST_STARVING;
    f->arg0 = starvation_threshold;
    f->arg1 = ((uint64_t)max_timeslice << 32) | boost_ticks;
    f->arg2 = caller_pid;
}

static inline void vibeos_syscall_make_process_token_get(vibeos_syscall_frame_t *f, uint32_t target_pid, uint32_t caller_pid) {
    if (!f) {
        return;
    }
    f->id = VIBEOS_SYSCALL_PROCESS_TOKEN_GET;
    f->arg0 = target_pid;
    f->arg1 = 0;
    f->arg2 = caller_pid;
}

static inline void vibeos_syscall_make_process_token_set(vibeos_syscall_frame_t *f, uint32_t target_pid, uint32_t capability_mask, uint32_t caller_pid) {
    if (!f) {
        return;
    }
    f->id = VIBEOS_SYSCALL_PROCESS_TOKEN_SET;
    f->arg0 = target_pid;
    f->arg1 = capability_mask;
    f->arg2 = caller_pid;
}

static inline void vibeos_syscall_make_process_security_label_get(vibeos_syscall_frame_t *f, uint32_t target_pid, uint32_t caller_pid) {
    if (!f) {
        return;
    }
    f->id = VIBEOS_SYSCALL_PROCESS_SECURITY_LABEL_GET;
    f->arg0 = target_pid;
    f->arg1 = 0;
    f->arg2 = caller_pid;
}

static inline void vibeos_syscall_make_process_security_label_set(vibeos_syscall_frame_t *f, uint32_t target_pid, uint32_t label, uint32_t caller_pid) {
    if (!f) {
        return;
    }
    f->id = VIBEOS_SYSCALL_PROCESS_SECURITY_LABEL_SET;
    f->arg0 = target_pid;
    f->arg1 = label;
    f->arg2 = caller_pid;
}

static inline void vibeos_syscall_make_process_interact_check(vibeos_syscall_frame_t *f, uint32_t target_pid, uint32_t caller_pid) {
    if (!f) {
        return;
    }
    f->id = VIBEOS_SYSCALL_PROCESS_INTERACT_CHECK;
    f->arg0 = target_pid;
    f->arg1 = 0;
    f->arg2 = caller_pid;
}

static inline void vibeos_syscall_make_policy_capability_get(vibeos_syscall_frame_t *f, uint32_t target, uint32_t caller_pid) {
    if (!f) {
        return;
    }
    f->id = VIBEOS_SYSCALL_POLICY_CAPABILITY_GET;
    f->arg0 = target;
    f->arg1 = 0;
    f->arg2 = caller_pid;
}

static inline void vibeos_syscall_make_policy_capability_set(vibeos_syscall_frame_t *f, uint32_t target, uint32_t capability_bit, uint32_t caller_pid) {
    if (!f) {
        return;
    }
    f->id = VIBEOS_SYSCALL_POLICY_CAPABILITY_SET;
    f->arg0 = target;
    f->arg1 = capability_bit;
    f->arg2 = caller_pid;
}

static inline void vibeos_syscall_make_policy_summary_get(vibeos_syscall_frame_t *f, uint32_t caller_pid) {
    if (!f) {
        return;
    }
    f->id = VIBEOS_SYSCALL_POLICY_SUMMARY_GET;
    f->arg0 = 0;
    f->arg1 = 0;
    f->arg2 = caller_pid;
}

static inline void vibeos_syscall_make_sec_audit_count(vibeos_syscall_frame_t *f, uint32_t caller_pid) {
    if (!f) {
        return;
    }
    f->id = VIBEOS_SYSCALL_SEC_AUDIT_COUNT;
    f->arg0 = 0;
    f->arg1 = 0;
    f->arg2 = caller_pid;
}

static inline void vibeos_syscall_make_sec_audit_get(vibeos_syscall_frame_t *f, uint32_t index, uint32_t caller_pid) {
    if (!f) {
        return;
    }
    f->id = VIBEOS_SYSCALL_SEC_AUDIT_GET;
    f->arg0 = index;
    f->arg1 = 0;
    f->arg2 = caller_pid;
}

static inline void vibeos_syscall_make_sec_audit_count_action(vibeos_syscall_frame_t *f, uint32_t action, uint32_t caller_pid) {
    if (!f) {
        return;
    }
    f->id = VIBEOS_SYSCALL_SEC_AUDIT_COUNT_ACTION;
    f->arg0 = action;
    f->arg1 = 0;
    f->arg2 = caller_pid;
}

static inline void vibeos_syscall_make_sec_audit_count_success(vibeos_syscall_frame_t *f, uint32_t success_value, uint32_t caller_pid) {
    if (!f) {
        return;
    }
    f->id = VIBEOS_SYSCALL_SEC_AUDIT_COUNT_SUCCESS;
    f->arg0 = success_value;
    f->arg1 = 0;
    f->arg2 = caller_pid;
}

static inline void vibeos_syscall_make_sec_audit_summary(vibeos_syscall_frame_t *f, uint32_t caller_pid) {
    if (!f) {
        return;
    }
    f->id = VIBEOS_SYSCALL_SEC_AUDIT_SUMMARY;
    f->arg0 = 0;
    f->arg1 = 0;
    f->arg2 = caller_pid;
}

static inline void vibeos_syscall_make_sec_audit_reset(vibeos_syscall_frame_t *f, uint32_t caller_pid) {
    if (!f) {
        return;
    }
    f->id = VIBEOS_SYSCALL_SEC_AUDIT_RESET;
    f->arg0 = 0;
    f->arg1 = 0;
    f->arg2 = caller_pid;
}

/* Waitset API */
static inline uint32_t vibeos_syscall_waitset_owner_pid(const vibeos_syscall_frame_t *f) {
    return f ? (uint32_t)f->arg1 : 0;
}

static inline void vibeos_syscall_make_waitset_add_event(vibeos_syscall_frame_t *f, uint32_t event_handle, uint32_t owner_pid, uint32_t caller_pid) {
    if (!f) {
        return;
    }
    f->id = VIBEOS_SYSCALL_WAITSET_ADD_EVENT;
    f->arg0 = event_handle;
    f->arg1 = owner_pid;
    f->arg2 = caller_pid;
}

static inline void vibeos_syscall_make_waitset_stats_get(vibeos_syscall_frame_t *f, uint32_t caller_pid) {
    if (!f) {
        return;
    }
    f->id = VIBEOS_SYSCALL_WAITSET_STATS_GET;
    f->arg0 = 0;
    f->arg1 = 0;
    f->arg2 = caller_pid;
}

static inline void vibeos_syscall_make_waitset_stats_ext_get(vibeos_syscall_frame_t *f, uint32_t caller_pid) {
    if (!f) {
        return;
    }
    f->id = VIBEOS_SYSCALL_WAITSET_STATS_EXT_GET;
    f->arg0 = 0;
    f->arg1 = 0;
    f->arg2 = caller_pid;
}

static inline void vibeos_syscall_make_waitset_wake_policy_set(vibeos_syscall_frame_t *f, uint32_t policy, uint32_t caller_pid) {
    if (!f) {
        return;
    }
    f->id = VIBEOS_SYSCALL_WAITSET_WAKE_POLICY_SET;
    f->arg0 = policy;
    f->arg1 = 0;
    f->arg2 = caller_pid;
}

static inline void vibeos_syscall_make_waitset_wake_policy_get(vibeos_syscall_frame_t *f, uint32_t caller_pid) {
    if (!f) {
        return;
    }
    f->id = VIBEOS_SYSCALL_WAITSET_WAKE_POLICY_GET;
    f->arg0 = 0;
    f->arg1 = 0;
    f->arg2 = caller_pid;
}

static inline void vibeos_syscall_make_waitset_stats_reset(vibeos_syscall_frame_t *f, uint32_t caller_pid) {
    if (!f) {
        return;
    }
    f->id = VIBEOS_SYSCALL_WAITSET_STATS_RESET;
    f->arg0 = 0;
    f->arg1 = 0;
    f->arg2 = caller_pid;
}

static inline void vibeos_syscall_make_waitset_owner_get(vibeos_syscall_frame_t *f, uint32_t caller_pid) {
    if (!f) {
        return;
    }
    f->id = VIBEOS_SYSCALL_WAITSET_OWNER_GET;
    f->arg0 = 0;
    f->arg1 = 0;
    f->arg2 = caller_pid;
}

static inline void vibeos_syscall_make_waitset_snapshot_get(vibeos_syscall_frame_t *f, uint32_t caller_pid) {
    if (!f) {
        return;
    }
    f->id = VIBEOS_SYSCALL_WAITSET_SNAPSHOT_GET;
    f->arg0 = 0;
    f->arg1 = 0;
    f->arg2 = caller_pid;
}

#endif
