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
        case VIBEOS_SYSCALL_PROCESS_SPAWN:
        {
            uint32_t pid;
            if (vibeos_policy_can_process_spawn(&kernel->policy, kernel->kernel_token.capability_mask) != VIBEOS_POLICY_ALLOW) {
                frame->result = -1;
                return -1;
            }
            if (vibeos_proc_spawn(&kernel->proc_table, (uint32_t)frame->arg0, &pid) != 0) {
                frame->result = -1;
                return -1;
            }
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
        default:
            frame->result = -1;
            return -1;
    }
}
