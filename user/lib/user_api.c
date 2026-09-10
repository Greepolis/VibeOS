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
    /* Zero, because the functions that implemented these went with the
     * dispatcher they called. An API that advertises a capability it no
     * longer has is the mirror of this project's most repeated defect: not
     * a mechanism nobody consults, but a claim nothing backs. The test that
     * asserted these were 1 is what caught it. */
    out_caps->supports_boot_event_signal = 0;
    out_caps->supports_process_security_label = 0;
    out_caps->supports_process_interaction_check = 0;
    out_caps->supports_policy_summary = 0;
    return 0;
}
/* The half of this API that called vibeos_syscall_dispatch went with the
 * dispatcher. It reached the portable kernel by calling into it in-process,
 * which is not how a syscall works and not a path any ring-3 program took -
 * the live kernel links this library only for the ELF blobs it embeds.
 * Removed: vibeos_user_signal_boot_event, vibeos_user_get_process_security_label, vibeos_user_set_process_security_label, vibeos_user_check_process_interaction. */
