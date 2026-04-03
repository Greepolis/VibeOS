#include "vibeos/drivers.h"

int vibeos_driver_framework_init(vibeos_driver_framework_t *fw) {
    if (!fw) {
        return -1;
    }
    fw->count = 0;
    return 0;
}

int vibeos_driver_register(vibeos_driver_framework_t *fw, uint32_t id) {
    if (!fw || fw->count >= 64) {
        return -1;
    }
    fw->records[fw->count].id = id;
    fw->records[fw->count].state = VIBEOS_DRIVER_LOADED;
    fw->count++;
    return 0;
}
