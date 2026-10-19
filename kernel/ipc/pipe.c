/* Pipes. See include/vibeos/pipe.h.
 *
 * A pipe's ring is a list of pages, not one array: the allocator hands out a page
 * at a time, and a contiguous 64 KiB - let alone the megabyte F_SETPIPE_SZ may
 * ask for - is not something it promises. Byte i of the ring is byte i % 4096 of
 * page i / 4096, and every copy runs to the end of a page at most. The pages are
 * had before the lock is taken and given back after it is let go: the allocator
 * may have to reclaim, and nothing slow happens with interrupts masked. */

#include "vibeos/pipe.h"
#include "vibeos/mbz.h"

#define PIPE_PAGE 4096u

typedef struct {
    int used;
    uint32_t readers;
    uint32_t writers;
    uint32_t cap;       /* bytes the ring holds: pages * 4096, a power of two */
    uint32_t head;      /* next byte to read  */
    uint32_t tail;      /* next byte to write */
    uint32_t count;     /* bytes currently held */
    uint8_t *page[VIBEOS_PIPE_PAGES];
} pipe_t;

static pipe_t g_pipes[VIBEOS_PIPE_MAX];
static void (*g_lock)(void);
static void (*g_unlock)(void);
static void *(*g_page_alloc)(void);
static void (*g_page_free)(void *page);

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

void vibeos_pipe_set_pages(void *(*alloc)(void), void (*release)(void *page)) {
    g_page_alloc = alloc;
    g_page_free = release;
}

static void pages_free(uint8_t **pages, uint32_t n) {
    uint32_t i;

    for (i = 0; i < n; i++) {
        if (pages[i] && g_page_free) {
            g_page_free(pages[i]);
        }
        pages[i] = 0;
    }
}

/* `n` pages into `pages`, all or none. 0, or -1. */
static int pages_alloc(uint8_t **pages, uint32_t n) {
    uint32_t i;

    for (i = 0; i < n; i++) {
        pages[i] = g_page_alloc ? (uint8_t *)g_page_alloc() : 0;
        if (!pages[i]) {
            pages_free(pages, i);
            return -1;
        }
    }
    return 0;
}

/* Give a pipe's pages back. Under the lock: a release is quick - it is the
 * allocation, which may have to reclaim, that is kept outside. */
static void pipe_free_pages(pipe_t *p) {
    pages_free(p->page, p->cap / PIPE_PAGE);
    p->cap = 0;
}

