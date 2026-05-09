#include "vibeos/services.h"

int vibeos_init_start(vibeos_init_state_t *state) {
    if (!state) {
        return -1;
    }
    state->state = VIBEOS_SERVICE_RUNNING;
    state->started_services = 1;
    state->dependency_resolved = 0;
    if (state->restart_budget_core == 0) {
        state->restart_budget_core = 3;
    }
    if (state->restart_budget_optional == 0) {
        state->restart_budget_optional = 1;
    }
    state->restart_attempts_core = 0;
    state->restart_attempts_optional = 0;
    return 0;
}

int vibeos_init_stop(vibeos_init_state_t *state) {
    if (!state) {
        return -1;
    }
    state->state = VIBEOS_SERVICE_STOPPED;
    state->started_services = 0;
    state->dependency_resolved = 0;
    state->restart_attempts_core = 0;
    state->restart_attempts_optional = 0;
    return 0;
}

int vibeos_init_graph_start(vibeos_init_state_t *state, const vibeos_init_node_t *nodes, uint32_t node_count, uint32_t *out_started, uint32_t *out_failed) {
    uint32_t resolved = 0;
    uint32_t started = 0;
    uint32_t pass_progress;
    if (!state || !nodes || !out_started || !out_failed || node_count == 0 || node_count > VIBEOS_INIT_MAX_GRAPH_NODES) {
        return -1;
    }
    do {
        uint32_t i;
        pass_progress = 0;
        for (i = 0; i < node_count; i++) {
            uint32_t bit = (1u << i);
            uint32_t deps = nodes[i].dependency_mask & ((1u << node_count) - 1u);
            if ((resolved & bit) != 0 || !nodes[i].enabled) {
                continue;
            }
            if ((deps & resolved) == deps) {
                resolved |= bit;
                started++;
                pass_progress = 1;
            }
        }
    } while (pass_progress);

    *out_started = started;
    *out_failed = 0;
    if (started < node_count) {
        uint32_t i;
        for (i = 0; i < node_count; i++) {
            if ((resolved & (1u << i)) == 0 && nodes[i].enabled) {
                (*out_failed)++;
            }
        }
        state->dependency_resolved = resolved;
        state->started_services = started;
        return -1;
    }
    state->dependency_resolved = resolved;
    state->started_services = started;
    return 0;
}

int vibeos_init_restart_policy(vibeos_init_state_t *state, uint32_t core_budget, uint32_t optional_budget) {
    if (!state || core_budget == 0) {
        return -1;
    }
    state->restart_budget_core = core_budget;
    state->restart_budget_optional = optional_budget;
    if (state->restart_attempts_core > state->restart_budget_core) {
        state->restart_attempts_core = state->restart_budget_core;
    }
    if (state->restart_attempts_optional > state->restart_budget_optional) {
        state->restart_attempts_optional = state->restart_budget_optional;
    }
    return 0;
}

int vibeos_init_restart_note(vibeos_init_state_t *state, vibeos_init_service_class_t service_class) {
    if (!state) {
        return -1;
    }
    if (service_class == VIBEOS_INIT_SERVICE_CORE) {
        if (state->restart_attempts_core >= state->restart_budget_core) {
            return -1;
        }
        state->restart_attempts_core++;
        return 0;
    }
    if (state->restart_attempts_optional >= state->restart_budget_optional) {
        return -1;
    }
    state->restart_attempts_optional++;
    return 0;
}

int vibeos_init_restart_allowed(const vibeos_init_state_t *state, vibeos_init_service_class_t service_class, uint32_t *out_allowed) {
    if (!state || !out_allowed) {
        return -1;
    }
    if (service_class == VIBEOS_INIT_SERVICE_CORE) {
        *out_allowed = (state->restart_attempts_core < state->restart_budget_core) ? 1u : 0u;
        return 0;
    }
    *out_allowed = (state->restart_attempts_optional < state->restart_budget_optional) ? 1u : 0u;
    return 0;
}
