#include "vibeos/services.h"

int vibeos_servicemgr_start(vibeos_init_state_t *init_state, vibeos_devmgr_state_t *devmgr_state, vibeos_vfs_state_t *vfs_state, vibeos_net_state_t *net_state) {
    if (!init_state || !devmgr_state || !vfs_state || !net_state) {
        return -1;
    }
    if (vibeos_init_start(init_state) != 0) {
        return -1;
    }
    if (vibeos_devmgr_start(devmgr_state) != 0) {
        return -1;
    }
    if (vibeos_vfs_start(vfs_state) != 0) {
        return -1;
    }
    if (vibeos_net_start(net_state) != 0) {
        return -1;
    }
    init_state->started_services = 4;
    return 0;
}