void vibeos_pipe_reset(void) {
    uint32_t i;

    lock();
    for (i = 0; i < VIBEOS_PIPE_MAX; i++) {
        pipe_free_pages(&g_pipes[i]);
        g_pipes[i].used = 0;
        g_pipes[i].readers = g_pipes[i].writers = 0;
        g_pipes[i].head = g_pipes[i].tail = g_pipes[i].count = 0;
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
    uint8_t *pages[VIBEOS_PIPE_BYTES / PIPE_PAGE];
    const uint32_t n = VIBEOS_PIPE_BYTES / PIPE_PAGE;
    uint32_t i, k;
    int slot = -1;

    if (pages_alloc(pages, n) != 0) {
        return -2;
    }
    lock();
    for (i = 0; i < VIBEOS_PIPE_MAX; i++) {
        if (!g_pipes[i].used) {
            pipe_t *p = &g_pipes[i];

            p->used = 1;
            p->readers = 1;
            p->writers = 1;
            p->head = p->tail = p->count = 0;
            p->cap = VIBEOS_PIPE_BYTES;
            for (k = 0; k < n; k++) {
                p->page[k] = pages[k];
            }
            slot = (int)i;
            break;
        }
    }
    unlock();
    if (slot < 0) {
        pages_free(pages, n);
    }
    return slot;
}

/* The slot freed and its pages given back. Under the lock. */
static void pipe_retire(pipe_t *p) {
    p->used = 0;
    p->count = 0;
    p->head = 0;
    p->tail = 0;
    pipe_free_pages(p);
}

void vibeos_pipe_abandon(int slot) {
    pipe_t *p;

    lock();
    p = pipe_at(slot);
    if (p) {
        pipe_retire(p);
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
        uint32_t *c = writable ? &p->writers : &p->readers;
        if (*c == 0u) {
            /* Nothing to release: a descriptor gave back an end twice, or a copy of
             * it was made without acquiring. The old code clamped this at zero. */
            vibeos_mbz_hit(VIBEOS_MBZ_PIPE_END_UNDERFLOW, (uint64_t)slot);
        } else {
            (*c)--;
        }
        if (p->readers == 0u && p->writers == 0u) {
            pipe_retire(p);
        }
    }
    unlock();
}

/* The run of the ring that starts at `at` and lies in one page: its address and
 * how many bytes it has before the page ends. */
static uint8_t *ring_at(const pipe_t *p, uint32_t at, uint64_t *run) {
    uint32_t in_page = at % PIPE_PAGE;

    *run = PIPE_PAGE - in_page;
    return p->page[at / PIPE_PAGE] + in_page;
}

/* Copy up to `len` bytes from the ring, starting `skip` bytes past its front,
 * into dst. The bytes copied; *faulted if the copy refused. Under the lock. */
static uint64_t ring_out(const pipe_t *p, uint64_t skip, uint8_t *dst, uint64_t len,
                         vibeos_pipe_copy_fn copy, int *faulted) {
    uint64_t copied = 0;
    uint32_t at, left;

    if (skip >= p->count) {
        *faulted = 0;
        return 0;
    }
    at = (uint32_t)((p->head + skip) % p->cap);
    left = p->count - (uint32_t)skip;

    *faulted = 0;
    while (copied < len && left > 0u) {
        uint64_t run;
        uint8_t *src = ring_at(p, at, &run);

        if (run > left) {
            run = left;
        }
        if (run > len - copied) {
            run = len - copied;
        }
        if (copy(dst + copied, src, run) != 0) {
            *faulted = 1;
            break;
        }
        copied += run;
        at = (uint32_t)((at + run) % p->cap);
        left -= (uint32_t)run;
    }
    return copied;
}

long vibeos_pipe_read(int slot, void *dst, uint64_t len, vibeos_pipe_copy_fn copy,
                      vibeos_pipe_status_t *status) {
    pipe_t *p;
    uint64_t copied;
    int faulted;
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
    /* Copied first and consumed only once copied: the caller may have slept with
     * its buffer validated, and a sibling can have unmapped it since (H-010). */
    copied = ring_out(p, 0, (uint8_t *)dst, len, copy, &faulted);
    p->head = (uint32_t)((p->head + copied) % p->cap);
    p->count -= (uint32_t)copied;
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

/* As vibeos_pipe_read, but the bytes stay where they are: what tee copies, and
 * what splice looks at before it knows how much the other side will take
 * (docs/abi/ L7 step 5). Taking them afterwards is a read of the same count. */
long vibeos_pipe_peek(int slot, uint64_t skip, void *dst, uint64_t len, vibeos_pipe_copy_fn copy,
                      vibeos_pipe_status_t *status) {
    pipe_t *p;
    uint64_t copied;
    int faulted;
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
    copied = ring_out(p, skip, (uint8_t *)dst, len, copy, &faulted);
    if (copied == 0u) {
        st = faulted ? VIBEOS_PIPE_FAULT : p->writers == 0u ? VIBEOS_PIPE_EOF : VIBEOS_PIPE_EMPTY;
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
    while (written < len && p->count < p->cap) {
        uint64_t room = p->cap - p->count;
        uint64_t run;
        uint8_t *at = ring_at(p, p->tail, &run);

        if (run > room) {
            run = room;
        }
        if (run > len - written) {
            run = len - written;
        }
        if (copy(at, (const uint8_t *)src + written, run) != 0) {
            st = VIBEOS_PIPE_FAULT;
            break;
        }
        written += run;
        p->tail = (uint32_t)((p->tail + run) % p->cap);
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

uint32_t vibeos_pipe_capacity(int slot) {
    uint32_t n = 0;

    lock();
    if (slot >= 0 && (uint32_t)slot < VIBEOS_PIPE_MAX && g_pipes[slot].used) {
        n = g_pipes[slot].cap;
    }
    unlock();
    return n;
}

static int resize_copy(void *dst, const void *src, uint64_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;

    while (n-- > 0u) {
        *d++ = *s++;
    }
    return 0;
}

/* `n` pages, had before the lock, chained through each page's first word so
 * that no array of a megabyte's worth of pointers sits on a kernel stack: the
 * chain's head, or 0 with every page given back. */
static uint8_t *chain_alloc(uint32_t n) {
    uint8_t *head = 0;
    uint32_t i;

    for (i = 0; i < n; i++) {
        uint8_t *pg = g_page_alloc ? (uint8_t *)g_page_alloc() : 0;

        if (!pg) {
            while (head) {
                uint8_t *next = *(uint8_t **)head;
                g_page_free(head);
                head = next;
            }
            return 0;
        }
        *(uint8_t **)pg = head;
        head = pg;
    }
    return head;
}

static void chain_free(uint8_t *head) {
    while (head) {
        uint8_t *next = *(uint8_t **)head;
        g_page_free(head);
        head = next;
    }
}

long vibeos_pipe_resize(int slot, uint64_t bytes) {
    /* Under g_lock whenever they are used: the old ring's pages, and one page
     * of the bytes on their way from the old ring to the new. */
    static uint8_t *old[VIBEOS_PIPE_PAGES];
    static uint8_t bounce[PIPE_PAGE];
    uint32_t pages = 1, n_old, i, moved = 0, left;
    uint8_t *chain;
    pipe_t *p;
    long r;

    while ((uint64_t)pages * PIPE_PAGE < bytes && pages < VIBEOS_PIPE_PAGES * 2u) {
        pages *= 2u;   /* a power of two pages, as Linux's round_pipe_size */
    }
    if ((uint64_t)pages * PIPE_PAGE > VIBEOS_PIPE_MAX_BYTES || slot < 0 || (uint32_t)slot >= VIBEOS_PIPE_MAX) {
        return -3;
    }
    lock();
    p = pipe_at(slot);
    r = !p ? -3 : (p->cap == pages * PIPE_PAGE) ? (long)p->cap : 0;
    unlock();
    if (r != 0) {
        return r;   /* no pipe, or already that size */
    }
    if (!(chain = chain_alloc(pages))) {
        return -2;
    }
    /* Whether it fits is decided once, here, where nothing can change it before
     * the move: decided before the pages were had, a writer could fill it in
     * between. */
    lock();
    p = pipe_at(slot);
    if (!p || p->count > pages * PIPE_PAGE) {
        unlock();
        chain_free(chain);
        return !p ? -3 : -1;
    }
    /* The old ring out of the slot, the new one in, and the bytes held moved to
     * the start of the new one in order. */
    n_old = p->cap / PIPE_PAGE;
    for (i = 0; i < n_old; i++) {
        old[i] = p->page[i];
    }
    for (i = 0; i < pages; i++) {
        uint8_t *next = *(uint8_t **)chain;

        p->page[i] = chain;
        chain = next;
    }
    left = p->count;
    while (left > 0u) {
        uint32_t at = p->head % p->cap, in_page = at % PIPE_PAGE;
        uint32_t run = PIPE_PAGE - in_page;

        if (run > left) {
            run = left;
        }
        resize_copy(bounce, old[at / PIPE_PAGE] + in_page, run);
        {
            uint32_t done = 0;

            while (done < run) {
                uint32_t to = moved % PIPE_PAGE, room = PIPE_PAGE - to;
                uint32_t step = run - done < room ? run - done : room;

                resize_copy(p->page[moved / PIPE_PAGE] + to, bounce + done, step);
                done += step;
                moved += step;
            }
        }
        p->head = (p->head + run) % p->cap;
        left -= run;
    }
    p->cap = pages * PIPE_PAGE;
    p->head = 0;
    p->count = moved;
    p->tail = moved % p->cap;
    pages_free(old, n_old);
    unlock();
    return (long)pages * PIPE_PAGE;
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

uint32_t vibeos_pipe_pending(int slot) {
    uint32_t n = 0;

    lock();
    if (slot >= 0 && (uint32_t)slot < VIBEOS_PIPE_MAX && g_pipes[slot].used) {
        n = g_pipes[slot].count;
    }
    unlock();
    return n;
}

uint32_t vibeos_pipe_room(int slot) {
    uint32_t n = 0;

    lock();
    if (slot >= 0 && (uint32_t)slot < VIBEOS_PIPE_MAX && g_pipes[slot].used &&
        g_pipes[slot].cap > g_pipes[slot].count) {
        n = g_pipes[slot].cap - g_pipes[slot].count;
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
