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

static int process_default_token(uint32_t pid, vibeos_security_token_t *out_token) {
    if (!out_token) {
        return -1;
    }
    return vibeos_sec_token_init(out_token, pid, pid, 0);
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

static int security_override_allowed(uint32_t capability_mask, uint32_t override_capability_bit) {
    if (override_capability_bit >= 32u) {
        return 0;
    }
    return (capability_mask & (1u << override_capability_bit)) ? 1 : 0;
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
        entry->handles.entries[i].object_type = VIBEOS_OBJECT_NONE;
        entry->handles.entries[i].object_id = 0;
        entry->handles.entries[i].origin_pid = 0;
        entry->handles.entries[i].origin_handle = 0;
    }
}

static int revoke_lineage_in_table(vibeos_handle_table_t *table, uint32_t table_pid, uint32_t root_pid, uint32_t root_handle, vibeos_object_type_t object_type_filter, uint32_t rights_mask_filter) {
    uint32_t revoked = 0;
    if (!table || root_pid == 0 || root_handle == 0) {
        return 0;
    }
    if (vibeos_handle_revoke_origin(table, table_pid, root_pid, root_handle, object_type_filter, rights_mask_filter, &revoked) != 0) {
        return 0;
    }
    return (int)revoked;
}

static void proc_audit_record(vibeos_process_table_t *pt,
    vibeos_proc_audit_action_t action,
    uint32_t owner_pid,
    uint32_t request_handle,
    uint32_t root_pid,
    uint32_t root_handle,
    vibeos_object_type_t object_type_filter,
    uint32_t rights_mask_filter,
    uint32_t revoked_count,
    uint32_t success) {
    vibeos_proc_audit_event_t *ev;
    if (!pt) {
        return;
    }
    if (pt->audit_count >= VIBEOS_PROC_AUDIT_CAPACITY && pt->audit_policy == VIBEOS_PROC_AUDIT_DROP_NEWEST) {
        pt->audit_dropped++;
        return;
    }
    ev = &pt->audit_events[pt->audit_head];
    pt->audit_head = (pt->audit_head + 1u) % VIBEOS_PROC_AUDIT_CAPACITY;
    if (pt->audit_count < VIBEOS_PROC_AUDIT_CAPACITY) {
        pt->audit_count++;
    }
    pt->audit_seq++;
    ev->seq = pt->audit_seq;
    ev->action = (uint32_t)action;
    ev->owner_pid = owner_pid;
    ev->request_handle = request_handle;
    ev->root_pid = root_pid;
    ev->root_handle = root_handle;
    ev->object_type_filter = (uint32_t)object_type_filter;
    ev->rights_mask_filter = rights_mask_filter;
    ev->revoked_count = revoked_count;
    ev->success = success;
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
    pt->audit_seq = 0;
    pt->audit_head = 0;
    pt->audit_count = 0;
    pt->audit_dropped = 0;
    pt->proc_state_transitions = 0;
    pt->thread_state_transitions = 0;
    pt->proc_terminations = 0;
    pt->thread_exits = 0;
    pt->audit_policy = VIBEOS_PROC_AUDIT_OVERWRITE_OLDEST;
    for (i = 0; i < VIBEOS_MAX_PROCESSES; i++) {
        pt->entries[i].pid = 0;
        pt->entries[i].parent_pid = 0;
        pt->entries[i].in_use = 0;
        pt->entries[i].state = VIBEOS_PROCESS_STATE_TERMINATED;
        pt->entries[i].security_label = 0;
        pt->entries[i].token.uid = 0;
        pt->entries[i].token.gid = 0;
        pt->entries[i].token.capability_mask = 0;
    }
    for (i = 0; i < VIBEOS_PROC_MAX_THREADS; i++) {
        pt->threads[i].tid = 0;
        pt->threads[i].owner_pid = 0;
        pt->threads[i].in_use = 0;
        pt->threads[i].state = VIBEOS_THREAD_STATE_EXITED;
        pt->threads[i].token_snapshot.uid = 0;
        pt->threads[i].token_snapshot.gid = 0;
        pt->threads[i].token_snapshot.capability_mask = 0;
    }
    for (i = 0; i < VIBEOS_PROC_AUDIT_CAPACITY; i++) {
        pt->audit_events[i].seq = 0;
        pt->audit_events[i].action = 0;
        pt->audit_events[i].owner_pid = 0;
        pt->audit_events[i].request_handle = 0;
        pt->audit_events[i].root_pid = 0;
        pt->audit_events[i].root_handle = 0;
        pt->audit_events[i].object_type_filter = 0;
        pt->audit_events[i].rights_mask_filter = 0;
        pt->audit_events[i].revoked_count = 0;
        pt->audit_events[i].success = 0;
    }
    return 0;
}

