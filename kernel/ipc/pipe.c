/* Pipes. See include/vibeos/pipe.h. */

#include "vibeos/pipe.h"
#include "vibeos/mbz.h"

typedef struct {
    int used;
    uint32_t readers;
    uint32_t writers;
    uint32_t head;      /* next byte to read  */
    uint32_t tail;      /* next byte to write */
    uint32_t count;     /* bytes currently held */
    uint8_t buf[VIBEOS_PIPE_BYTES];
} pipe_t;

static pipe_t g_pipes[VIBEOS_PIPE_MAX];
static void (*g_lock)(void);
static void (*g_unlock)(void);

static void lock(void) {
    if (g_lock) {
        g_lock();
    }
}

static void unlock(void) {
    if (g_unlock) {
        g_unlock();
    }
}

void vibeos_pipe_set_lock(void (*l)(void), void (*u)(void)) {
    g_lock = l;
    g_unlock = u;
}

void vibeos_pipe_reset(void) {
    uint32_t i;
    unsigned char *raw;
    uint32_t k;

    lock();
    for (i = 0; i < VIBEOS_PIPE_MAX; i++) {
        raw = (unsigned char *)&g_pipes[i];
        for (k = 0; k < (uint32_t)sizeof(g_pipes[i]); k++) {
            raw[k] = 0;
        }
    }
    unlock();
}

/* A slot the caller names must exist and be in use. Anything else is a descriptor
 * that outlived its pipe or was never given one - counted, because clamping it away
 * is how a wrong count stays invisible until a pipeline hangs. Called under the lock. */
static pipe_t *pipe_at(int slot) {
    if (slot < 0 || (uint32_t)slot >= VIBEOS_PIPE_MAX || !g_pipes[slot].used) {
        vibeos_mbz_hit(VIBEOS_MBZ_PIPE_BAD_SLOT, (uint64_t)(int64_t)slot);
        return 0;
    }
    return &g_pipes[slot];
}

int vibeos_pipe_create(void) {
    uint32_t i;
    int slot = -1;

    lock();
    for (i = 0; i < VIBEOS_PIPE_MAX; i++) {
        if (!g_pipes[i].used) {
            g_pipes[i].used = 1;
            g_pipes[i].readers = 1;
            g_pipes[i].writers = 1;
            g_pipes[i].head = 0;
            g_pipes[i].tail = 0;
            g_pipes[i].count = 0;
            slot = (int)i;
            break;
        }
    }
    unlock();
    return slot;
}

void vibeos_pipe_abandon(int slot) {
    pipe_t *p;

    lock();
    p = pipe_at(slot);
    if (p) {
        p->used = 0;
        p->count = 0;
        p->head = 0;
        p->tail = 0;
    }
    unlock();
}

void vibeos_pipe_add_end(int slot, int writable) {
    pipe_t *p;

    lock();
    p = pipe_at(slot);
    if (p) {
        if (writable) {
            p->writers++;
        } else {
            p->readers++;
        }
    }
    unlock();
}

void vibeos_pipe_release_end(int slot, int writable) {
    pipe_t *p;

    lock();
    p = pipe_at(slot);
    if (p) {
        uint32_t *n = writable ? &p->writers : &p->readers;
        if (*n == 0u) {
            /* Nothing to release: a descriptor gave back an end twice, or a copy of
             * it was made without acquiring. The old code clamped this at zero. */
            vibeos_mbz_hit(VIBEOS_MBZ_PIPE_END_UNDERFLOW, (uint64_t)slot);
        } else {
            (*n)--;
        }
        if (p->readers == 0u && p->writers == 0u) {
            p->used = 0;
            p->count = 0;
            p->head = 0;
            p->tail = 0;
        }
    }
    unlock();
}

void vibeos_pipe_end_acquire(const vibeos_fd_t *f) {
    if (f && f->used && f->pipe >= 0) {
        vibeos_pipe_add_end(f->pipe, f->writable);
    }
}

void vibeos_pipe_end_release(vibeos_fd_t *f) {
    if (!f || f->pipe < 0) {
        return;
    }
    vibeos_pipe_release_end(f->pipe, f->writable);
    f->pipe = -1;
}

