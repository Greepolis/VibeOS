#include "vibeos/kernel.h"
#include "vibeos/syscall.h"
#include "vibeos/syscall_policy.h"
#include "vibeos/waitset.h"

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
        vibeos_syscall_policy_t policy = vibeos_syscall_policy_for((vibeos_syscall_id_t)frame->id);
        if (policy.requires_handle) {
            if (!vibeos_handle_has_rights(&kernel->handles, (uint32_t)frame->arg0, policy.required_rights)) {
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
            uint32_t handle;
            if (vibeos_handle_alloc(&kernel->handles, (uint32_t)frame->arg0, &handle) != 0) {
                frame->result = -1;
                return -1;
            }
            frame->result = (int64_t)handle;
            return 0;
        }
        case VIBEOS_SYSCALL_HANDLE_CLOSE:
            if (vibeos_handle_close(&kernel->handles, (uint32_t)frame->arg0) != 0) {
                frame->result = -1;
                return -1;
            }
            frame->result = 0;
            return 0;
        case VIBEOS_SYSCALL_WAITSET_ADD_EVENT:
            if (kernel_waitset_owner_pid == 0) {
                kernel_waitset_owner_pid = (uint32_t)frame->arg1;
                if (kernel_waitset_owner_pid == 0 || vibeos_waitset_init_owned(&kernel_waitset, kernel_waitset_owner_pid) != 0) {
                    frame->result = -1;
                    return -1;
                }
            }
            if (vibeos_waitset_add_owned(&kernel_waitset, &kernel->boot_event, (uint32_t)frame->arg1) != 0) {
                frame->result = -1;
                return -1;
            }
            frame->result = 0;
            return 0;
        case VIBEOS_SYSCALL_PROCESS_SPAWN:
        {
            uint32_t pid;
            if (vibeos_proc_spawn(&kernel->proc_table, (uint32_t)frame->arg0, &pid) != 0) {
                frame->result = -1;
                return -1;
            }
            frame->result = (int64_t)pid;
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
        default:
            frame->result = -1;
            return -1;
    }
}
