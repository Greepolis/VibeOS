#include "vibeos/services.h"

int vibeos_servicemgr_start(vibeos_servicemgr_state_t *mgr, vibeos_init_state_t *init_state, vibeos_devmgr_state_t *devmgr_state, vibeos_vfs_state_t *vfs_state, vibeos_net_state_t *net_state) {
    if (!mgr || !init_state || !devmgr_state || !vfs_state || !net_state) {
        return -1;
    }
    mgr->state = VIBEOS_SERVICE_RUNNING;
    mgr->supervised_count = 0;
    if (mgr->restart_budget == 0) {
        mgr->restart_budget = 3;
    }
    mgr->restart_attempts = 0;
    mgr->failed_services = 0;
    if (vibeos_init_start(init_state) != 0) {
        return -1;
    }
    mgr->supervised_count++;
    if (vibeos_devmgr_start(devmgr_state) != 0) {
        return -1;
    }
    mgr->supervised_count++;
    if (vibeos_vfs_start(vfs_state) != 0) {
        return -1;
    }
    mgr->supervised_count++;
    if (vibeos_net_start(net_state) != 0) {
        return -1;
    }
    mgr->supervised_count++;
    init_state->started_services = 4;
    return 0;
}

int vibeos_servicemgr_stop(vibeos_servicemgr_state_t *mgr, vibeos_init_state_t *init_state, vibeos_devmgr_state_t *devmgr_state, vibeos_vfs_state_t *vfs_state, vibeos_net_state_t *net_state) {
    if (!mgr || !init_state || !devmgr_state || !vfs_state || !net_state) {
        return -1;
    }
    if (vibeos_net_stop(net_state) != 0) {
        return -1;
    }
    if (vibeos_vfs_stop(vfs_state) != 0) {
        return -1;
    }
    if (vibeos_devmgr_stop(devmgr_state) != 0) {
        return -1;
    }
    if (vibeos_init_stop(init_state) != 0) {
        return -1;
    }
    mgr->supervised_count = 0;
    mgr->restart_attempts = 0;
    mgr->failed_services = 0;
    mgr->state = VIBEOS_SERVICE_STOPPED;
    return 0;
}

int vibeos_servicemgr_health(const vibeos_servicemgr_state_t *mgr, const vibeos_init_state_t *init_state, const vibeos_devmgr_state_t *devmgr_state, const vibeos_vfs_state_t *vfs_state, const vibeos_net_state_t *net_state, uint32_t *out_running_services) {
    uint32_t running = 0;
    if (!mgr || !init_state || !devmgr_state || !vfs_state || !net_state || !out_running_services) {
        return -1;
    }
    if (mgr->state == VIBEOS_SERVICE_RUNNING) {
        running++;
    }
    if (init_state->state == VIBEOS_SERVICE_RUNNING) {
        running++;
    }
    if (devmgr_state->state == VIBEOS_SERVICE_RUNNING) {
        running++;
    }
    if (vfs_state->state == VIBEOS_SERVICE_RUNNING) {
        running++;
    }
    if (net_state->state == VIBEOS_SERVICE_RUNNING) {
        running++;
    }
    *out_running_services = running;
    return 0;
}

int vibeos_servicemgr_set_restart_budget(vibeos_servicemgr_state_t *mgr, uint32_t budget) {
    if (!mgr) {
        return -1;
    }
    mgr->restart_budget = budget;
    if (mgr->restart_attempts > mgr->restart_budget) {
        mgr->restart_attempts = mgr->restart_budget;
    }
    return 0;
}

int vibeos_servicemgr_report_service_failure(vibeos_servicemgr_state_t *mgr) {
    if (!mgr || mgr->state != VIBEOS_SERVICE_RUNNING) {
        return -1;
    }
    mgr->failed_services++;
    if (mgr->restart_attempts < mgr->restart_budget) {
        mgr->restart_attempts++;
        return 0;
    }
    return -1;
}

int vibeos_servicemgr_can_restart(const vibeos_servicemgr_state_t *mgr, uint32_t *out_allowed) {
    if (!mgr || !out_allowed) {
        return -1;
    }
    *out_allowed = (mgr->restart_attempts < mgr->restart_budget) ? 1u : 0u;
    return 0;
}
