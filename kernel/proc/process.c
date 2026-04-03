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

static vibeos_thread_entry_t *find_thread_entry(vibeos_process_table_t *pt, uint32_t tid) {
    uint32_t i;
    if (!pt || tid == 0) {
        return 0;
    }
    for (i = 0; i < VIBEOS_PROC_MAX_THREADS; i++) {
        if (pt->threads[i].in_use && pt->threads[i].tid == tid) {
            return &pt->threads[i];
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

static void release_all_process_handles(vibeos_process_entry_t *entry) {
    uint32_t i;
    if (!entry) {
        return;
    }
    for (i = 0; i < VIBEOS_MAX_HANDLES; i++) {
        entry->handles.entries[i].in_use = 0;
        entry->handles.entries[i].id = 0;
        entry->handles.entries[i].rights = 0;
    }
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
        pt->entries[i].state = VIBEOS_PROCESS_STATE_TERMINATED;
    }
    for (i = 0; i < VIBEOS_PROC_MAX_THREADS; i++) {
        pt->threads[i].tid = 0;
        pt->threads[i].owner_pid = 0;
        pt->threads[i].in_use = 0;
        pt->threads[i].state = VIBEOS_THREAD_STATE_EXITED;
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
            pt->entries[i].state = VIBEOS_PROCESS_STATE_NEW;
            if (vibeos_handle_table_init(&pt->entries[i].handles) != 0) {
                pt->entries[i].in_use = 0;
                pt->entries[i].pid = 0;
                pt->entries[i].parent_pid = 0;
                pt->entries[i].state = VIBEOS_PROCESS_STATE_TERMINATED;
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
            uint32_t j;
            for (j = 0; j < VIBEOS_PROC_MAX_THREADS; j++) {
                if (!pt->threads[j].in_use) {
                    *out_tid = pt->next_tid++;
                    pt->thread_count++;
                    pt->threads[j].tid = *out_tid;
                    pt->threads[j].owner_pid = pid;
                    pt->threads[j].in_use = 1;
                    pt->threads[j].state = VIBEOS_THREAD_STATE_RUNNABLE;
                    pt->entries[i].state = VIBEOS_PROCESS_STATE_RUNNING;
                    return 0;
                }
            }
            return -1;
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

int vibeos_proc_state(vibeos_process_table_t *pt, uint32_t pid, vibeos_process_state_t *out_state) {
    vibeos_process_entry_t *entry;
    if (!out_state) {
        return -1;
    }
    entry = find_process_entry(pt, pid);
    if (!entry) {
        return -1;
    }
    *out_state = entry->state;
    return 0;
}

int vibeos_proc_set_state(vibeos_process_table_t *pt, uint32_t pid, vibeos_process_state_t state) {
    vibeos_process_entry_t *entry = find_process_entry(pt, pid);
    if (!entry) {
        return -1;
    }
    entry->state = state;
    return 0;
}

int vibeos_proc_terminate(vibeos_process_table_t *pt, uint32_t pid) {
    vibeos_process_entry_t *entry = find_process_entry(pt, pid);
    uint32_t i;
    if (!entry) {
        return -1;
    }
    release_all_process_handles(entry);
    for (i = 0; i < VIBEOS_PROC_MAX_THREADS; i++) {
        if (pt->threads[i].in_use && pt->threads[i].owner_pid == pid) {
            pt->threads[i].in_use = 0;
            pt->threads[i].state = VIBEOS_THREAD_STATE_EXITED;
            pt->threads[i].owner_pid = 0;
            pt->threads[i].tid = 0;
        }
    }
    entry->state = VIBEOS_PROCESS_STATE_TERMINATED;
    return 0;
}

int vibeos_proc_bind_process_handle(vibeos_process_table_t *pt, uint32_t owner_pid, uint32_t target_pid, uint32_t rights, uint32_t *out_handle) {
    vibeos_process_entry_t *owner;
    vibeos_process_entry_t *target;
    if (!pt || !out_handle || rights == 0) {
        return -1;
    }
    owner = find_process_entry(pt, owner_pid);
    target = find_process_entry(pt, target_pid);
    if (!owner || !target) {
        return -1;
    }
    if (!are_processes_related(owner, target)) {
        return -1;
    }
    return vibeos_handle_alloc_object(&owner->handles, rights, VIBEOS_OBJECT_PROCESS, target_pid, out_handle);
}

int vibeos_proc_bind_thread_handle(vibeos_process_table_t *pt, uint32_t owner_pid, uint32_t target_tid, uint32_t rights, uint32_t *out_handle) {
    vibeos_process_entry_t *owner;
    vibeos_thread_entry_t *target_thread;
    vibeos_process_entry_t *target_owner;
    if (!pt || !out_handle || rights == 0) {
        return -1;
    }
    owner = find_process_entry(pt, owner_pid);
    target_thread = find_thread_entry(pt, target_tid);
    if (!owner || !target_thread) {
        return -1;
    }
    target_owner = find_process_entry(pt, target_thread->owner_pid);
    if (!target_owner || !are_processes_related(owner, target_owner)) {
        return -1;
    }
    return vibeos_handle_alloc_object(&owner->handles, rights, VIBEOS_OBJECT_THREAD, target_tid, out_handle);
}

int vibeos_proc_resolve_object_handle(vibeos_process_table_t *pt, uint32_t owner_pid, uint32_t handle, vibeos_object_type_t *out_object_type, uint32_t *out_object_id) {
    vibeos_process_entry_t *owner;
    if (!pt || !out_object_type || !out_object_id) {
        return -1;
    }
    owner = find_process_entry(pt, owner_pid);
    if (!owner) {
        return -1;
    }
    return vibeos_handle_object(&owner->handles, handle, out_object_type, out_object_id);
}
