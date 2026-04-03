#include "vibeos/driver_host.h"
#include "vibeos/drivers.h"
#include "vibeos/services.h"

int vibeos_driver_host_probe(vibeos_driver_framework_t *fw, vibeos_devmgr_state_t *devmgr_state, uint32_t driver_id) {
    if (!fw || !devmgr_state || devmgr_state->state != VIBEOS_SERVICE_RUNNING) {
        return -1;
    }
    if (vibeos_driver_register(fw, driver_id) != 0) {
        return -1;
    }
    devmgr_state->discovered_devices++;
    return 0;
}
