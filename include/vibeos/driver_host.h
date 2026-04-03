#ifndef VIBEOS_DRIVER_HOST_H
#define VIBEOS_DRIVER_HOST_H

#include <stdint.h>

struct vibeos_driver_framework;
struct vibeos_devmgr_state;

int vibeos_driver_host_probe(struct vibeos_driver_framework *fw, struct vibeos_devmgr_state *devmgr_state, uint32_t driver_id);

#endif
