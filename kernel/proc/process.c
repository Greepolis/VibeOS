#include "vibeos/proc.h"
#include "vibeos/ipc_transfer.h"

static vibeos_process_entry_t *find_process_entry(vibeos_process_table_t *pt, uint32_t pid) {
    uint32_t i;
    if (!pt || pid == 0) {
        return 0;
    }
    for (i = 0; i < VIBEOS_MAX_PROCESSES; i++) {
        if (pt->entries[i].in_use && pt->entries[i].pid == pid) {
            return &pt->entries[i];
        }
    }
    return 0;
}

static int are_processes_related(const vibeos_process_entry_t *a, const vibeos_process_entry_t *b) {
    if (!a || !b) {
        return 0;
    }
    if (a->pid == b->pid) {
        return 1;
    }
    if (a->parent_pid == b->pid || b->parent_pid == a->pid) {
        return 1;
    }
    return 0;
}

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
    vibeos_process_entry_t *entry;
    if (!out_handles) {
        return -1;
    }
    entry = find_process_entry(pt, pid);
    if (!entry) {
        return -1;
    }
    *out_handles = &entry->handles;
    return 0;
}

int vibeos_proc_duplicate_handle(vibeos_process_table_t *pt, uint32_t src_pid, uint32_t dst_pid, uint32_t src_handle, uint32_t requested_rights, uint32_t *out_dst_handle) {
    vibeos_process_entry_t *src_entry;
    vibeos_process_entry_t *dst_entry;
    if (!pt || !out_dst_handle || src_handle == 0 || requested_rights == 0) {
        return -1;
    }
    src_entry = find_process_entry(pt, src_pid);
    dst_entry = find_process_entry(pt, dst_pid);
    if (!src_entry || !dst_entry) {
        return -1;
    }
    if (!are_processes_related(src_entry, dst_entry)) {
        return -1;
    }
    if (!vibeos_handle_has_rights(&src_entry->handles, src_handle, VIBEOS_HANDLE_RIGHT_MANAGE)) {
        return -1;
    }
    return vibeos_ipc_transfer_handle(&src_entry->handles, &dst_entry->handles, src_handle, requested_rights, out_dst_handle);
}
