#include "vibeos/policy.h"

static int policy_capability_target_valid(vibeos_policy_capability_target_t target) {
    return target == VIBEOS_POLICY_TARGET_FS_OPEN ||
        target == VIBEOS_POLICY_TARGET_NET_BIND ||
        target == VIBEOS_POLICY_TARGET_PROCESS_SPAWN ||
        target == VIBEOS_POLICY_TARGET_PROCESS_INTERACT_OVERRIDE;
}

int vibeos_policy_init(vibeos_policy_state_t *policy) {
    if (!policy) {
        return -1;
    }
    policy->fs_open_required_capability_bit = 0;
    policy->net_bind_required_capability_bit = 1;
    policy->process_spawn_required_capability_bit = 2;
    policy->process_interact_override_capability_bit = 3;
    policy->mac_enforced = 0;
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

int vibeos_policy_required_capability_get(const vibeos_policy_state_t *policy, vibeos_policy_capability_target_t target, uint32_t *out_capability_bit) {
    if (!policy || !out_capability_bit || !policy_capability_target_valid(target)) {
        return -1;
    }
    switch (target) {
        case VIBEOS_POLICY_TARGET_FS_OPEN:
            *out_capability_bit = policy->fs_open_required_capability_bit;
            return 0;
        case VIBEOS_POLICY_TARGET_NET_BIND:
            *out_capability_bit = policy->net_bind_required_capability_bit;
            return 0;
        case VIBEOS_POLICY_TARGET_PROCESS_SPAWN:
            *out_capability_bit = policy->process_spawn_required_capability_bit;
            return 0;
        case VIBEOS_POLICY_TARGET_PROCESS_INTERACT_OVERRIDE:
            *out_capability_bit = policy->process_interact_override_capability_bit;
            return 0;
        default:
            return -1;
    }
}

int vibeos_policy_required_capability_set(vibeos_policy_state_t *policy, vibeos_policy_capability_target_t target, uint32_t capability_bit) {
    if (!policy || !policy_capability_target_valid(target) || capability_bit >= 32u) {
        return -1;
    }
    switch (target) {
        case VIBEOS_POLICY_TARGET_FS_OPEN:
            policy->fs_open_required_capability_bit = capability_bit;
            return 0;
        case VIBEOS_POLICY_TARGET_NET_BIND:
            policy->net_bind_required_capability_bit = capability_bit;
            return 0;
        case VIBEOS_POLICY_TARGET_PROCESS_SPAWN:
            policy->process_spawn_required_capability_bit = capability_bit;
            return 0;
        case VIBEOS_POLICY_TARGET_PROCESS_INTERACT_OVERRIDE:
            policy->process_interact_override_capability_bit = capability_bit;
            return 0;
        default:
            return -1;
    }
}

int vibeos_policy_summary(const vibeos_policy_state_t *policy, uint32_t *out_fs_open_bit, uint32_t *out_net_bind_bit, uint32_t *out_process_spawn_bit, uint32_t *out_process_interact_override_bit) {
    if (!policy || !out_fs_open_bit || !out_net_bind_bit || !out_process_spawn_bit || !out_process_interact_override_bit) {
        return -1;
    }
    *out_fs_open_bit = policy->fs_open_required_capability_bit;
    *out_net_bind_bit = policy->net_bind_required_capability_bit;
    *out_process_spawn_bit = policy->process_spawn_required_capability_bit;
    *out_process_interact_override_bit = policy->process_interact_override_capability_bit;
    return 0;
}

int vibeos_policy_set_mac_enforced(vibeos_policy_state_t *policy, uint32_t enabled) {
    if (!policy) {
        return -1;
    }
    policy->mac_enforced = enabled ? 1u : 0u;
    return 0;
}

int vibeos_policy_get_mac_enforced(const vibeos_policy_state_t *policy, uint32_t *out_enabled) {
    if (!policy || !out_enabled) {
        return -1;
    }
    *out_enabled = policy->mac_enforced;
    return 0;
}

vibeos_policy_action_t vibeos_policy_can_process_interact_mac(const vibeos_policy_state_t *policy, uint32_t caller_label, uint32_t target_label, uint32_t capability_mask) {
    if (!policy) {
        return VIBEOS_POLICY_DENY;
    }
    if (caller_label == target_label) {
        return VIBEOS_POLICY_ALLOW;
    }
    if (!policy->mac_enforced) {
        return VIBEOS_POLICY_ALLOW;
    }
    if (policy->process_interact_override_capability_bit < 32u &&
        (capability_mask & (1u << policy->process_interact_override_capability_bit)) != 0) {
        return VIBEOS_POLICY_ALLOW;
    }
    return (caller_label <= target_label) ? VIBEOS_POLICY_ALLOW : VIBEOS_POLICY_DENY;
}