int vibeos_proc_spawn(vibeos_process_table_t *pt, uint32_t parent_pid, uint32_t *out_pid) {
    return vibeos_proc_spawn_with_token(pt, parent_pid, 0, out_pid);
}

int vibeos_proc_spawn_with_token(vibeos_process_table_t *pt, uint32_t parent_pid, const vibeos_security_token_t *token, uint32_t *out_pid) {
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
            pt->entries[i].security_label = 0;
            if (token) {
                pt->entries[i].token = *token;
            } else if (parent_pid != 0) {
                vibeos_process_entry_t *parent = find_process_entry(pt, parent_pid);
                if (parent) {
                    pt->entries[i].token = parent->token;
                    pt->entries[i].security_label = parent->security_label;
                } else if (process_default_token(pt->entries[i].pid, &pt->entries[i].token) != 0) {
                    pt->entries[i].in_use = 0;
                    pt->entries[i].pid = 0;
                    pt->entries[i].parent_pid = 0;
                    pt->entries[i].state = VIBEOS_PROCESS_STATE_TERMINATED;
                    pt->entries[i].security_label = 0;
                    return -1;
                }
            } else if (process_default_token(pt->entries[i].pid, &pt->entries[i].token) != 0) {
                pt->entries[i].in_use = 0;
                pt->entries[i].pid = 0;
                pt->entries[i].parent_pid = 0;
                pt->entries[i].state = VIBEOS_PROCESS_STATE_TERMINATED;
                pt->entries[i].security_label = 0;
                return -1;
            }
            if (vibeos_handle_table_init(&pt->entries[i].handles) != 0) {
                pt->entries[i].in_use = 0;
                pt->entries[i].pid = 0;
                pt->entries[i].parent_pid = 0;
                pt->entries[i].state = VIBEOS_PROCESS_STATE_TERMINATED;
                pt->entries[i].security_label = 0;
                pt->entries[i].token.uid = 0;
                pt->entries[i].token.gid = 0;
                pt->entries[i].token.capability_mask = 0;
                return -1;
            }
            *out_pid = pt->entries[i].pid;
            pt->process_count++;
            return 0;
        }
    }
    return -1;
}

int vibeos_proc_are_related(vibeos_process_table_t *pt, uint32_t pid_a, uint32_t pid_b) {
    vibeos_process_entry_t *a;
    vibeos_process_entry_t *b;
    if (!pt || pid_a == 0 || pid_b == 0) {
        return 0;
    }
    a = find_process_entry(pt, pid_a);
    b = find_process_entry(pt, pid_b);
    if (!a || !b) {
        return 0;
    }
    return are_processes_related(a, b);
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
                    pt->threads[j].token_snapshot = pt->entries[i].token;
                    pt->thread_state_transitions++;
                    if (pt->entries[i].state != VIBEOS_PROCESS_STATE_RUNNING) {
                        pt->proc_state_transitions++;
                        pt->entries[i].state = VIBEOS_PROCESS_STATE_RUNNING;
                    }
                    return 0;
                }
            }
            return -1;
        }
    }
    return -1;
}

