#include "vibeos/drivers.h"

int vibeos_driver_framework_init(vibeos_driver_framework_t *fw) {
    if (!fw) {
        return -1;
    }
    fw->count = 0;
    return 0;
}

int vibeos_driver_register(vibeos_driver_framework_t *fw, uint32_t id) {
    uint32_t i;
    if (!fw || id == 0 || fw->count >= 64) {
        return -1;
    }
    for (i = 0; i < fw->count; i++) {
        if (fw->records[i].id == id && fw->records[i].state == VIBEOS_DRIVER_LOADED) {
            return -1;
        }
    }
    fw->records[fw->count].id = id;
    fw->records[fw->count].state = VIBEOS_DRIVER_LOADED;
    fw->count++;
    return 0;
}

int vibeos_driver_unregister(vibeos_driver_framework_t *fw, uint32_t id) {
    uint32_t i;
    if (!fw || id == 0) {
        return -1;
    }
    for (i = 0; i < fw->count; i++) {
        if (fw->records[i].id == id && fw->records[i].state == VIBEOS_DRIVER_LOADED) {
            fw->records[i] = fw->records[fw->count - 1];
            fw->count--;
            return 0;
        }
    }
    return -1;
}

int vibeos_driver_state(const vibeos_driver_framework_t *fw, uint32_t id, vibeos_driver_state_t *out_state) {
    uint32_t i;
    if (!fw || !out_state || id == 0) {
        return -1;
    }
    for (i = 0; i < fw->count; i++) {
        if (fw->records[i].id == id) {
            *out_state = fw->records[i].state;
            return 0;
        }
    }
    return -1;
}
