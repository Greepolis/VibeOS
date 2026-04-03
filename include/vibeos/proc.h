#ifndef VIBEOS_PROC_H
#define VIBEOS_PROC_H

#include <stdint.h>

#include "vibeos/object.h"

#define VIBEOS_MAX_PROCESSES 32u

typedef struct vibeos_process_entry {
    uint32_t pid;
    uint32_t parent_pid;
    uint32_t in_use;
    vibeos_handle_table_t handles;
} vibeos_process_entry_t;

typedef struct vibeos_process_table {
    uint32_t next_pid;
    uint32_t next_tid;
    uint32_t process_count;
    uint32_t thread_count;
    vibeos_process_entry_t entries[VIBEOS_MAX_PROCESSES];
} vibeos_process_table_t;

int vibeos_proc_init(vibeos_process_table_t *pt);
int vibeos_proc_spawn(vibeos_process_table_t *pt, uint32_t parent_pid, uint32_t *out_pid);
int vibeos_thread_create(vibeos_process_table_t *pt, uint32_t pid, uint32_t *out_tid);
int vibeos_proc_handles(vibeos_process_table_t *pt, uint32_t pid, vibeos_handle_table_t **out_handles);

#endif