int vibeos_thread_owner(vibeos_process_table_t *pt, uint32_t tid, uint32_t *out_owner_pid) {
    vibeos_thread_entry_t *thread;
    if (!pt || !out_owner_pid) {
        return -1;
    }
    thread = find_thread_entry(pt, tid);
    if (!thread) {
        return -1;
    }
    *out_owner_pid = thread->owner_pid;
    return 0;
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
    uint32_t root_pid = 0;
    uint32_t root_handle = 0;
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
    if (vibeos_ipc_transfer_handle(&src_entry->handles, &dst_entry->handles, src_handle, requested_rights, out_dst_handle) != 0) {
        return -1;
    }
    if (vibeos_handle_provenance(&src_entry->handles, src_handle, &root_pid, &root_handle) != 0) {
        return -1;
    }
    if (root_pid == 0 || root_handle == 0) {
        root_pid = src_pid;
        root_handle = src_handle;
    }
    if (vibeos_handle_set_provenance(&dst_entry->handles, *out_dst_handle, root_pid, root_handle) != 0) {
        return -1;
    }
    return 0;
}

int vibeos_proc_revoke_handle_lineage(vibeos_process_table_t *pt, uint32_t owner_pid, uint32_t handle) {
    return vibeos_proc_revoke_handle_lineage_scoped(pt, owner_pid, handle, VIBEOS_OBJECT_NONE, 0);
}

int vibeos_proc_revoke_handle_lineage_scoped(vibeos_process_table_t *pt, uint32_t owner_pid, uint32_t handle, vibeos_object_type_t object_type_filter, uint32_t rights_mask_filter) {
    vibeos_process_entry_t *owner;
    uint32_t root_pid = 0;
    uint32_t root_handle = 0;
    uint32_t i;
    int revoked_total = 0;
    vibeos_proc_audit_action_t action;
    uint32_t success = 0;
    action = (object_type_filter == VIBEOS_OBJECT_NONE && rights_mask_filter == 0) ? VIBEOS_PROC_AUDIT_REVOKE_LINEAGE : VIBEOS_PROC_AUDIT_REVOKE_LINEAGE_SCOPED;
    if (!pt || owner_pid == 0 || handle == 0) {
        return -1;
    }
    owner = find_process_entry(pt, owner_pid);
    if (!owner) {
        return -1;
    }
    if (!vibeos_handle_has_rights(&owner->handles, handle, VIBEOS_HANDLE_RIGHT_MANAGE)) {
        proc_audit_record(pt, action, owner_pid, handle, 0, 0, object_type_filter, rights_mask_filter, 0, 0);
        return -1;
    }
    if (vibeos_handle_provenance(&owner->handles, handle, &root_pid, &root_handle) != 0) {
        proc_audit_record(pt, action, owner_pid, handle, 0, 0, object_type_filter, rights_mask_filter, 0, 0);
        return -1;
    }
    if (root_pid == 0 || root_handle == 0) {
        root_pid = owner_pid;
        root_handle = handle;
    }
    for (i = 0; i < VIBEOS_MAX_PROCESSES; i++) {
        if (pt->entries[i].in_use) {
            revoked_total += revoke_lineage_in_table(&pt->entries[i].handles, pt->entries[i].pid, root_pid, root_handle, object_type_filter, rights_mask_filter);
        }
    }
    success = (revoked_total > 0) ? 1u : 0u;
    proc_audit_record(pt, action, owner_pid, handle, root_pid, root_handle, object_type_filter, rights_mask_filter, (uint32_t)revoked_total, success);
    return (revoked_total > 0) ? 0 : -1;
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
    if (state > VIBEOS_PROCESS_STATE_TERMINATED) {
        return -1;
    }
    if (entry->state != state) {
        pt->proc_state_transitions++;
        entry->state = state;
    }
    return 0;
}

