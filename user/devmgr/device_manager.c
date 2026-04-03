#include "vibeos/services.h"

int vibeos_devmgr_start(vibeos_devmgr_state_t *state) {
    if (!state) {
        return -1;
    }
    state->state = VIBEOS_SERVICE_RUNNING;
    state->discovered_devices = 0;
    return 0;
}
