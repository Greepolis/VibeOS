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

/* Validate the pointer arguments a row declares, before its handler is entered.
 * Returns 0, or the negated error for the first range that is not valid - the
 * descriptor's own, or `efault`, which is the personality's word for a bad
 * address (EFAULT for Linux); the handler then
 * never runs, so nothing it does first (a lookup, a lock, a dequeue) has happened
 * to a call that was going to fail anyway. */
long vibeos_abi_check_pointers(const vibeos_row_t *row, const vibeos_call_t *call,
                               int (*user_ok)(uint64_t base, uint64_t len, int write),
                               long efault) {
    uint32_t i;

    for (i = 0; i < VIBEOS_PTR_MAX; i++) {
        const vibeos_ptr_t *d = &row->ptr[i];
        uint64_t len;

        if (!(d->flags & VIBEOS_PTR_LIVE)) {
            continue;
        }
        if (d->when_arg) {
            uint64_t v = call->a[d->when_arg - 1u];
            if (d->when_mask) {
                v &= d->when_mask;
            }
            if (v != d->when_val) {
                continue;
            }
        }
        if ((d->flags & VIBEOS_PTR_OPT) && call->a[d->arg] == 0u) {
            continue;
        }
        if (d->len_arg) {
            uint64_t n = call->a[d->len_arg - 1u];
            if (n == 0u || (d->cap && n > d->cap)) {
                continue;
            }
            len = n * (uint64_t)d->len;
        } else {
            len = d->len;
        }
        if (!user_ok(call->a[d->arg], len, (d->flags & VIBEOS_PTR_WRITE) ? 1 : 0)) {
            return d->err ? -(long)d->err : -efault;
        }
    }
    return 0;
}