int vibeos_proc_terminate(vibeos_process_table_t *pt, uint32_t pid) {
    vibeos_process_entry_t *entry = find_process_entry(pt, pid);
    uint32_t i;
    if (!entry) {
        return -1;
    }
    if (entry->state != VIBEOS_PROCESS_STATE_TERMINATED) {
        pt->proc_state_transitions++;
        pt->proc_terminations++;
    }
    release_all_process_handles(entry);
    for (i = 0; i < VIBEOS_PROC_MAX_THREADS; i++) {
        if (pt->threads[i].in_use && pt->threads[i].owner_pid == pid) {
            if (pt->threads[i].state != VIBEOS_THREAD_STATE_EXITED) {
                pt->thread_state_transitions++;
            }
            pt->threads[i].in_use = 0;
            pt->threads[i].state = VIBEOS_THREAD_STATE_EXITED;
            pt->threads[i].owner_pid = 0;
            pt->threads[i].tid = 0;
            pt->threads[i].token_snapshot.uid = 0;
            pt->threads[i].token_snapshot.gid = 0;
            pt->threads[i].token_snapshot.capability_mask = 0;
            pt->thread_exits++;
            if (pt->thread_count > 0) {
                pt->thread_count--;
            }
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

int vibeos_thread_state(vibeos_process_table_t *pt, uint32_t tid, vibeos_thread_state_t *out_state) {
    vibeos_thread_entry_t *thread;
    if (!pt || !out_state) {
        return -1;
    }
    thread = find_thread_entry(pt, tid);
    if (!thread) {
        return -1;
    }
    *out_state = thread->state;
    return 0;
}

int vibeos_thread_set_state(vibeos_process_table_t *pt, uint32_t tid, vibeos_thread_state_t state) {
    vibeos_thread_entry_t *thread;
    if (!pt) {
        return -1;
    }
    thread = find_thread_entry(pt, tid);
    if (!thread) {
        return -1;
    }
    if (thread->state != state) {
        pt->thread_state_transitions++;
        thread->state = state;
    }
    return 0;
}

int vibeos_thread_exit(vibeos_process_table_t *pt, uint32_t tid) {
    vibeos_thread_entry_t *thread;
    if (!pt) {
        return -1;
    }
    thread = find_thread_entry(pt, tid);
    if (!thread) {
        return -1;
    }
    thread->in_use = 0;
    if (thread->state != VIBEOS_THREAD_STATE_EXITED) {
        pt->thread_state_transitions++;
    }
    thread->state = VIBEOS_THREAD_STATE_EXITED;
    thread->owner_pid = 0;
    thread->tid = 0;
    thread->token_snapshot.uid = 0;
    thread->token_snapshot.gid = 0;
    thread->token_snapshot.capability_mask = 0;
    pt->thread_exits++;
    if (pt->thread_count > 0) {
        pt->thread_count--;
    }
    return 0;
}

int vibeos_proc_audit_count(vibeos_process_table_t *pt, uint32_t *out_count) {
    if (!pt || !out_count) {
        return -1;
    }
    *out_count = pt->audit_count;
    return 0;
}

int vibeos_proc_audit_get(vibeos_process_table_t *pt, uint32_t index, vibeos_proc_audit_event_t *out_event) {
    uint32_t start;
    uint32_t slot;
    if (!pt || !out_event || index >= pt->audit_count) {
        return -1;
    }
    start = (pt->audit_head + VIBEOS_PROC_AUDIT_CAPACITY - pt->audit_count) % VIBEOS_PROC_AUDIT_CAPACITY;
    slot = (start + index) % VIBEOS_PROC_AUDIT_CAPACITY;
    *out_event = pt->audit_events[slot];
    return 0;
}

int vibeos_proc_audit_count_for_pid(vibeos_process_table_t *pt, uint32_t caller_pid, uint32_t *out_count) {
    uint32_t i;
    uint32_t visible = 0;
    vibeos_proc_audit_event_t ev;
    if (!pt || !out_count || caller_pid == 0) {
        return -1;
    }
    for (i = 0; i < pt->audit_count; i++) {
        if (vibeos_proc_audit_get(pt, i, &ev) != 0) {
            return -1;
        }
        if (ev.owner_pid == caller_pid) {
            visible++;
        }
    }
    *out_count = visible;
    return 0;
}

int vibeos_proc_audit_get_for_pid(vibeos_process_table_t *pt, uint32_t caller_pid, uint32_t index, vibeos_proc_audit_event_t *out_event) {
    uint32_t i;
    uint32_t visible_index = 0;
    vibeos_proc_audit_event_t ev;
    if (!pt || !out_event || caller_pid == 0) {
        return -1;
    }
    for (i = 0; i < pt->audit_count; i++) {
        if (vibeos_proc_audit_get(pt, i, &ev) != 0) {
            return -1;
        }
        if (ev.owner_pid != caller_pid) {
            continue;
        }
        if (visible_index == index) {
            *out_event = ev;
            return 0;
        }
        visible_index++;
    }
    return -1;
}

int vibeos_proc_audit_count_action(vibeos_process_table_t *pt, uint32_t action, uint32_t *out_count) {
    uint32_t i;
    uint32_t count = 0;
    vibeos_proc_audit_event_t ev;
    if (!pt || !out_count || action == 0) {
        return -1;
    }
    for (i = 0; i < pt->audit_count; i++) {
        if (vibeos_proc_audit_get(pt, i, &ev) != 0) {
            return -1;
        }
        if (ev.action == action) {
            count++;
        }
    }
    *out_count = count;
    return 0;
}

int vibeos_proc_audit_count_success(vibeos_process_table_t *pt, uint32_t success_value, uint32_t *out_count) {
    uint32_t i;
    uint32_t count = 0;
    vibeos_proc_audit_event_t ev;
    if (!pt || !out_count || success_value > 1) {
        return -1;
    }
    for (i = 0; i < pt->audit_count; i++) {
        if (vibeos_proc_audit_get(pt, i, &ev) != 0) {
            return -1;
        }
        if (ev.success == success_value) {
            count++;
        }
    }
    *out_count = count;
    return 0;
}

int vibeos_proc_audit_summary(vibeos_process_table_t *pt, uint32_t *out_total, uint32_t *out_success, uint32_t *out_fail) {
    uint32_t success_count = 0;
    uint32_t fail_count = 0;
    if (!pt || !out_total || !out_success || !out_fail) {
        return -1;
    }
    if (vibeos_proc_audit_count_success(pt, 1, &success_count) != 0) {
        return -1;
    }
    if (vibeos_proc_audit_count_success(pt, 0, &fail_count) != 0) {
        return -1;
    }
    *out_total = pt->audit_count;
    *out_success = success_count;
    *out_fail = fail_count;
    return 0;
}

int vibeos_proc_audit_set_policy(vibeos_process_table_t *pt, vibeos_proc_audit_policy_t policy) {
    if (!pt) {
        return -1;
    }
    if (policy != VIBEOS_PROC_AUDIT_OVERWRITE_OLDEST && policy != VIBEOS_PROC_AUDIT_DROP_NEWEST) {
        return -1;
    }
    pt->audit_policy = policy;
    return 0;
}

int vibeos_proc_audit_get_policy(vibeos_process_table_t *pt, vibeos_proc_audit_policy_t *out_policy) {
    if (!pt || !out_policy) {
        return -1;
    }
    *out_policy = pt->audit_policy;
    return 0;
}

int vibeos_proc_audit_get_dropped(vibeos_process_table_t *pt, uint32_t *out_dropped) {
    if (!pt || !out_dropped) {
        return -1;
    }
    *out_dropped = pt->audit_dropped;
    return 0;
}

int vibeos_proc_process_count(vibeos_process_table_t *pt, uint32_t *out_count) {
    if (!pt || !out_count) {
        return -1;
    }
    *out_count = pt->process_count;
    return 0;
}

int vibeos_proc_thread_count(vibeos_process_table_t *pt, uint32_t *out_count) {
    if (!pt || !out_count) {
        return -1;
    }
    *out_count = pt->thread_count;
    return 0;
}

int vibeos_proc_live_count(vibeos_process_table_t *pt, uint32_t *out_count) {
    uint32_t i;
    uint32_t live = 0;
    if (!pt || !out_count) {
        return -1;
    }
    for (i = 0; i < VIBEOS_MAX_PROCESSES; i++) {
        if (pt->entries[i].in_use && pt->entries[i].state != VIBEOS_PROCESS_STATE_TERMINATED) {
            live++;
        }
    }
    *out_count = live;
    return 0;
}

int vibeos_proc_terminated_count(vibeos_process_table_t *pt, uint32_t *out_count) {
    uint32_t i;
    uint32_t terminated = 0;
    if (!pt || !out_count) {
        return -1;
    }
    for (i = 0; i < VIBEOS_MAX_PROCESSES; i++) {
        if (pt->entries[i].in_use && pt->entries[i].state == VIBEOS_PROCESS_STATE_TERMINATED) {
            terminated++;
        }
    }
    *out_count = terminated;
    return 0;
}

int vibeos_proc_count_in_state(vibeos_process_table_t *pt, vibeos_process_state_t state, uint32_t *out_count) {
    uint32_t i;
    uint32_t count = 0;
    if (!pt || !out_count || state > VIBEOS_PROCESS_STATE_TERMINATED) {
        return -1;
    }
    for (i = 0; i < VIBEOS_MAX_PROCESSES; i++) {
        if (pt->entries[i].in_use && pt->entries[i].state == state) {
            count++;
        }
    }
    *out_count = count;
    return 0;
}

int vibeos_thread_count_in_state(vibeos_process_table_t *pt, vibeos_thread_state_t state, uint32_t *out_count) {
    uint32_t i;
    uint32_t count = 0;
    if (!pt || !out_count || state > VIBEOS_THREAD_STATE_EXITED) {
        return -1;
    }
    for (i = 0; i < VIBEOS_PROC_MAX_THREADS; i++) {
        if (pt->threads[i].in_use && pt->threads[i].state == state) {
            count++;
        }
    }
    *out_count = count;
    return 0;
}

int vibeos_proc_state_summary(vibeos_process_table_t *pt, uint32_t *out_new, uint32_t *out_running, uint32_t *out_blocked, uint32_t *out_terminated) {
    uint32_t i;
    if (!pt || !out_new || !out_running || !out_blocked || !out_terminated) {
        return -1;
    }
    *out_new = 0;
    *out_running = 0;
    *out_blocked = 0;
    *out_terminated = 0;
    for (i = 0; i < VIBEOS_MAX_PROCESSES; i++) {
        if (!pt->entries[i].in_use) {
            continue;
        }
        switch (pt->entries[i].state) {
            case VIBEOS_PROCESS_STATE_NEW:
                (*out_new)++;
                break;
            case VIBEOS_PROCESS_STATE_RUNNING:
                (*out_running)++;
                break;
            case VIBEOS_PROCESS_STATE_BLOCKED:
                (*out_blocked)++;
                break;
            case VIBEOS_PROCESS_STATE_TERMINATED:
                (*out_terminated)++;
                break;
            default:
                return -1;
        }
    }
    return 0;
}

int vibeos_thread_state_summary(vibeos_process_table_t *pt, uint32_t *out_new, uint32_t *out_runnable, uint32_t *out_blocked, uint32_t *out_exited) {
    uint32_t i;
    if (!pt || !out_new || !out_runnable || !out_blocked || !out_exited) {
        return -1;
    }
    *out_new = 0;
    *out_runnable = 0;
    *out_blocked = 0;
    *out_exited = 0;
    for (i = 0; i < VIBEOS_PROC_MAX_THREADS; i++) {
        if (!pt->threads[i].in_use) {
            continue;
        }
        switch (pt->threads[i].state) {
            case VIBEOS_THREAD_STATE_NEW:
                (*out_new)++;
                break;
            case VIBEOS_THREAD_STATE_RUNNABLE:
                (*out_runnable)++;
                break;
            case VIBEOS_THREAD_STATE_BLOCKED:
                (*out_blocked)++;
                break;
            case VIBEOS_THREAD_STATE_EXITED:
                (*out_exited)++;
                break;
            default:
                return -1;
        }
    }
    return 0;
}

int vibeos_proc_transition_counters(vibeos_process_table_t *pt, uint64_t *out_proc_state_transitions, uint64_t *out_thread_state_transitions, uint64_t *out_proc_terminations, uint64_t *out_thread_exits) {
    if (!pt || !out_proc_state_transitions || !out_thread_state_transitions || !out_proc_terminations || !out_thread_exits) {
        return -1;
    }
    *out_proc_state_transitions = pt->proc_state_transitions;
    *out_thread_state_transitions = pt->thread_state_transitions;
    *out_proc_terminations = pt->proc_terminations;
    *out_thread_exits = pt->thread_exits;
    return 0;
}

int vibeos_proc_transition_counters_reset(vibeos_process_table_t *pt) {
    if (!pt) {
        return -1;
    }
    pt->proc_state_transitions = 0;
    pt->thread_state_transitions = 0;
    pt->proc_terminations = 0;
    pt->thread_exits = 0;
    return 0;
}

int vibeos_proc_token_get(vibeos_process_table_t *pt, uint32_t pid, vibeos_security_token_t *out_token) {
    vibeos_process_entry_t *entry;
    if (!pt || !out_token || pid == 0) {
        return -1;
    }
    entry = find_process_entry(pt, pid);
    if (!entry) {
        return -1;
    }
    *out_token = entry->token;
    return 0;
}

int vibeos_proc_token_set(vibeos_process_table_t *pt, uint32_t pid, const vibeos_security_token_t *token) {
    vibeos_process_entry_t *entry;
    uint32_t i;
    if (!pt || !token || pid == 0) {
        return -1;
    }
    entry = find_process_entry(pt, pid);
    if (!entry) {
        return -1;
    }
    entry->token = *token;
    for (i = 0; i < VIBEOS_PROC_MAX_THREADS; i++) {
        if (pt->threads[i].in_use && pt->threads[i].owner_pid == pid) {
            pt->threads[i].token_snapshot = *token;
        }
    }
    return 0;
}

int vibeos_thread_token_get(vibeos_process_table_t *pt, uint32_t tid, vibeos_security_token_t *out_token) {
    vibeos_thread_entry_t *thread;
    if (!pt || !out_token || tid == 0) {
        return -1;
    }
    thread = find_thread_entry(pt, tid);
    if (!thread) {
        return -1;
    }
    *out_token = thread->token_snapshot;
    return 0;
}

int vibeos_proc_security_label_get(vibeos_process_table_t *pt, uint32_t pid, uint32_t *out_label) {
    vibeos_process_entry_t *entry;
    if (!pt || !out_label || pid == 0) {
        return -1;
    }
    entry = find_process_entry(pt, pid);
    if (!entry) {
        return -1;
    }
    *out_label = entry->security_label;
    return 0;
}

int vibeos_proc_security_label_set(vibeos_process_table_t *pt, uint32_t pid, uint32_t label) {
    vibeos_process_entry_t *entry;
    if (!pt || pid == 0) {
        return -1;
    }
    entry = find_process_entry(pt, pid);
    if (!entry) {
        return -1;
    }
    entry->security_label = label;
    return 0;
}

int vibeos_proc_security_can_interact(vibeos_process_table_t *pt, uint32_t caller_pid, uint32_t target_pid, uint32_t caller_capability_mask, uint32_t override_capability_bit) {
    vibeos_process_entry_t *caller;
    vibeos_process_entry_t *target;
    if (!pt || caller_pid == 0 || target_pid == 0) {
        return -1;
    }
    caller = find_process_entry(pt, caller_pid);
    target = find_process_entry(pt, target_pid);
    if (!caller || !target) {
        return -1;
    }
    if (caller->security_label == target->security_label) {
        return 1;
    }
    if (security_override_allowed(caller_capability_mask, override_capability_bit)) {
        return 1;
    }
    return 0;
}
