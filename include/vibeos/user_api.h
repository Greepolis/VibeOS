#ifndef VIBEOS_USER_API_H
#define VIBEOS_USER_API_H

#include <stdint.h>

#define VIBEOS_USER_API_VERSION_MAJOR 1u
#define VIBEOS_USER_API_VERSION_MINOR 1u

typedef struct vibeos_user_context {
    uint32_t pid;
    uint32_t tid;
} vibeos_user_context_t;

typedef struct vibeos_user_api_caps {
    uint32_t supports_boot_event_signal;
    uint32_t supports_process_security_label;
    uint32_t supports_process_interaction_check;
    uint32_t supports_policy_summary;
} vibeos_user_api_caps_t;

int vibeos_user_context_init(vibeos_user_context_t *ctx, uint32_t pid, uint32_t tid);
int vibeos_user_api_contract(uint32_t *out_major, uint32_t *out_minor);
int vibeos_user_api_capabilities(vibeos_user_api_caps_t *out_caps);
int vibeos_user_signal_boot_event(void *kernel_ptr, uint32_t signal_handle);
int vibeos_user_get_process_security_label(void *kernel_ptr, uint32_t caller_pid, uint32_t target_pid, uint32_t *out_label);
int vibeos_user_set_process_security_label(void *kernel_ptr, uint32_t caller_pid, uint32_t target_pid, uint32_t label);
int vibeos_user_check_process_interaction(void *kernel_ptr, uint32_t caller_pid, uint32_t target_pid, uint32_t *out_allowed);

#endif
