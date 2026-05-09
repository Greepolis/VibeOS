#ifndef VIBEOS_SERVICES_H
#define VIBEOS_SERVICES_H

#include <stdint.h>

#define VIBEOS_INIT_MAX_GRAPH_NODES 16u

typedef enum vibeos_service_state {
    VIBEOS_SERVICE_STOPPED = 0,
    VIBEOS_SERVICE_RUNNING = 1
} vibeos_service_state_t;

typedef enum vibeos_init_service_class {
    VIBEOS_INIT_SERVICE_CORE = 0,
    VIBEOS_INIT_SERVICE_OPTIONAL = 1
} vibeos_init_service_class_t;

typedef struct vibeos_init_node {
    uint32_t service_id;
    uint32_t dependency_mask;
    uint32_t enabled;
    vibeos_init_service_class_t restart_class;
} vibeos_init_node_t;

typedef struct vibeos_init_state {
    vibeos_service_state_t state;
    uint32_t started_services;
    uint32_t dependency_resolved;
    uint32_t restart_budget_core;
    uint32_t restart_budget_optional;
    uint32_t restart_attempts_core;
    uint32_t restart_attempts_optional;
} vibeos_init_state_t;

typedef struct vibeos_devmgr_state {
    vibeos_service_state_t state;
    uint32_t discovered_devices;
} vibeos_devmgr_state_t;

typedef struct vibeos_vfs_state {
    vibeos_service_state_t state;
    uint32_t mount_count;
} vibeos_vfs_state_t;

typedef struct vibeos_net_state {
    vibeos_service_state_t state;
    uint32_t interfaces_online;
} vibeos_net_state_t;

typedef struct vibeos_servicemgr_state {
    vibeos_service_state_t state;
    uint32_t supervised_count;
    uint32_t restart_budget;
    uint32_t restart_attempts;
    uint32_t failed_services;
} vibeos_servicemgr_state_t;

int vibeos_init_start(vibeos_init_state_t *state);
int vibeos_init_stop(vibeos_init_state_t *state);
int vibeos_init_graph_start(vibeos_init_state_t *state, const vibeos_init_node_t *nodes, uint32_t node_count, uint32_t *out_started, uint32_t *out_failed);
int vibeos_init_restart_policy(vibeos_init_state_t *state, uint32_t core_budget, uint32_t optional_budget);
int vibeos_init_restart_note(vibeos_init_state_t *state, vibeos_init_service_class_t service_class);
int vibeos_init_restart_allowed(const vibeos_init_state_t *state, vibeos_init_service_class_t service_class, uint32_t *out_allowed);
int vibeos_devmgr_start(vibeos_devmgr_state_t *state);
int vibeos_devmgr_stop(vibeos_devmgr_state_t *state);
int vibeos_vfs_start(vibeos_vfs_state_t *state);
int vibeos_vfs_stop(vibeos_vfs_state_t *state);
int vibeos_net_start(vibeos_net_state_t *state);
int vibeos_net_stop(vibeos_net_state_t *state);
int vibeos_servicemgr_start(vibeos_servicemgr_state_t *mgr, vibeos_init_state_t *init_state, vibeos_devmgr_state_t *devmgr_state, vibeos_vfs_state_t *vfs_state, vibeos_net_state_t *net_state);
int vibeos_servicemgr_stop(vibeos_servicemgr_state_t *mgr, vibeos_init_state_t *init_state, vibeos_devmgr_state_t *devmgr_state, vibeos_vfs_state_t *vfs_state, vibeos_net_state_t *net_state);
int vibeos_servicemgr_health(const vibeos_servicemgr_state_t *mgr, const vibeos_init_state_t *init_state, const vibeos_devmgr_state_t *devmgr_state, const vibeos_vfs_state_t *vfs_state, const vibeos_net_state_t *net_state, uint32_t *out_running_services);
int vibeos_servicemgr_set_restart_budget(vibeos_servicemgr_state_t *mgr, uint32_t budget);
int vibeos_servicemgr_report_service_failure(vibeos_servicemgr_state_t *mgr);
int vibeos_servicemgr_can_restart(const vibeos_servicemgr_state_t *mgr, uint32_t *out_allowed);

#endif
