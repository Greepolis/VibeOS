#include "vibeos/drivers.h"

static vibeos_driver_record_t *find_driver_record(vibeos_driver_framework_t *fw, uint32_t id) {
    uint32_t i;
    if (!fw || id == 0) {
        return 0;
    }
    for (i = 0; i < fw->count; i++) {
        if (fw->records[i].id == id) {
            return &fw->records[i];
        }
    }
    return 0;
}

int vibeos_driver_framework_init(vibeos_driver_framework_t *fw) {
    if (!fw) {
        return -1;
    }
    fw->count = 0;
    fw->required_abi_major = 1;
    fw->faulted_total = 0;
    return 0;
}

int vibeos_driver_framework_require_abi(vibeos_driver_framework_t *fw, uint16_t abi_major) {
    if (!fw || abi_major == 0) {
        return -1;
    }
    fw->required_abi_major = abi_major;
    return 0;
}

int vibeos_driver_register(vibeos_driver_framework_t *fw, uint32_t id) {
    return vibeos_driver_register_versioned(fw, id, 1, 0, 0);
}

int vibeos_driver_register_versioned(vibeos_driver_framework_t *fw, uint32_t id, uint16_t abi_major, uint16_t abi_minor, uint32_t isolation_caps) {
    uint32_t i;
    if (!fw || id == 0 || fw->count >= 64 || abi_major == 0) {
        return -1;
    }
    if (fw->required_abi_major != 0 && abi_major != fw->required_abi_major) {
        return -1;
    }
    for (i = 0; i < fw->count; i++) {
        if (fw->records[i].id == id && fw->records[i].state != VIBEOS_DRIVER_UNLOADED) {
            return -1;
        }
    }
    fw->records[fw->count].id = id;
    fw->records[fw->count].abi_major = abi_major;
    fw->records[fw->count].abi_minor = abi_minor;
    fw->records[fw->count].isolation_caps = isolation_caps;
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
        if (fw->records[i].id == id && fw->records[i].state != VIBEOS_DRIVER_UNLOADED) {
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

int vibeos_driver_mark_faulted(vibeos_driver_framework_t *fw, uint32_t id) {
    vibeos_driver_record_t *rec = find_driver_record(fw, id);
    if (!rec || rec->state != VIBEOS_DRIVER_LOADED) {
        return -1;
    }
    rec->state = VIBEOS_DRIVER_FAULTED;
    fw->faulted_total++;
    return 0;
}

int vibeos_driver_count_state(const vibeos_driver_framework_t *fw, vibeos_driver_state_t state, uint32_t *out_count) {
    uint32_t i;
    uint32_t count = 0;
    if (!fw || !out_count) {
        return -1;
    }
    for (i = 0; i < fw->count; i++) {
        if (fw->records[i].state == state) {
            count++;
        }
    }
    *out_count = count;
    return 0;
}
