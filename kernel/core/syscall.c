#include "vibeos/kernel.h"
#include "vibeos/syscall.h"
#include "vibeos/syscall_abi.h"
#include "vibeos/syscall_policy.h"
#include "vibeos/waitset.h"

static int syscall_thread_access_allowed(vibeos_kernel_t *kernel, uint32_t caller_pid, uint32_t tid) {
    uint32_t owner_pid = 0;
    if (!kernel || tid == 0) {
        return 0;
    }
    if (caller_pid == 0) {
        return 1;
    }
    if (vibeos_thread_owner(&kernel->proc_table, tid, &owner_pid) != 0) {
        return 0;
    }
    return caller_pid == owner_pid;
}

static int syscall_process_view_allowed(vibeos_kernel_t *kernel, uint32_t caller_pid, uint32_t target_pid) {
    if (!kernel || target_pid == 0) {
        return 0;
    }
    if (caller_pid == 0 || caller_pid == target_pid) {
        return 1;
    }
    return vibeos_proc_are_related(&kernel->proc_table, caller_pid, target_pid);
}

static int syscall_process_mutation_allowed(uint32_t caller_pid, uint32_t target_pid) {
    if (target_pid == 0) {
        return 0;
    }
    if (caller_pid == 0) {
        return 1;
    }
    return caller_pid == target_pid;
}

static int syscall_process_token_access_allowed(vibeos_kernel_t *kernel, uint32_t caller_pid, uint32_t target_pid) {
    if (!kernel || target_pid == 0) {
        return 0;
    }
    if (caller_pid == 0 || caller_pid == target_pid) {
        return 1;
    }
    return vibeos_proc_are_related(&kernel->proc_table, caller_pid, target_pid);
}

static int syscall_caller_capability_mask(vibeos_kernel_t *kernel, uint32_t caller_pid, uint32_t *out_capability_mask) {
    vibeos_security_token_t token;
    if (!kernel || !out_capability_mask) {
        return -1;
    }
    if (caller_pid == 0) {
        *out_capability_mask = kernel->kernel_token.capability_mask;
        return 0;
    }
    if (vibeos_proc_token_get(&kernel->proc_table, caller_pid, &token) != 0) {
        return -1;
    }
    *out_capability_mask = token.capability_mask;
    return 0;
}

static int syscall_waitset_owner_access_allowed(uint32_t caller_pid, uint32_t owner_pid, int waitset_initialized) {
    if (!waitset_initialized) {
        return 0;
    }
    if (caller_pid == 0) {
        return 1;
    }
    return caller_pid == owner_pid;
}

static void syscall_sec_audit_record(vibeos_kernel_t *kernel, vibeos_sec_audit_action_t action, uint32_t caller_pid, uint32_t target_pid, uint64_t aux, uint32_t success) {
    if (!kernel || !kernel->sec_audit.initialized) {
        return;
    }
    (void)vibeos_sec_audit_record(&kernel->sec_audit, action, caller_pid, target_pid, aux, success);
}

