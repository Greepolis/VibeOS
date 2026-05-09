#ifndef VIBEOS_DRIVERS_H
#define VIBEOS_DRIVERS_H

#include <stdint.h>

typedef enum vibeos_driver_state {
    VIBEOS_DRIVER_UNLOADED = 0,
    VIBEOS_DRIVER_LOADED = 1,
    VIBEOS_DRIVER_FAULTED = 2
} vibeos_driver_state_t;

typedef struct vibeos_driver_record {
    uint32_t id;
    uint16_t abi_major;
    uint16_t abi_minor;
    uint32_t isolation_caps;
    vibeos_driver_state_t state;
} vibeos_driver_record_t;

typedef struct vibeos_driver_framework {
    vibeos_driver_record_t records[64];
    uint32_t count;
    uint16_t required_abi_major;
    uint32_t faulted_total;
} vibeos_driver_framework_t;

int vibeos_driver_framework_init(vibeos_driver_framework_t *fw);
int vibeos_driver_framework_require_abi(vibeos_driver_framework_t *fw, uint16_t abi_major);
int vibeos_driver_register(vibeos_driver_framework_t *fw, uint32_t id);
int vibeos_driver_register_versioned(vibeos_driver_framework_t *fw, uint32_t id, uint16_t abi_major, uint16_t abi_minor, uint32_t isolation_caps);
int vibeos_driver_unregister(vibeos_driver_framework_t *fw, uint32_t id);
int vibeos_driver_state(const vibeos_driver_framework_t *fw, uint32_t id, vibeos_driver_state_t *out_state);
int vibeos_driver_mark_faulted(vibeos_driver_framework_t *fw, uint32_t id);
int vibeos_driver_count_state(const vibeos_driver_framework_t *fw, vibeos_driver_state_t state, uint32_t *out_count);

#endif
