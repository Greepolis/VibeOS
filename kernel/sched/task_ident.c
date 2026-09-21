#include "vibeos/task_ident.h"

void vibeos_task_identity_reset(vibeos_task_t *t) {
    unsigned char *raw = (unsigned char *)t;
    uint32_t i;

    if (!t) {
        return;
    }
    /* The whole object, byte by byte, rather than field by field: a field added to
     * the structure is then cleared without anybody remembering to say so, which is
     * the failure this function exists to end - and padding goes with it. */
    for (i = 0; i < (uint32_t)sizeof(*t); i++) {
        raw[i] = 0;
    }
}

int vibeos_task_is_group_leader(const vibeos_task_t *t) {
    return t && t->pid == t->tgid;
}

int vibeos_task_accountable_to(const vibeos_task_t *t, uint32_t parent_pid) {
    if (!t) {
        return 0;
    }
    if (t->ppid == parent_pid) {
        return 1;
    }
    return t->tgid == parent_pid && t->is_thread;
}

void vibeos_task_set_comm(vibeos_task_t *t, const char *src, uint32_t n) {
    uint32_t i;

    if (!t || !src) {
        return;
    }
    if (n > (uint32_t)sizeof(t->comm) - 1u) {
        n = (uint32_t)sizeof(t->comm) - 1u;
    }
    for (i = 0; i < n; i++) {
        t->comm[i] = src[i];
        if (src[i] == 0) {
            break;
        }
    }
    for (; i < (uint32_t)sizeof(t->comm); i++) {
        t->comm[i] = 0;
    }
}
