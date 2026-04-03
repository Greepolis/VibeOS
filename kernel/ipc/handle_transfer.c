#include "vibeos/ipc_transfer.h"

int vibeos_ipc_transfer_handle(
    const vibeos_handle_table_t *sender,
    vibeos_handle_table_t *receiver,
    uint32_t src_handle,
    uint32_t requested_rights,
    uint32_t *out_receiver_handle) {
    uint32_t sender_rights = 0;
    uint32_t effective_rights;
    vibeos_object_type_t object_type = VIBEOS_OBJECT_NONE;
    uint32_t object_id = 0;

    if (!sender || !receiver || !out_receiver_handle || src_handle == 0 || requested_rights == 0) {
        return -1;
    }
    if (vibeos_handle_rights(sender, src_handle, &sender_rights) != 0) {
        return -1;
    }
    if ((sender_rights & requested_rights) != requested_rights) {
        return -1;
    }
    effective_rights = sender_rights & requested_rights;
    if (vibeos_handle_object(sender, src_handle, &object_type, &object_id) == 0 && object_type != VIBEOS_OBJECT_NONE && object_id != 0) {
        return vibeos_handle_alloc_object(receiver, effective_rights, object_type, object_id, out_receiver_handle);
    }
    return vibeos_handle_alloc(receiver, effective_rights, out_receiver_handle);
}
