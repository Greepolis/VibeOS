#include "vibeos/fdtable.h"

void vibeos_fd_clear(vibeos_fd_t *f) {
    unsigned char *raw = (unsigned char *)f;
    uint32_t i;

    if (!f) {
        return;
    }
    for (i = 0; i < (uint32_t)sizeof(*f); i++) {
        raw[i] = 0;
    }
    f->pipe = -1;
    f->net_sock = -1;
}

void vibeos_fdtable_reset(vibeos_fdtable_t *t) {
    uint32_t i;

    if (!t) {
        return;
    }
    for (i = 0; i < VIBEOS_FD_SLOTS; i++) {
        vibeos_fd_clear(&t->fds[i]);
    }
    for (i = 0; i < VIBEOS_FD_STD; i++) {
        vibeos_fd_clear(&t->std[i]);
    }
}

vibeos_fd_t *vibeos_fdtable_get(vibeos_fdtable_t *t, uint64_t fd) {
    vibeos_fd_t *f;

    if (!t || fd < VIBEOS_FD_FIRST || fd >= VIBEOS_FD_FIRST + VIBEOS_FD_SLOTS) {
        return 0;
    }
    f = &t->fds[fd - VIBEOS_FD_FIRST];
    return f->used ? f : 0;
}

vibeos_fd_t *vibeos_fdtable_redirect(vibeos_fdtable_t *t, uint64_t fd) {
    if (!t || fd >= VIBEOS_FD_STD) {
        return 0;
    }
    return t->std[fd].used ? &t->std[fd] : 0;
}

int vibeos_fdtable_free_index(const vibeos_fdtable_t *t) {
    uint32_t i;

    if (!t) {
        return -1;
    }
    for (i = 0; i < VIBEOS_FD_SLOTS; i++) {
        if (!t->fds[i].used) {
            return (int)i;
        }
    }
    return -1;
}

int vibeos_fdtable_claim(vibeos_fdtable_t *t) {
    int i = vibeos_fdtable_free_index(t);

    if (i < 0) {
        return -1;
    }
    vibeos_fd_clear(&t->fds[i]);
    t->fds[i].used = 1;
    return i;
}

void vibeos_fdtable_copy(vibeos_fdtable_t *dst, const vibeos_fdtable_t *src) {
    uint32_t i;

    if (!dst || !src) {
        return;
    }
    /* Entry by entry: the child's table is whatever the previous occupant of the
     * slot left, so it is overwritten, not added to. */
    for (i = 0; i < VIBEOS_FD_SLOTS; i++) {
        dst->fds[i] = src->fds[i];
    }
    for (i = 0; i < VIBEOS_FD_STD; i++) {
        dst->std[i] = src->std[i];
    }
}

uint32_t vibeos_fdtable_count(void) {
    return VIBEOS_FD_SLOTS + VIBEOS_FD_STD;
}

vibeos_fd_t *vibeos_fdtable_entry(vibeos_fdtable_t *t, uint32_t index) {
    if (!t || index >= VIBEOS_FD_SLOTS + VIBEOS_FD_STD) {
        return 0;
    }
    return index < VIBEOS_FD_SLOTS ? &t->fds[index] : &t->std[index - VIBEOS_FD_SLOTS];
}
