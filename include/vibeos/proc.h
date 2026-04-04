#ifndef VIBEOS_PROC_H
#define VIBEOS_PROC_H

#include <stdint.h>

#include "vibeos/object.h"

#define VIBEOS_MAX_PROCESSES 32u
#define VIBEOS_PROC_MAX_THREADS 256u
#define VIBEOS_PROC_AUDIT_CAPACITY 128u

typedef enum vibeos_process_state {
    VIBEOS_PROCESS_STATE_NEW = 0,
    VIBEOS_PROCESS_STATE_RUNNING = 1,
    VIBEOS_PROCESS_STATE_BLOCKED = 2,
    VIBEOS_PROCESS_STATE_TERMINATED = 3
} vibeos_process_state_t;

typedef struct vibeos_process_entry {
    uint32_t pid;
    uint32_t parent_pid;
    uint32_t in_use;
    vibeos_process_state_t state;
    vibeos_handle_table_t handles;
} vibeos_process_entry_t;

typedef enum vibeos_thread_state {
    VIBEOS_THREAD_STATE_NEW = 0,
    VIBEOS_THREAD_STATE_RUNNABLE = 1,
    VIBEOS_THREAD_STATE_BLOCKED = 2,
    VIBEOS_THREAD_STATE_EXITED = 3
} vibeos_thread_state_t;

typedef struct vibeos_thread_entry {
    uint32_t tid;
    uint32_t owner_pid;
    uint32_t in_use;
    vibeos_thread_state_t state;
} vibeos_thread_entry_t;

typedef struct vibeos_proc_audit_event {
    uint64_t seq;
    uint32_t action;
    uint32_t owner_pid;
    uint32_t request_handle;
    uint32_t root_pid;
    uint32_t root_handle;
    uint32_t object_type_filter;
    uint32_t rights_mask_filter;
    uint32_t revoked_count;
    uint32_t success;
} vibeos_proc_audit_event_t;

typedef enum vibeos_proc_audit_action {
    VIBEOS_PROC_AUDIT_REVOKE_LINEAGE = 1,
    VIBEOS_PROC_AUDIT_REVOKE_LINEAGE_SCOPED = 2
} vibeos_proc_audit_action_t;

typedef enum vibeos_proc_audit_policy {
    VIBEOS_PROC_AUDIT_OVERWRITE_OLDEST = 0,
    VIBEOS_PROC_AUDIT_DROP_NEWEST = 1
} vibeos_proc_audit_policy_t;

typedef struct vibeos_process_table {
    uint32_t next_pid;
    uint32_t next_tid;
    uint32_t process_count;
    uint32_t thread_count;
    vibeos_process_entry_t entries[VIBEOS_MAX_PROCESSES];
    vibeos_thread_entry_t threads[VIBEOS_PROC_MAX_THREADS];
    uint64_t audit_seq;
    uint32_t audit_head;
    uint32_t audit_count;
    uint32_t audit_dropped;
    uint64_t proc_state_transitions;
    uint64_t thread_state_transitions;
    uint64_t proc_terminations;
    uint64_t thread_exits;
    vibeos_proc_audit_policy_t audit_policy;
    vibeos_proc_audit_event_t audit_events[VIBEOS_PROC_AUDIT_CAPACITY];
} vibeos_process_table_t;