long vibeos_pipe_read(int slot, void *dst, uint64_t len, vibeos_pipe_copy_fn copy,
                      vibeos_pipe_status_t *status) {
    pipe_t *p;
    uint64_t copied = 0;
    int faulted = 0;
    vibeos_pipe_status_t st = VIBEOS_PIPE_OK;

    lock();
    p = pipe_at(slot);
    if (!p) {
        unlock();
        if (status) {
            *status = VIBEOS_PIPE_FAULT;
        }
        return 0;
    }
    /* In contiguous runs of the ring, each through the caller's copy, and consumed
     * only once copied: the caller may have slept with its buffer validated, and a
     * sibling can have unmapped it since (H-010). */
    while (copied < len && p->count > 0u) {
        uint64_t run = VIBEOS_PIPE_BYTES - p->head;
        if (run > p->count) {
            run = p->count;
        }
        if (run > len - copied) {
            run = len - copied;
        }
        if (copy((uint8_t *)dst + copied, &p->buf[p->head], run) != 0) {
            faulted = 1;
            break;
        }
        copied += run;
        p->head = (uint32_t)((p->head + run) % VIBEOS_PIPE_BYTES);
        p->count -= (uint32_t)run;
    }
    /* End of file is decided here, in the same critical section that found the buffer
     * empty (M-003, the read half): decided afterwards, a writer could enqueue and
     * close in between and the reader would report end-of-file with those bytes still
     * in the buffer - the shape of `ls | wc -l` in the boot script. */
    if (copied == 0u) {
        if (faulted) {
            st = VIBEOS_PIPE_FAULT;
        } else if (p->writers == 0u) {
            st = VIBEOS_PIPE_EOF;
        } else {
            st = VIBEOS_PIPE_EMPTY;
        }
    }
    unlock();
    if (status) {
        *status = st;
    }
    return (long)copied;
}

long vibeos_pipe_write(int slot, const void *src, uint64_t len, vibeos_pipe_copy_fn copy,
                       vibeos_pipe_status_t *status) {
    pipe_t *p;
    uint64_t written = 0;
    vibeos_pipe_status_t st = VIBEOS_PIPE_OK;

    lock();
    p = pipe_at(slot);
    if (!p) {
        unlock();
        if (status) {
            *status = VIBEOS_PIPE_FAULT;
        }
        return 0;
    }
    /* Tested under the lock that enqueues (M-003, the write half): tested before
     * taking it, the last reader could close in between and the bytes were accepted for
     * nobody, with no signal. */
    if (p->readers == 0u) {
        unlock();
        if (status) {
            *status = VIBEOS_PIPE_NO_READER;
        }
        return 0;
    }
    while (written < len && p->count < VIBEOS_PIPE_BYTES) {
        uint64_t room = VIBEOS_PIPE_BYTES - p->count;
        uint64_t run = VIBEOS_PIPE_BYTES - p->tail;
        if (run > room) {
            run = room;
        }
        if (run > len - written) {
            run = len - written;
        }
        if (copy(&p->buf[p->tail], (const uint8_t *)src + written, run) != 0) {
            st = VIBEOS_PIPE_FAULT;
            break;
        }
        written += run;
        p->tail = (uint32_t)((p->tail + run) % VIBEOS_PIPE_BYTES);
        p->count += (uint32_t)run;
    }
    if (st == VIBEOS_PIPE_OK && written == 0u && len > 0u) {
        st = VIBEOS_PIPE_FULL;
    }
    /* Bytes that did move are reported as moved; the fault is reported only when
     * nothing did, matching what a partial write is on Linux. */
    if (written > 0u) {
        st = VIBEOS_PIPE_OK;
    }
    unlock();
    if (status) {
        *status = st;
    }
    return (long)written;
}

uint32_t vibeos_pipe_in_use(void) {
    uint32_t i, n = 0;

    lock();
    for (i = 0; i < VIBEOS_PIPE_MAX; i++) {
        if (g_pipes[i].used) {
            n++;
        }
    }
    unlock();
    return n;
}

uint32_t vibeos_pipe_readers(int slot) {
    uint32_t n = 0;

    lock();
    if (slot >= 0 && (uint32_t)slot < VIBEOS_PIPE_MAX && g_pipes[slot].used) {
        n = g_pipes[slot].readers;
    }
    unlock();
    return n;
}

uint32_t vibeos_pipe_writers(int slot) {
    uint32_t n = 0;

    lock();
    if (slot >= 0 && (uint32_t)slot < VIBEOS_PIPE_MAX && g_pipes[slot].used) {
        n = g_pipes[slot].writers;
    }
    unlock();
    return n;
}
