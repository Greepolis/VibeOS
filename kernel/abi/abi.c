/* The kernel's syscall vocabulary: names and declared checks. See
 * include/vibeos/abi.h. Read-only tables generated from the one list there,
 * so this file has no state and nothing to keep in step by hand. The table is
 * const, so it needs no lock: nothing writes it after link time. */

#include "vibeos/abi.h"

static const vibeos_op_entry_t g_entries[VIBEOS_OP_COUNT] = {
#define VIBEOS_OP_ROW(id, name, checks) \
    [VIBEOS_OP_##id] = { VIBEOS_OP_##id, name, checks },
    VIBEOS_OP_LIST(VIBEOS_OP_ROW)
#undef VIBEOS_OP_ROW
};

const vibeos_op_entry_t *vibeos_op_entry(vibeos_op_id_t id) {
    if ((uint32_t)id == 0u || (uint32_t)id >= (uint32_t)VIBEOS_OP_COUNT) {
        return 0;
    }
    return &g_entries[id];
}