int vibeos_proc_init(vibeos_process_table_t *pt);
int vibeos_proc_spawn(vibeos_process_table_t *pt, uint32_t parent_pid, uint32_t *out_pid);
int vibeos_proc_are_related(vibeos_process_table_t *pt, uint32_t pid_a, uint32_t pid_b);
int vibeos_thread_create(vibeos_process_table_t *pt, uint32_t pid, uint32_t *out_tid);
int vibeos_thread_owner(vibeos_process_table_t *pt, uint32_t tid, uint32_t *out_owner_pid);
int vibeos_proc_handles(vibeos_process_table_t *pt, uint32_t pid, vibeos_handle_table_t **out_handles);
int vibeos_proc_duplicate_handle(vibeos_process_table_t *pt, uint32_t src_pid, uint32_t dst_pid, uint32_t src_handle, uint32_t requested_rights, uint32_t *out_dst_handle);
int vibeos_proc_revoke_handle_lineage(vibeos_process_table_t *pt, uint32_t owner_pid, uint32_t handle);
int vibeos_proc_revoke_handle_lineage_scoped(vibeos_process_table_t *pt, uint32_t owner_pid, uint32_t handle, vibeos_object_type_t object_type_filter, uint32_t rights_mask_filter);
int vibeos_proc_state(vibeos_process_table_t *pt, uint32_t pid, vibeos_process_state_t *out_state);
int vibeos_proc_set_state(vibeos_process_table_t *pt, uint32_t pid, vibeos_process_state_t state);
int vibeos_proc_terminate(vibeos_process_table_t *pt, uint32_t pid);
int vibeos_proc_bind_process_handle(vibeos_process_table_t *pt, uint32_t owner_pid, uint32_t target_pid, uint32_t rights, uint32_t *out_handle);
int vibeos_proc_bind_thread_handle(vibeos_process_table_t *pt, uint32_t owner_pid, uint32_t target_tid, uint32_t rights, uint32_t *out_handle);
int vibeos_proc_resolve_object_handle(vibeos_process_table_t *pt, uint32_t owner_pid, uint32_t handle, vibeos_object_type_t *out_object_type, uint32_t *out_object_id);
int vibeos_thread_state(vibeos_process_table_t *pt, uint32_t tid, vibeos_thread_state_t *out_state);
int vibeos_thread_set_state(vibeos_process_table_t *pt, uint32_t tid, vibeos_thread_state_t state);
int vibeos_thread_exit(vibeos_process_table_t *pt, uint32_t tid);
int vibeos_proc_audit_count(vibeos_process_table_t *pt, uint32_t *out_count);
int vibeos_proc_audit_get(vibeos_process_table_t *pt, uint32_t index, vibeos_proc_audit_event_t *out_event);
int vibeos_proc_audit_count_for_pid(vibeos_process_table_t *pt, uint32_t caller_pid, uint32_t *out_count);
int vibeos_proc_audit_get_for_pid(vibeos_process_table_t *pt, uint32_t caller_pid, uint32_t index, vibeos_proc_audit_event_t *out_event);
int vibeos_proc_audit_set_policy(vibeos_process_table_t *pt, vibeos_proc_audit_policy_t policy);
int vibeos_proc_audit_get_policy(vibeos_process_table_t *pt, vibeos_proc_audit_policy_t *out_policy);
int vibeos_proc_audit_get_dropped(vibeos_process_table_t *pt, uint32_t *out_dropped);
int vibeos_proc_process_count(vibeos_process_table_t *pt, uint32_t *out_count);
int vibeos_proc_thread_count(vibeos_process_table_t *pt, uint32_t *out_count);
int vibeos_proc_live_count(vibeos_process_table_t *pt, uint32_t *out_count);
int vibeos_proc_terminated_count(vibeos_process_table_t *pt, uint32_t *out_count);
int vibeos_proc_count_in_state(vibeos_process_table_t *pt, vibeos_process_state_t state, uint32_t *out_count);
int vibeos_thread_count_in_state(vibeos_process_table_t *pt, vibeos_thread_state_t state, uint32_t *out_count);
int vibeos_proc_state_summary(vibeos_process_table_t *pt, uint32_t *out_new, uint32_t *out_running, uint32_t *out_blocked, uint32_t *out_terminated);
int vibeos_thread_state_summary(vibeos_process_table_t *pt, uint32_t *out_new, uint32_t *out_runnable, uint32_t *out_blocked, uint32_t *out_exited);
int vibeos_proc_transition_counters(vibeos_process_table_t *pt, uint64_t *out_proc_state_transitions, uint64_t *out_thread_state_transitions, uint64_t *out_proc_terminations, uint64_t *out_thread_exits);
int vibeos_proc_transition_counters_reset(vibeos_process_table_t *pt);

#endif
