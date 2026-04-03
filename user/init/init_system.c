#include "vibeos/services.h"

int vibeos_init_start(vibeos_init_state_t *state) {
    if (!state) {
        return -1;
    }
    state->state = VIBEOS_SERVICE_RUNNING;
    state->started_services = 1;
    return 0;
}
