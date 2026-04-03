#ifndef VIBEOS_PROC_H
#define VIBEOS_PROC_H

#include <stdint.h>

typedef struct vibeos_process_table {
    uint32_t next_pid;
    uint32_t next_tid;
    uint32_t process_count;
    uint32_t thread_count;
} vibeos_process_table_t;

int vibeos_proc_init(vibeos_process_table_t *pt);
int vibeos_proc_spawn(vibeos_process_table_t *pt, uint32_t parent_pid, uint32_t *out_pid);
int vibeos_thread_create(vibeos_process_table_t *pt, uint32_t pid, uint32_t *out_tid);

#endif
