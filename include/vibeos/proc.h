#ifndef VIBEOS_PROC_H
#define VIBEOS_PROC_H

#include <stdint.h>

#include "vibeos/object.h"

#define VIBEOS_MAX_PROCESSES 32u
#define VIBEOS_PROC_MAX_THREADS 256u

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

typedef struct vibeos_process_table {
    uint32_t next_pid;
    uint32_t next_tid;
    uint32_t process_count;
    uint32_t thread_count;
    vibeos_process_entry_t entries[VIBEOS_MAX_PROCESSES];
    vibeos_thread_entry_t threads[VIBEOS_PROC_MAX_THREADS];
} vibeos_process_table_t;

int vibeos_proc_init(vibeos_process_table_t *pt);
int vibeos_proc_spawn(vibeos_process_table_t *pt, uint32_t parent_pid, uint32_t *out_pid);
int vibeos_thread_create(vibeos_process_table_t *pt, uint32_t pid, uint32_t *out_tid);
int vibeos_proc_handles(vibeos_process_table_t *pt, uint32_t pid, vibeos_handle_table_t **out_handles);
int vibeos_proc_duplicate_handle(vibeos_process_table_t *pt, uint32_t src_pid, uint32_t dst_pid, uint32_t src_handle, uint32_t requested_rights, uint32_t *out_dst_handle);
int vibeos_proc_revoke_handle_lineage(vibeos_process_table_t *pt, uint32_t owner_pid, uint32_t handle);
int vibeos_proc_state(vibeos_process_table_t *pt, uint32_t pid, vibeos_process_state_t *out_state);
int vibeos_proc_set_state(vibeos_process_table_t *pt, uint32_t pid, vibeos_process_state_t state);
int vibeos_proc_terminate(vibeos_process_table_t *pt, uint32_t pid);
int vibeos_proc_bind_process_handle(vibeos_process_table_t *pt, uint32_t owner_pid, uint32_t target_pid, uint32_t rights, uint32_t *out_handle);
int vibeos_proc_bind_thread_handle(vibeos_process_table_t *pt, uint32_t owner_pid, uint32_t target_tid, uint32_t rights, uint32_t *out_handle);
int vibeos_proc_resolve_object_handle(vibeos_process_table_t *pt, uint32_t owner_pid, uint32_t handle, vibeos_object_type_t *out_object_type, uint32_t *out_object_id);

#endif
