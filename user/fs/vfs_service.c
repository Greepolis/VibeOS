#include "vibeos/services.h"

int vibeos_vfs_start(vibeos_vfs_state_t *state) {
    if (!state) {
        return -1;
    }
    state->state = VIBEOS_SERVICE_RUNNING;
    state->mount_count = 0;
    return 0;
}
