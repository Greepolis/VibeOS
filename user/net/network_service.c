#include "vibeos/services.h"

int vibeos_net_start(vibeos_net_state_t *state) {
    if (!state) {
        return -1;
    }
    state->state = VIBEOS_SERVICE_RUNNING;
    state->interfaces_online = 0;
    return 0;
}

int vibeos_net_stop(vibeos_net_state_t *state) {
    if (!state) {
        return -1;
    }
    state->state = VIBEOS_SERVICE_STOPPED;
    state->interfaces_online = 0;
    return 0;
}
