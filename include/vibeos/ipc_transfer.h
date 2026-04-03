#ifndef VIBEOS_IPC_TRANSFER_H
#define VIBEOS_IPC_TRANSFER_H

#include <stdint.h>

#include "vibeos/object.h"

int vibeos_ipc_transfer_handle(
    const vibeos_handle_table_t *sender,
    vibeos_handle_table_t *receiver,
    uint32_t src_handle,
    uint32_t requested_rights,
    uint32_t *out_receiver_handle);

#endif