int64_t vibeos_syscall_dispatch(struct vibeos_kernel *kernel, vibeos_syscall_frame_t *frame) {
    static vibeos_waitset_t kernel_waitset;
    static uint32_t kernel_waitset_owner_pid = 0;
    static int waitset_initialized = 0;

    if (!kernel || !frame) {
        return -1;
    }

    if (!waitset_initialized) {
        if (vibeos_waitset_init(&kernel_waitset) != 0) {
            frame->result = -1;
            return -1;
        }
        waitset_initialized = 1;
    }
    if (!kernel->sec_audit.initialized) {
        if (vibeos_sec_audit_init(&kernel->sec_audit) != 0) {
            frame->result = -1;
            return -1;
        }
    }

    {
        uint32_t caller_pid = vibeos_syscall_caller_pid(frame);
        vibeos_handle_table_t *caller_handles = &kernel->handles;
        vibeos_syscall_policy_t policy = vibeos_syscall_policy_for((vibeos_syscall_id_t)frame->id);
        if (policy.requires_handle) {
            if (caller_pid != 0) {
                if (vibeos_proc_handles(&kernel->proc_table, caller_pid, &caller_handles) != 0) {
                    frame->result = -1;
                    return -1;
                }
            }
            if (!vibeos_handle_has_rights(caller_handles, vibeos_syscall_handle_arg(frame), policy.required_rights)) {
                frame->result = -1;
                return -1;
            }
        }
    }

    switch ((vibeos_syscall_id_t)frame->id) {
        case VIBEOS_SYSCALL_NOP:
            frame->result = 0;
            return 0;
        case VIBEOS_SYSCALL_EVENT_SIGNAL:
            vibeos_event_signal(&kernel->boot_event);
            frame->result = 0;
            return 0;
        case VIBEOS_SYSCALL_EVENT_CLEAR:
            vibeos_event_clear(&kernel->boot_event);
            frame->result = 0;
            return 0;
        case VIBEOS_SYSCALL_HANDLE_ALLOC:
        {
            uint32_t caller_pid = vibeos_syscall_caller_pid(frame);
            vibeos_handle_table_t *caller_handles = &kernel->handles;
            uint32_t handle;
            if (caller_pid != 0 && vibeos_proc_handles(&kernel->proc_table, caller_pid, &caller_handles) != 0) {
                frame->result = -1;
                return -1;
            }
            if (vibeos_handle_alloc(caller_handles, (uint32_t)frame->arg0, &handle) != 0) {
                frame->result = -1;
                return -1;
            }
            frame->result = (int64_t)handle;
            return 0;
        }
        case VIBEOS_SYSCALL_HANDLE_CLOSE:
        {
            uint32_t caller_pid = vibeos_syscall_caller_pid(frame);
            vibeos_handle_table_t *caller_handles = &kernel->handles;
            if (caller_pid != 0 && vibeos_proc_handles(&kernel->proc_table, caller_pid, &caller_handles) != 0) {
                frame->result = -1;
                return -1;
            }
            if (vibeos_handle_close(caller_handles, (uint32_t)frame->arg0) != 0) {
                frame->result = -1;
                return -1;
            }
            frame->result = 0;
            return 0;
        }
        case VIBEOS_SYSCALL_WAITSET_ADD_EVENT:
            if (kernel_waitset_owner_pid == 0) {
                kernel_waitset_owner_pid = vibeos_syscall_waitset_owner_pid(frame);
                if (kernel_waitset_owner_pid == 0 || vibeos_waitset_init_owned(&kernel_waitset, kernel_waitset_owner_pid) != 0) {
                    frame->result = -1;
                    return -1;
                }
            }
            if (vibeos_waitset_add_owned(&kernel_waitset, &kernel->boot_event, vibeos_syscall_waitset_owner_pid(frame)) != 0) {
                frame->result = -1;
                return -1;
            }
            frame->result = 0;
            return 0;
        case VIBEOS_SYSCALL_WAITSET_STATS_GET:
        {
            uint32_t caller_pid = vibeos_syscall_caller_pid(frame);
            uint64_t added = 0;
            uint64_t removed = 0;
            uint64_t waits = 0;
            uint64_t wakes = 0;
            uint64_t timeouts = 0;
            uint64_t denials = 0;
            if (!syscall_waitset_owner_access_allowed(caller_pid, kernel_waitset_owner_pid, waitset_initialized)) {
                frame->result = -1;
                return -1;
            }
            if (vibeos_waitset_stats(&kernel_waitset, &added, &removed, &waits, &wakes, &timeouts, &denials) != 0) {
                frame->result = -1;
                return -1;
            }
            frame->arg0 = added;
            frame->arg1 = removed;
            frame->arg2 = waits;
            frame->result = (int64_t)timeouts;
            return 0;
        }
        case VIBEOS_SYSCALL_WAITSET_STATS_EXT_GET:
        {
            uint32_t caller_pid = vibeos_syscall_caller_pid(frame);
            uint64_t added = 0;
            uint64_t removed = 0;
            uint64_t waits = 0;
            uint64_t wakes = 0;
            uint64_t timeouts = 0;
            uint64_t denials = 0;
            if (!syscall_waitset_owner_access_allowed(caller_pid, kernel_waitset_owner_pid, waitset_initialized)) {
                frame->result = -1;
                return -1;
            }
            if (vibeos_waitset_stats(&kernel_waitset, &added, &removed, &waits, &wakes, &timeouts, &denials) != 0) {
                frame->result = -1;
                return -1;
            }
            (void)added;
            (void)removed;
            (void)waits;
            frame->arg0 = wakes;
            frame->arg1 = denials;
            frame->arg2 = (uint64_t)kernel_waitset_owner_pid;
            frame->result = (int64_t)kernel_waitset.count;
            return 0;
        }
        case VIBEOS_SYSCALL_WAITSET_WAKE_POLICY_SET:
        {
            uint32_t caller_pid = vibeos_syscall_caller_pid(frame);
            if (!syscall_waitset_owner_access_allowed(caller_pid, kernel_waitset_owner_pid, waitset_initialized)) {
                frame->result = -1;
                return -1;
            }
            if (vibeos_waitset_set_wake_policy(&kernel_waitset, (vibeos_waitset_wake_policy_t)frame->arg0) != 0) {
                frame->result = -1;
                return -1;
            }
            frame->result = 0;
            return 0;
        }
        case VIBEOS_SYSCALL_WAITSET_WAKE_POLICY_GET:
        {
            uint32_t caller_pid = vibeos_syscall_caller_pid(frame);
            vibeos_waitset_wake_policy_t policy;
            if (!syscall_waitset_owner_access_allowed(caller_pid, kernel_waitset_owner_pid, waitset_initialized)) {
                frame->result = -1;
                return -1;
            }
            if (vibeos_waitset_get_wake_policy(&kernel_waitset, &policy) != 0) {
                frame->result = -1;
                return -1;
            }
            frame->result = (int64_t)policy;
            return 0;
        }
        case VIBEOS_SYSCALL_WAITSET_STATS_RESET:
        {
            uint32_t caller_pid = vibeos_syscall_caller_pid(frame);
            if (!syscall_waitset_owner_access_allowed(caller_pid, kernel_waitset_owner_pid, waitset_initialized)) {
                frame->result = -1;
                return -1;
            }
            if (vibeos_waitset_stats_reset(&kernel_waitset) != 0) {
                frame->result = -1;
                return -1;
            }
            frame->result = 0;
            return 0;
        }
        case VIBEOS_SYSCALL_WAITSET_OWNER_GET:
        {
            uint32_t caller_pid = vibeos_syscall_caller_pid(frame);
            uint32_t owner_pid = 0;
            uint32_t enforced = 0;
            if (!syscall_waitset_owner_access_allowed(caller_pid, kernel_waitset_owner_pid, waitset_initialized)) {
                frame->result = -1;
                return -1;
            }
            if (vibeos_waitset_owner(&kernel_waitset, &owner_pid, &enforced) != 0) {
                frame->result = -1;
                return -1;
            }
            frame->arg0 = enforced;
            frame->arg1 = (uint64_t)kernel_waitset.count;
            frame->arg2 = 0;
            frame->result = (int64_t)owner_pid;
            return 0;
        }
        case VIBEOS_SYSCALL_WAITSET_SNAPSHOT_GET:
        {
            uint32_t caller_pid = vibeos_syscall_caller_pid(frame);
            uint32_t owner_pid = 0;
            uint32_t enforced = 0;
            vibeos_waitset_wake_policy_t policy;
            if (!syscall_waitset_owner_access_allowed(caller_pid, kernel_waitset_owner_pid, waitset_initialized)) {
                frame->result = -1;
                return -1;
            }
            if (vibeos_waitset_owner(&kernel_waitset, &owner_pid, &enforced) != 0) {
                frame->result = -1;
                return -1;
            }
            if (vibeos_waitset_get_wake_policy(&kernel_waitset, &policy) != 0) {
                frame->result = -1;
                return -1;
            }
            frame->arg0 = (uint64_t)enforced;
            frame->arg1 = (uint64_t)policy;
            frame->arg2 = (uint64_t)kernel_waitset.count;
            frame->result = (int64_t)owner_pid;
            return 0;
        }
        case VIBEOS_SYSCALL_PROCESS_SPAWN:
        {
            uint32_t caller_pid = vibeos_syscall_caller_pid(frame);
            uint32_t caller_capability_mask = 0;
            vibeos_security_token_t caller_token;
            uint32_t pid;
            if (syscall_caller_capability_mask(kernel, caller_pid, &caller_capability_mask) != 0) {
                syscall_sec_audit_record(kernel, VIBEOS_SEC_AUDIT_PROCESS_SPAWN, caller_pid, 0, (uint64_t)frame->arg0, 0);
                frame->result = -1;
                return -1;
            }
            if (vibeos_policy_can_process_spawn(&kernel->policy, caller_capability_mask) != VIBEOS_POLICY_ALLOW) {
                syscall_sec_audit_record(kernel, VIBEOS_SEC_AUDIT_PROCESS_SPAWN, caller_pid, 0, (uint64_t)frame->arg0, 0);
                frame->result = -1;
                return -1;
            }
            if (caller_pid == 0) {
                caller_token = kernel->kernel_token;
            } else if (vibeos_proc_token_get(&kernel->proc_table, caller_pid, &caller_token) != 0) {
                syscall_sec_audit_record(kernel, VIBEOS_SEC_AUDIT_PROCESS_SPAWN, caller_pid, 0, (uint64_t)frame->arg0, 0);
                frame->result = -1;
                return -1;
            }
            if (vibeos_proc_spawn_with_token(&kernel->proc_table, (uint32_t)frame->arg0, &caller_token, &pid) != 0) {
                syscall_sec_audit_record(kernel, VIBEOS_SEC_AUDIT_PROCESS_SPAWN, caller_pid, 0, (uint64_t)frame->arg0, 0);
                frame->result = -1;
                return -1;
            }
            syscall_sec_audit_record(kernel, VIBEOS_SEC_AUDIT_PROCESS_SPAWN, caller_pid, pid, (uint64_t)frame->arg0, 1);
            frame->result = (int64_t)pid;
            return 0;
        }
        case VIBEOS_SYSCALL_PROCESS_STATE_GET:
        {
            uint32_t caller_pid = vibeos_syscall_caller_pid(frame);
            uint32_t target_pid = (uint32_t)frame->arg0;
            vibeos_process_state_t state;
            if (!syscall_process_view_allowed(kernel, caller_pid, target_pid)) {
                frame->result = -1;
                return -1;
            }
            if (vibeos_proc_state(&kernel->proc_table, target_pid, &state) != 0) {
                frame->result = -1;
                return -1;
            }
            frame->result = (int64_t)state;
            return 0;
        }
        case VIBEOS_SYSCALL_PROCESS_STATE_SET:
        {
            uint32_t caller_pid = vibeos_syscall_caller_pid(frame);
            uint32_t target_pid = (uint32_t)frame->arg0;
            vibeos_process_state_t state = (vibeos_process_state_t)frame->arg1;
            if (!syscall_process_mutation_allowed(caller_pid, target_pid)) {
                frame->result = -1;
                return -1;
            }
            if (vibeos_proc_set_state(&kernel->proc_table, target_pid, state) != 0) {
                frame->result = -1;
                return -1;
            }
            frame->result = 0;
            return 0;
        }
        case VIBEOS_SYSCALL_PROCESS_TERMINATE:
        {
            uint32_t caller_pid = vibeos_syscall_caller_pid(frame);
            uint32_t target_pid = (uint32_t)frame->arg0;
            if (!syscall_process_mutation_allowed(caller_pid, target_pid)) {
                frame->result = -1;
                return -1;
            }
            if (vibeos_proc_terminate(&kernel->proc_table, target_pid) != 0) {
                frame->result = -1;
                return -1;
            }
            frame->result = 0;
            return 0;
        }
        case VIBEOS_SYSCALL_PROCESS_COUNT_GET:
        {
            uint32_t count = 0;
            if (vibeos_proc_process_count(&kernel->proc_table, &count) != 0) {
                frame->result = -1;
                return -1;
            }
            frame->result = (int64_t)count;
            return 0;
        }
        case VIBEOS_SYSCALL_THREAD_COUNT_GET:
        {
            uint32_t count = 0;
            if (vibeos_proc_thread_count(&kernel->proc_table, &count) != 0) {
                frame->result = -1;
                return -1;
            }
            frame->result = (int64_t)count;
            return 0;
        }
        case VIBEOS_SYSCALL_PROCESS_LIVE_COUNT_GET:
        {
            uint32_t count = 0;
            if (vibeos_proc_live_count(&kernel->proc_table, &count) != 0) {
                frame->result = -1;
                return -1;
            }
            frame->result = (int64_t)count;
            return 0;
        }
        case VIBEOS_SYSCALL_PROCESS_TERMINATED_COUNT_GET:
        {
            uint32_t count = 0;
            if (vibeos_proc_terminated_count(&kernel->proc_table, &count) != 0) {
                frame->result = -1;
                return -1;
            }
            frame->result = (int64_t)count;
            return 0;
        }
        case VIBEOS_SYSCALL_PROCESS_STATE_COUNT_GET:
        {
            uint32_t count = 0;
            if (vibeos_proc_count_in_state(&kernel->proc_table, (vibeos_process_state_t)frame->arg0, &count) != 0) {
                frame->result = -1;
                return -1;
            }
            frame->result = (int64_t)count;
            return 0;
        }
        case VIBEOS_SYSCALL_PROCESS_STATE_SUMMARY_GET:
        {
            uint32_t state_new = 0;
            uint32_t state_running = 0;
            uint32_t state_blocked = 0;
            uint32_t state_terminated = 0;
            if (vibeos_proc_state_summary(&kernel->proc_table, &state_new, &state_running, &state_blocked, &state_terminated) != 0) {
                frame->result = -1;
                return -1;
            }
            frame->arg0 = state_new;
            frame->arg1 = state_running;
            frame->arg2 = state_blocked;
            frame->result = (int64_t)state_terminated;
            return 0;
        }
        case VIBEOS_SYSCALL_THREAD_CREATE:
        {
            uint32_t tid;
            if (vibeos_thread_create(&kernel->proc_table, (uint32_t)frame->arg0, &tid) != 0) {
                frame->result = -1;
                return -1;
            }
            frame->result = (int64_t)tid;
            return 0;
        }
        case VIBEOS_SYSCALL_THREAD_STATE_GET:
        {
            uint32_t caller_pid = vibeos_syscall_caller_pid(frame);
            uint32_t tid = (uint32_t)frame->arg0;
            vibeos_thread_state_t state;
            if (!syscall_thread_access_allowed(kernel, caller_pid, tid)) {
                frame->result = -1;
                return -1;
            }
            if (vibeos_thread_state(&kernel->proc_table, tid, &state) != 0) {
                frame->result = -1;
                return -1;
            }
            frame->result = (int64_t)state;
            return 0;
        }
        case VIBEOS_SYSCALL_THREAD_STATE_SET:
        {
            uint32_t caller_pid = vibeos_syscall_caller_pid(frame);
            uint32_t tid = (uint32_t)frame->arg0;
            vibeos_thread_state_t state = (vibeos_thread_state_t)frame->arg1;
            if (!syscall_thread_access_allowed(kernel, caller_pid, tid)) {
                frame->result = -1;
                return -1;
            }
            if (state > VIBEOS_THREAD_STATE_EXITED) {
                frame->result = -1;
                return -1;
            }
            if (vibeos_thread_set_state(&kernel->proc_table, tid, state) != 0) {
                frame->result = -1;
                return -1;
            }
            frame->result = 0;
            return 0;
        }
        case VIBEOS_SYSCALL_THREAD_EXIT:
        {
            uint32_t caller_pid = vibeos_syscall_caller_pid(frame);
            uint32_t tid = (uint32_t)frame->arg0;
            if (!syscall_thread_access_allowed(kernel, caller_pid, tid)) {
                frame->result = -1;
                return -1;
            }
            if (vibeos_thread_exit(&kernel->proc_table, tid) != 0) {
                frame->result = -1;
                return -1;
            }
            frame->result = 0;
            return 0;
        }
        case VIBEOS_SYSCALL_THREAD_STATE_COUNT_GET:
        {
            uint32_t count = 0;
            if (vibeos_thread_count_in_state(&kernel->proc_table, (vibeos_thread_state_t)frame->arg0, &count) != 0) {
                frame->result = -1;
                return -1;
            }
            frame->result = (int64_t)count;
            return 0;
        }
        case VIBEOS_SYSCALL_THREAD_STATE_SUMMARY_GET:
        {
            uint32_t state_new = 0;
            uint32_t state_runnable = 0;
            uint32_t state_blocked = 0;
            uint32_t state_exited = 0;
            if (vibeos_thread_state_summary(&kernel->proc_table, &state_new, &state_runnable, &state_blocked, &state_exited) != 0) {
                frame->result = -1;
                return -1;
            }
            frame->arg0 = state_new;
            frame->arg1 = state_runnable;
            frame->arg2 = state_blocked;
            frame->result = (int64_t)state_exited;
            return 0;
        }
        case VIBEOS_SYSCALL_PROC_TRANSITION_COUNTERS_GET:
        {
            uint64_t proc_transitions = 0;
            uint64_t thread_transitions = 0;
            uint64_t proc_terms = 0;
            uint64_t thread_exits = 0;
            if (vibeos_proc_transition_counters(&kernel->proc_table, &proc_transitions, &thread_transitions, &proc_terms, &thread_exits) != 0) {
                frame->result = -1;
                return -1;
            }
            frame->arg0 = proc_transitions;
            frame->arg1 = thread_transitions;
            frame->arg2 = proc_terms;
            frame->result = (int64_t)thread_exits;
            return 0;
        }
        case VIBEOS_SYSCALL_PROC_TRANSITION_COUNTERS_RESET:
        {
            uint32_t caller_pid = vibeos_syscall_caller_pid(frame);
            if (caller_pid != 0) {
                frame->result = -1;
                return -1;
            }
            if (vibeos_proc_transition_counters_reset(&kernel->proc_table) != 0) {
                frame->result = -1;
                return -1;
            }
            frame->result = 0;
            return 0;
        }
        case VIBEOS_SYSCALL_VM_MAP:
            if (vibeos_vm_map(&kernel->kernel_aspace, (uintptr_t)frame->arg0, (uintptr_t)frame->arg1, (size_t)frame->arg2, VIBEOS_VM_PERM_READ | VIBEOS_VM_PERM_WRITE) != 0) {
                frame->result = -1;
                return -1;
            }
            frame->result = 0;
            return 0;
        case VIBEOS_SYSCALL_VM_UNMAP:
            if (vibeos_vm_unmap(&kernel->kernel_aspace, (uintptr_t)frame->arg0, (size_t)frame->arg1) != 0) {
                frame->result = -1;
                return -1;
            }
            frame->result = 0;
            return 0;
        case VIBEOS_SYSCALL_VM_PROTECT:
            if (vibeos_vm_protect(&kernel->kernel_aspace, (uintptr_t)frame->arg0, (size_t)frame->arg1, (uint32_t)frame->arg2) != 0) {
                frame->result = -1;
                return -1;
            }
            frame->result = 0;
            return 0;
        case VIBEOS_SYSCALL_PROC_AUDIT_COUNT:
        {
            uint32_t count = 0;
            uint32_t caller_pid = vibeos_syscall_caller_pid(frame);
            if (caller_pid == 0) {
                if (vibeos_proc_audit_count(&kernel->proc_table, &count) != 0) {
                    frame->result = -1;
                    return -1;
                }
            } else {
                if (vibeos_proc_audit_count_for_pid(&kernel->proc_table, caller_pid, &count) != 0) {
                    frame->result = -1;
                    return -1;
                }
            }
            frame->result = (int64_t)count;
            return 0;
        }
        case VIBEOS_SYSCALL_PROC_AUDIT_GET:
        {
            uint32_t caller_pid = vibeos_syscall_caller_pid(frame);
            vibeos_proc_audit_event_t ev;
            if (caller_pid == 0) {
                if (vibeos_proc_audit_get(&kernel->proc_table, (uint32_t)frame->arg0, &ev) != 0) {
                    frame->result = -1;
                    return -1;
                }
            } else {
                if (vibeos_proc_audit_get_for_pid(&kernel->proc_table, caller_pid, (uint32_t)frame->arg0, &ev) != 0) {
                    frame->result = -1;
                    return -1;
                }
                /* Redacted for non-kernel callers: hide global sequence. */
                ev.seq = (uint64_t)((uint32_t)frame->arg0 + 1u);
            }
            frame->arg0 = ev.action;
            frame->arg1 = ev.success;
            frame->arg2 = ev.revoked_count;
            frame->result = (int64_t)ev.seq;
            return 0;
        }
        case VIBEOS_SYSCALL_PROC_AUDIT_POLICY_SET:
        {
            uint32_t caller_pid = vibeos_syscall_caller_pid(frame);
            if (caller_pid != 0) {
                frame->result = -1;
                return -1;
            }
            if (vibeos_proc_audit_set_policy(&kernel->proc_table, (vibeos_proc_audit_policy_t)frame->arg0) != 0) {
                frame->result = -1;
                return -1;
            }
            frame->result = 0;
            return 0;
        }
        case VIBEOS_SYSCALL_PROC_AUDIT_POLICY_GET:
        {
            uint32_t caller_pid = vibeos_syscall_caller_pid(frame);
            vibeos_proc_audit_policy_t policy;
            if (caller_pid != 0) {
                frame->result = -1;
                return -1;
            }
            if (vibeos_proc_audit_get_policy(&kernel->proc_table, &policy) != 0) {
                frame->result = -1;
                return -1;
            }
            frame->result = (int64_t)policy;
            return 0;
        }
        case VIBEOS_SYSCALL_PROC_AUDIT_DROPPED:
        {
            uint32_t caller_pid = vibeos_syscall_caller_pid(frame);
            uint32_t dropped = 0;
            if (caller_pid != 0) {
                frame->result = -1;
                return -1;
            }
            if (vibeos_proc_audit_get_dropped(&kernel->proc_table, &dropped) != 0) {
                frame->result = -1;
                return -1;
            }
            frame->result = (int64_t)dropped;
            return 0;
        }
        case VIBEOS_SYSCALL_PROC_AUDIT_COUNT_ACTION:
        {
            uint32_t caller_pid = vibeos_syscall_caller_pid(frame);
            uint32_t action = (uint32_t)frame->arg0;
            uint32_t count = 0;
            if (caller_pid == 0) {
                if (vibeos_proc_audit_count_action(&kernel->proc_table, action, &count) != 0) {
                    frame->result = -1;
                    return -1;
                }
            } else {
                uint32_t i;
                uint32_t visible = 0;
                vibeos_proc_audit_event_t ev;
                uint32_t scoped_count = 0;
                if (vibeos_proc_audit_count_for_pid(&kernel->proc_table, caller_pid, &visible) != 0) {
                    frame->result = -1;
                    return -1;
                }
                for (i = 0; i < visible; i++) {
                    if (vibeos_proc_audit_get_for_pid(&kernel->proc_table, caller_pid, i, &ev) != 0) {
                        frame->result = -1;
                        return -1;
                    }
                    if (ev.action == action) {
                        scoped_count++;
                    }
                }
                count = scoped_count;
            }
            frame->result = (int64_t)count;
            return 0;
        }
        case VIBEOS_SYSCALL_PROC_AUDIT_COUNT_SUCCESS:
        {
            uint32_t caller_pid = vibeos_syscall_caller_pid(frame);
            uint32_t success_value = (uint32_t)frame->arg0;
            uint32_t count = 0;
            if (success_value > 1) {
                frame->result = -1;
                return -1;
            }
            if (caller_pid == 0) {
                if (vibeos_proc_audit_count_success(&kernel->proc_table, success_value, &count) != 0) {
                    frame->result = -1;
                    return -1;
                }
            } else {
                uint32_t i;
                uint32_t visible = 0;
                vibeos_proc_audit_event_t ev;
                uint32_t scoped_count = 0;
                if (vibeos_proc_audit_count_for_pid(&kernel->proc_table, caller_pid, &visible) != 0) {
                    frame->result = -1;
                    return -1;
                }
                for (i = 0; i < visible; i++) {
                    if (vibeos_proc_audit_get_for_pid(&kernel->proc_table, caller_pid, i, &ev) != 0) {
                        frame->result = -1;
                        return -1;
                    }
                    if (ev.success == success_value) {
                        scoped_count++;
                    }
                }
                count = scoped_count;
            }
            frame->result = (int64_t)count;
            return 0;
        }
        case VIBEOS_SYSCALL_PROC_AUDIT_SUMMARY:
        {
            uint32_t caller_pid = vibeos_syscall_caller_pid(frame);
            uint32_t total = 0;
            uint32_t success_count = 0;
            uint32_t fail_count = 0;
            if (caller_pid == 0) {
                if (vibeos_proc_audit_summary(&kernel->proc_table, &total, &success_count, &fail_count) != 0) {
                    frame->result = -1;
                    return -1;
                }
            } else {
                uint32_t i;
                uint32_t visible = 0;
                vibeos_proc_audit_event_t ev;
                if (vibeos_proc_audit_count_for_pid(&kernel->proc_table, caller_pid, &visible) != 0) {
                    frame->result = -1;
                    return -1;
                }
                total = visible;
                for (i = 0; i < visible; i++) {
                    if (vibeos_proc_audit_get_for_pid(&kernel->proc_table, caller_pid, i, &ev) != 0) {
                        frame->result = -1;
                        return -1;
                    }
                    if (ev.success == 1) {
                        success_count++;
                    }
                }
                fail_count = total - success_count;
            }
            frame->arg0 = total;
            frame->arg1 = success_count;
            frame->arg2 = fail_count;
            frame->result = 0;
            return 0;
        }
        case VIBEOS_SYSCALL_SCHED_RUNNABLE_GET:
            frame->result = (int64_t)vibeos_sched_runnable_threads(&kernel->scheduler);
            return 0;
        case VIBEOS_SYSCALL_SCHED_RUNQUEUE_DEPTH_GET:
        {
            uint32_t cpu_count = 0;
            if (vibeos_sched_cpu_count(&kernel->scheduler, &cpu_count) != 0 || (uint32_t)frame->arg0 >= cpu_count) {
                frame->result = -1;
                return -1;
            }
            frame->result = (int64_t)vibeos_sched_runqueue_depth(&kernel->scheduler, (uint32_t)frame->arg0);
            return 0;
        }
        case VIBEOS_SYSCALL_SCHED_CPU_COUNT_GET:
        {
            uint32_t cpu_count = 0;
            if (vibeos_sched_cpu_count(&kernel->scheduler, &cpu_count) != 0) {
                frame->result = -1;
                return -1;
            }
            frame->result = (int64_t)cpu_count;
            return 0;
        }
        case VIBEOS_SYSCALL_SCHED_PREEMPTIONS_GET:
        {
            uint32_t cpu_count = 0;
            if (vibeos_sched_cpu_count(&kernel->scheduler, &cpu_count) != 0 || (uint32_t)frame->arg0 >= cpu_count) {
                frame->result = -1;
                return -1;
            }
            frame->result = (int64_t)vibeos_sched_preemptions(&kernel->scheduler, (uint32_t)frame->arg0);
            return 0;
        }
        case VIBEOS_SYSCALL_SCHED_WAIT_TIMEOUTS_GET:
        {
            uint32_t cpu_count = 0;
            if (vibeos_sched_cpu_count(&kernel->scheduler, &cpu_count) != 0 || (uint32_t)frame->arg0 >= cpu_count) {
                frame->result = -1;
                return -1;
            }
            frame->result = (int64_t)vibeos_sched_wait_timeouts(&kernel->scheduler, (uint32_t)frame->arg0);
            return 0;
        }
        case VIBEOS_SYSCALL_SCHED_WAIT_WAKES_GET:
        {
            uint32_t cpu_count = 0;
            if (vibeos_sched_cpu_count(&kernel->scheduler, &cpu_count) != 0 || (uint32_t)frame->arg0 >= cpu_count) {
                frame->result = -1;
                return -1;
            }
            frame->result = (int64_t)vibeos_sched_wait_wakes(&kernel->scheduler, (uint32_t)frame->arg0);
            return 0;
        }
        case VIBEOS_SYSCALL_SCHED_PREEMPTIONS_TOTAL_GET:
            frame->result = (int64_t)vibeos_sched_preemptions_total(&kernel->scheduler);
            return 0;
        case VIBEOS_SYSCALL_SCHED_WAIT_TIMEOUTS_TOTAL_GET:
            frame->result = (int64_t)vibeos_sched_wait_timeouts_total(&kernel->scheduler);
            return 0;
        case VIBEOS_SYSCALL_SCHED_WAIT_WAKES_TOTAL_GET:
            frame->result = (int64_t)vibeos_sched_wait_wakes_total(&kernel->scheduler);
            return 0;
        case VIBEOS_SYSCALL_SCHED_COUNTER_SUMMARY_GET:
        {
            uint64_t preemptions = 0;
            uint64_t wait_timeouts = 0;
            uint64_t wait_wakes = 0;
            size_t runnable = 0;
            uint32_t cpu_count = 0;
            if (vibeos_sched_counter_summary(&kernel->scheduler, &preemptions, &wait_timeouts, &wait_wakes, &runnable, &cpu_count) != 0) {
                frame->result = -1;
                return -1;
            }
            frame->arg0 = preemptions;
            frame->arg1 = wait_timeouts;
            frame->arg2 = wait_wakes;
            frame->result = (int64_t)runnable;
            return 0;
        }
        case VIBEOS_SYSCALL_SCHED_COUNTERS_RESET:
        {
            uint32_t caller_pid = vibeos_syscall_caller_pid(frame);
            if (caller_pid != 0) {
                frame->result = -1;
                return -1;
            }
            if (vibeos_sched_counters_reset(&kernel->scheduler) != 0) {
                frame->result = -1;
                return -1;
            }
            frame->result = 0;
            return 0;
        }
        case VIBEOS_SYSCALL_PROCESS_TOKEN_GET:
        {
            uint32_t caller_pid = vibeos_syscall_caller_pid(frame);
            uint32_t target_pid = (uint32_t)frame->arg0;
            vibeos_security_token_t token;
            if (!syscall_process_token_access_allowed(kernel, caller_pid, target_pid)) {
                frame->result = -1;
                return -1;
            }
            if (vibeos_proc_token_get(&kernel->proc_table, target_pid, &token) != 0) {
                frame->result = -1;
                return -1;
            }
            frame->arg0 = token.uid;
            frame->arg1 = token.gid;
            frame->arg2 = token.capability_mask;
            frame->result = 0;
            return 0;
        }
        case VIBEOS_SYSCALL_PROCESS_TOKEN_SET:
        {
            uint32_t caller_pid = vibeos_syscall_caller_pid(frame);
            uint32_t target_pid = (uint32_t)frame->arg0;
            vibeos_security_token_t current;
            vibeos_security_token_t updated;
            if (!syscall_process_mutation_allowed(caller_pid, target_pid)) {
                syscall_sec_audit_record(kernel, VIBEOS_SEC_AUDIT_PROCESS_TOKEN_SET, caller_pid, target_pid, frame->arg1, 0);
                frame->result = -1;
                return -1;
            }
            if (vibeos_proc_token_get(&kernel->proc_table, target_pid, &current) != 0) {
                syscall_sec_audit_record(kernel, VIBEOS_SEC_AUDIT_PROCESS_TOKEN_SET, caller_pid, target_pid, frame->arg1, 0);
                frame->result = -1;
                return -1;
            }
            updated = current;
            updated.capability_mask = (uint32_t)frame->arg1;
            if (vibeos_proc_token_set(&kernel->proc_table, target_pid, &updated) != 0) {
                syscall_sec_audit_record(kernel, VIBEOS_SEC_AUDIT_PROCESS_TOKEN_SET, caller_pid, target_pid, frame->arg1, 0);
                frame->result = -1;
                return -1;
            }
            syscall_sec_audit_record(kernel, VIBEOS_SEC_AUDIT_PROCESS_TOKEN_SET, caller_pid, target_pid, frame->arg1, 1);
            frame->result = 0;
            return 0;
        }
        case VIBEOS_SYSCALL_POLICY_CAPABILITY_GET:
        {
            uint32_t required_bit = 0;
            if (vibeos_policy_required_capability_get(&kernel->policy, (vibeos_policy_capability_target_t)frame->arg0, &required_bit) != 0) {
                frame->result = -1;
                return -1;
            }
            frame->result = (int64_t)required_bit;
            return 0;
        }
        case VIBEOS_SYSCALL_POLICY_CAPABILITY_SET:
        {
            uint32_t caller_pid = vibeos_syscall_caller_pid(frame);
            if (caller_pid != 0) {
                syscall_sec_audit_record(kernel, VIBEOS_SEC_AUDIT_POLICY_CAPABILITY_SET, caller_pid, 0, ((frame->arg0 & 0xFFFFFFFFu) << 32) | (frame->arg1 & 0xFFFFFFFFu), 0);
                frame->result = -1;
                return -1;
            }
            if (vibeos_policy_required_capability_set(&kernel->policy, (vibeos_policy_capability_target_t)frame->arg0, (uint32_t)frame->arg1) != 0) {
                syscall_sec_audit_record(kernel, VIBEOS_SEC_AUDIT_POLICY_CAPABILITY_SET, caller_pid, 0, ((frame->arg0 & 0xFFFFFFFFu) << 32) | (frame->arg1 & 0xFFFFFFFFu), 0);
                frame->result = -1;
                return -1;
            }
            syscall_sec_audit_record(kernel, VIBEOS_SEC_AUDIT_POLICY_CAPABILITY_SET, caller_pid, 0, ((frame->arg0 & 0xFFFFFFFFu) << 32) | (frame->arg1 & 0xFFFFFFFFu), 1);
            frame->result = 0;
            return 0;
        }
        case VIBEOS_SYSCALL_POLICY_SUMMARY_GET:
        {
            uint32_t fs_open_bit = 0;
            uint32_t net_bind_bit = 0;
            uint32_t process_spawn_bit = 0;
            if (vibeos_policy_summary(&kernel->policy, &fs_open_bit, &net_bind_bit, &process_spawn_bit) != 0) {
                frame->result = -1;
                return -1;
            }
            frame->arg0 = fs_open_bit;
            frame->arg1 = net_bind_bit;
            frame->arg2 = process_spawn_bit;
            frame->result = 0;
            return 0;
        }
        case VIBEOS_SYSCALL_SEC_AUDIT_COUNT:
        {
            uint32_t caller_pid = vibeos_syscall_caller_pid(frame);
            uint32_t count = 0;
            if (caller_pid == 0) {
                if (vibeos_sec_audit_count(&kernel->sec_audit, &count) != 0) {
                    frame->result = -1;
                    return -1;
                }
            } else {
                if (vibeos_sec_audit_count_for_pid(&kernel->sec_audit, caller_pid, &count) != 0) {
                    frame->result = -1;
                    return -1;
                }
            }
            frame->result = (int64_t)count;
            return 0;
        }
        case VIBEOS_SYSCALL_SEC_AUDIT_GET:
        {
            uint32_t caller_pid = vibeos_syscall_caller_pid(frame);
            vibeos_sec_audit_event_t event;
            if (caller_pid == 0) {
                if (vibeos_sec_audit_get(&kernel->sec_audit, (uint32_t)frame->arg0, &event) != 0) {
                    frame->result = -1;
                    return -1;
                }
            } else {
                if (vibeos_sec_audit_get_for_pid(&kernel->sec_audit, caller_pid, (uint32_t)frame->arg0, &event) != 0) {
                    frame->result = -1;
                    return -1;
                }
                event.seq = (uint64_t)((uint32_t)frame->arg0 + 1u);
            }
            frame->arg0 = event.action;
            frame->arg1 = event.success;
            frame->arg2 = event.target_pid;
            frame->result = (int64_t)event.seq;
            return 0;
        }
        case VIBEOS_SYSCALL_SEC_AUDIT_COUNT_ACTION:
        {
            uint32_t caller_pid = vibeos_syscall_caller_pid(frame);
            uint32_t action = (uint32_t)frame->arg0;
            uint32_t count = 0;
            if (caller_pid == 0) {
                if (vibeos_sec_audit_count_action(&kernel->sec_audit, action, &count) != 0) {
                    frame->result = -1;
                    return -1;
                }
            } else {
                uint32_t i;
                uint32_t visible = 0;
                uint32_t scoped_count = 0;
                vibeos_sec_audit_event_t event;
                if (vibeos_sec_audit_count_for_pid(&kernel->sec_audit, caller_pid, &visible) != 0) {
                    frame->result = -1;
                    return -1;
                }
                for (i = 0; i < visible; i++) {
                    if (vibeos_sec_audit_get_for_pid(&kernel->sec_audit, caller_pid, i, &event) != 0) {
                        frame->result = -1;
                        return -1;
                    }
                    if (event.action == action) {
                        scoped_count++;
                    }
                }
                count = scoped_count;
            }
            frame->result = (int64_t)count;
            return 0;
        }
        case VIBEOS_SYSCALL_SEC_AUDIT_SUMMARY:
        {
            uint32_t caller_pid = vibeos_syscall_caller_pid(frame);
            uint32_t total = 0;
            uint32_t success_count = 0;
            uint32_t fail_count = 0;
            if (caller_pid == 0) {
                if (vibeos_sec_audit_summary(&kernel->sec_audit, &total, &success_count, &fail_count) != 0) {
                    frame->result = -1;
                    return -1;
                }
            } else {
                uint32_t i;
                uint32_t visible = 0;
                vibeos_sec_audit_event_t event;
                if (vibeos_sec_audit_count_for_pid(&kernel->sec_audit, caller_pid, &visible) != 0) {
                    frame->result = -1;
                    return -1;
                }
                total = visible;
                for (i = 0; i < visible; i++) {
                    if (vibeos_sec_audit_get_for_pid(&kernel->sec_audit, caller_pid, i, &event) != 0) {
                        frame->result = -1;
                        return -1;
                    }
                    if (event.success) {
                        success_count++;
                    }
                }
                fail_count = total - success_count;
            }
            frame->arg0 = total;
            frame->arg1 = success_count;
            frame->arg2 = fail_count;
            frame->result = 0;
            return 0;
        }
        case VIBEOS_SYSCALL_SEC_AUDIT_RESET:
        {
            uint32_t caller_pid = vibeos_syscall_caller_pid(frame);
            if (caller_pid != 0) {
                frame->result = -1;
                return -1;
            }
            if (vibeos_sec_audit_reset(&kernel->sec_audit) != 0) {
                frame->result = -1;
                return -1;
            }
            frame->result = 0;
            return 0;
        }
        default:
            frame->result = -1;
            return -1;
    }
}
