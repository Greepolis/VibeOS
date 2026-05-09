#include "vibeos/kernel.h"
#include "vibeos/syscall.h"
#include "vibeos/syscall_abi.h"
#include "vibeos/user_api.h"

int vibeos_user_context_init(vibeos_user_context_t *ctx, uint32_t pid, uint32_t tid) {
    if (!ctx) {
        return -1;
    }
    ctx->pid = pid;
    ctx->tid = tid;
    return 0;
}

int vibeos_user_api_contract(uint32_t *out_major, uint32_t *out_minor) {
    if (!out_major || !out_minor) {
        return -1;
    }
    *out_major = VIBEOS_USER_API_VERSION_MAJOR;
    *out_minor = VIBEOS_USER_API_VERSION_MINOR;
    return 0;
}

int vibeos_user_api_capabilities(vibeos_user_api_caps_t *out_caps) {
    if (!out_caps) {
        return -1;
    }
    out_caps->supports_boot_event_signal = 1;
    out_caps->supports_process_security_label = 1;
    out_caps->supports_process_interaction_check = 1;
    out_caps->supports_policy_summary = 1;
    return 0;
}

int vibeos_user_signal_boot_event(void *kernel_ptr, uint32_t signal_handle) {
    vibeos_syscall_frame_t frame;
    vibeos_kernel_t *kernel = (vibeos_kernel_t *)kernel_ptr;
    if (!kernel) {
        return -1;
    }
    frame.id = VIBEOS_SYSCALL_EVENT_SIGNAL;
    frame.arg0 = signal_handle;
    frame.arg1 = 0;
    frame.arg2 = 0;
    frame.result = -1;
    return (int)vibeos_syscall_dispatch(kernel, &frame);
}

int vibeos_user_get_process_security_label(void *kernel_ptr, uint32_t caller_pid, uint32_t target_pid, uint32_t *out_label) {
    vibeos_syscall_frame_t frame;
    vibeos_kernel_t *kernel = (vibeos_kernel_t *)kernel_ptr;
    if (!kernel || !out_label) {
        return -1;
    }
    vibeos_syscall_make_process_security_label_get(&frame, target_pid, caller_pid);
    if (vibeos_syscall_dispatch(kernel, &frame) != 0) {
        return -1;
    }
    *out_label = (uint32_t)frame.result;
    return 0;
}

int vibeos_user_set_process_security_label(void *kernel_ptr, uint32_t caller_pid, uint32_t target_pid, uint32_t label) {
    vibeos_syscall_frame_t frame;
    vibeos_kernel_t *kernel = (vibeos_kernel_t *)kernel_ptr;
    if (!kernel) {
        return -1;
    }
    vibeos_syscall_make_process_security_label_set(&frame, target_pid, label, caller_pid);
    return (int)vibeos_syscall_dispatch(kernel, &frame);
}

int vibeos_user_check_process_interaction(void *kernel_ptr, uint32_t caller_pid, uint32_t target_pid, uint32_t *out_allowed) {
    vibeos_syscall_frame_t frame;
    vibeos_kernel_t *kernel = (vibeos_kernel_t *)kernel_ptr;
    if (!kernel || !out_allowed) {
        return -1;
    }
    vibeos_syscall_make_process_interact_check(&frame, target_pid, caller_pid);
    if (vibeos_syscall_dispatch(kernel, &frame) != 0) {
        return -1;
    }
    *out_allowed = (uint32_t)frame.result;
    return 0;
}
