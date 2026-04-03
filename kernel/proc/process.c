#include "vibeos/proc.h"

int vibeos_proc_init(vibeos_process_table_t *pt) {
    if (!pt) {
        return -1;
    }
    pt->next_pid = 1;
    pt->next_tid = 1;
    pt->process_count = 0;
    pt->thread_count = 0;
    return 0;
}

int vibeos_proc_spawn(vibeos_process_table_t *pt, uint32_t parent_pid, uint32_t *out_pid) {
    (void)parent_pid;
    if (!pt || !out_pid) {
        return -1;
    }
    *out_pid = pt->next_pid++;
    pt->process_count++;
    return 0;
}

int vibeos_thread_create(vibeos_process_table_t *pt, uint32_t pid, uint32_t *out_tid) {
    (void)pid;
    if (!pt || !out_tid) {
        return -1;
    }
    *out_tid = pt->next_tid++;
    pt->thread_count++;
    return 0;
}
