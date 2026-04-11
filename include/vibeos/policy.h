#ifndef VIBEOS_POLICY_H
#define VIBEOS_POLICY_H

#include <stdint.h>

typedef enum vibeos_policy_action {
    VIBEOS_POLICY_DENY = 0,
    VIBEOS_POLICY_ALLOW = 1
} vibeos_policy_action_t;

typedef enum vibeos_policy_capability_target {
    VIBEOS_POLICY_TARGET_FS_OPEN = 1,
    VIBEOS_POLICY_TARGET_NET_BIND = 2,
    VIBEOS_POLICY_TARGET_PROCESS_SPAWN = 3,
    VIBEOS_POLICY_TARGET_PROCESS_INTERACT_OVERRIDE = 4
} vibeos_policy_capability_target_t;

typedef struct vibeos_policy_state {
    uint32_t fs_open_required_capability_bit;
    uint32_t net_bind_required_capability_bit;
    uint32_t process_spawn_required_capability_bit;
    uint32_t process_interact_override_capability_bit;
    uint32_t mac_enforced;
} vibeos_policy_state_t;

int vibeos_policy_init(vibeos_policy_state_t *policy);
vibeos_policy_action_t vibeos_policy_can_fs_open(const vibeos_policy_state_t *policy, uint32_t capability_mask);
vibeos_policy_action_t vibeos_policy_can_net_bind(const vibeos_policy_state_t *policy, uint32_t capability_mask);
vibeos_policy_action_t vibeos_policy_can_process_spawn(const vibeos_policy_state_t *policy, uint32_t capability_mask);
int vibeos_policy_required_capability_get(const vibeos_policy_state_t *policy, vibeos_policy_capability_target_t target, uint32_t *out_capability_bit);
int vibeos_policy_required_capability_set(vibeos_policy_state_t *policy, vibeos_policy_capability_target_t target, uint32_t capability_bit);
int vibeos_policy_summary(const vibeos_policy_state_t *policy, uint32_t *out_fs_open_bit, uint32_t *out_net_bind_bit, uint32_t *out_process_spawn_bit, uint32_t *out_process_interact_override_bit);
int vibeos_policy_set_mac_enforced(vibeos_policy_state_t *policy, uint32_t enabled);
int vibeos_policy_get_mac_enforced(const vibeos_policy_state_t *policy, uint32_t *out_enabled);
vibeos_policy_action_t vibeos_policy_can_process_interact_mac(const vibeos_policy_state_t *policy, uint32_t caller_label, uint32_t target_label, uint32_t capability_mask);

#endif
