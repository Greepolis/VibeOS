#ifndef VIBEOS_DRIVERS_H
#define VIBEOS_DRIVERS_H

#include <stdint.h>

typedef enum vibeos_driver_state {
    VIBEOS_DRIVER_UNLOADED = 0,
    VIBEOS_DRIVER_LOADED = 1
} vibeos_driver_state_t;

typedef struct vibeos_driver_record {
    uint32_t id;
    vibeos_driver_state_t state;
} vibeos_driver_record_t;

typedef struct vibeos_driver_framework {
    vibeos_driver_record_t records[64];
    uint32_t count;
} vibeos_driver_framework_t;

int vibeos_driver_framework_init(vibeos_driver_framework_t *fw);
int vibeos_driver_register(vibeos_driver_framework_t *fw, uint32_t id);
int vibeos_driver_unregister(vibeos_driver_framework_t *fw, uint32_t id);
int vibeos_driver_state(const vibeos_driver_framework_t *fw, uint32_t id, vibeos_driver_state_t *out_state);

#endif
