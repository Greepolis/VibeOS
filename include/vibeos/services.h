#ifndef VIBEOS_SERVICES_H
#define VIBEOS_SERVICES_H

#include <stdint.h>

typedef enum vibeos_service_state {
    VIBEOS_SERVICE_STOPPED = 0,
    VIBEOS_SERVICE_RUNNING = 1
} vibeos_service_state_t;

typedef struct vibeos_init_state {
    vibeos_service_state_t state;
    uint32_t started_services;
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

int vibeos_init_start(vibeos_init_state_t *state);
int vibeos_devmgr_start(vibeos_devmgr_state_t *state);
int vibeos_vfs_start(vibeos_vfs_state_t *state);
int vibeos_net_start(vibeos_net_state_t *state);

#endif
