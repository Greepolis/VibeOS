#include "vibeos/policy.h"

int vibeos_policy_init(vibeos_policy_state_t *policy) {
    if (!policy) {
        return -1;
    }
    policy->fs_open_required_capability_bit = 0;
    policy->net_bind_required_capability_bit = 1;
    policy->process_spawn_required_capability_bit = 2;
    return 0;
}

vibeos_policy_action_t vibeos_policy_can_fs_open(const vibeos_policy_state_t *policy, uint32_t capability_mask) {
    if (!policy) {
        return VIBEOS_POLICY_DENY;
    }
    return (capability_mask & (1u << policy->fs_open_required_capability_bit)) ? VIBEOS_POLICY_ALLOW : VIBEOS_POLICY_DENY;
}

vibeos_policy_action_t vibeos_policy_can_net_bind(const vibeos_policy_state_t *policy, uint32_t capability_mask) {
    if (!policy) {
        return VIBEOS_POLICY_DENY;
    }
    return (capability_mask & (1u << policy->net_bind_required_capability_bit)) ? VIBEOS_POLICY_ALLOW : VIBEOS_POLICY_DENY;
}

vibeos_policy_action_t vibeos_policy_can_process_spawn(const vibeos_policy_state_t *policy, uint32_t capability_mask) {
    if (!policy) {
        return VIBEOS_POLICY_DENY;
    }
    return (capability_mask & (1u << policy->process_spawn_required_capability_bit)) ? VIBEOS_POLICY_ALLOW : VIBEOS_POLICY_DENY;
}
