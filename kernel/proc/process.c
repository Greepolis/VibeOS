#include "vibeos/proc.h"

int vibeos_proc_init(vibeos_process_table_t *pt) {
    uint32_t i;
    if (!pt) {
        return -1;
    }
    pt->next_pid = 1;
    pt->next_tid = 1;
    pt->process_count = 0;
    pt->thread_count = 0;
    for (i = 0; i < VIBEOS_MAX_PROCESSES; i++) {
        pt->entries[i].pid = 0;
        pt->entries[i].parent_pid = 0;
        pt->entries[i].in_use = 0;
    }
    return 0;
}

int vibeos_proc_spawn(vibeos_process_table_t *pt, uint32_t parent_pid, uint32_t *out_pid) {
    uint32_t i;
    if (!pt || !out_pid) {
        return -1;
    }
    for (i = 0; i < VIBEOS_MAX_PROCESSES; i++) {
        if (!pt->entries[i].in_use) {
            pt->entries[i].pid = pt->next_pid++;
            pt->entries[i].parent_pid = parent_pid;
            pt->entries[i].in_use = 1;
            if (vibeos_handle_table_init(&pt->entries[i].handles) != 0) {
                pt->entries[i].in_use = 0;
                pt->entries[i].pid = 0;
                pt->entries[i].parent_pid = 0;
                return -1;
            }
            *out_pid = pt->entries[i].pid;
            pt->process_count++;
            return 0;
        }
    }
    return -1;
}

int vibeos_thread_create(vibeos_process_table_t *pt, uint32_t pid, uint32_t *out_tid) {
    uint32_t i;
    if (!pt || !out_tid) {
        return -1;
    }
    for (i = 0; i < VIBEOS_MAX_PROCESSES; i++) {
        if (pt->entries[i].in_use && pt->entries[i].pid == pid) {
            *out_tid = pt->next_tid++;
            pt->thread_count++;
            return 0;
        }
    }
    return -1;
}

int vibeos_proc_handles(vibeos_process_table_t *pt, uint32_t pid, vibeos_handle_table_t **out_handles) {
    uint32_t i;
    if (!pt || !out_handles || pid == 0) {
        return -1;
    }
    for (i = 0; i < VIBEOS_MAX_PROCESSES; i++) {
        if (pt->entries[i].in_use && pt->entries[i].pid == pid) {
            *out_handles = &pt->entries[i].handles;
            return 0;
        }
    }
    return -1;
}
