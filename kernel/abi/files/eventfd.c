/* An event counter as a file (docs/abi/ L4 step 2): Linux's eventfd, and what any
 * personality's "a count a waiter can wait on" would be - so it lives with the
 * file types, not in the Linux layer.
 *
 * The count lives in the description, so dup and fork share it as Linux's does,
 * and every change to it is a compare-exchange: two threads, or two processes
 * after a fork, can read and write it at the same instant, and a description
 * has no lock of its own. A read waits for a count that is not zero, a write
 * for room under the largest count, the way a pipe's do: look, give up the
 * core, look again, stop for a signal (pipefile.c). */

#include "files_internal.h"

/* The largest count; a write that would pass it waits, and the one value past
 * it is refused outright, as Linux's eventfd_write refuses ULLONG_MAX. */
#define EVENT_MAX 0xfffffffffffffffeull

static long event_read(vibeos_file_t *f, uint64_t buf, uint64_t len) {
    if (len < 8u) {
        return -VIBEOS_EINVAL;
    }
    for (;;) {
        uint64_t v = __atomic_load_n(&f->event_count, __ATOMIC_ACQUIRE), take;

        if (v != 0u) {
            take = (f->event_flags & VIBEOS_EVENT_SEMAPHORE) ? 1u : v;
            if (!__atomic_compare_exchange_n(&f->event_count, &v, v - take, 0, __ATOMIC_ACQ_REL,
                                             __ATOMIC_ACQUIRE)) {
                continue;   /* somebody else read or wrote meanwhile: look again */
            }
            ks_wake_waiters();   /* a writer may have room now */
            if (vibeos_uaccess_copy((void *)(uintptr_t)buf, &take, sizeof(take)) != 0) {
                /* Given back: the count is not lost to a buffer that could not
                 * take it. */
                __atomic_add_fetch(&f->event_count, take, __ATOMIC_ACQ_REL);
                return -VIBEOS_EFAULT;
            }
            return 8;
        }
        if (f->flags & VIBEOS_O_NONBLOCK) {
            return -VIBEOS_EAGAIN;
        }
        if (ks_current() >= 0 && ks_signal_interrupts(ks_current())) {
            return -VIBEOS_RESTART_CALL;
        }
        ks_block_point();
    }
}

static long event_write(vibeos_file_t *f, uint64_t buf, uint64_t len) {
    uint64_t add;

    if (len < 8u) {
        return -VIBEOS_EINVAL;
    }
    if (vibeos_uaccess_copy(&add, (const void *)(uintptr_t)buf, sizeof(add)) != 0) {
        return -VIBEOS_EFAULT;
    }
    if (add > EVENT_MAX) {
        return -VIBEOS_EINVAL;
    }
    for (;;) {
        uint64_t v = __atomic_load_n(&f->event_count, __ATOMIC_ACQUIRE);

        if (add <= EVENT_MAX - v) {
            if (!__atomic_compare_exchange_n(&f->event_count, &v, v + add, 0, __ATOMIC_ACQ_REL,
                                             __ATOMIC_ACQUIRE)) {
                continue;
            }
            ks_wake_waiters();   /* a reader may have something now */
            return 8;
        }
        if (f->flags & VIBEOS_O_NONBLOCK) {
            return -VIBEOS_EAGAIN;
        }
        if (ks_current() >= 0 && ks_signal_interrupts(ks_current())) {
            return -VIBEOS_RESTART_CALL;
        }
        ks_block_point();
    }
}

/* Readable while the count is not zero; writable while one more fits - as
 * Linux's eventfd_poll says it. */
static uint32_t event_ready(vibeos_file_t *f) {
    uint64_t v = __atomic_load_n(&f->event_count, __ATOMIC_ACQUIRE);

    return (v != 0u ? VIBEOS_READY_IN : 0u) | (v < EVENT_MAX ? VIBEOS_READY_OUT : 0u);
}

/* Linux's anonymous inode, as a pidfd's (pidfd.c). */
static int event_stat(vibeos_file_t *f, vibeos_file_stat_t *out) {
    (void)f;
    out->mode = VIBEOS_S_IFREG | 0600u;
    out->size = 0;
    return 0;
}

const vibeos_file_ops_t vibeos_fops_eventfd = {
    .name = "eventfd",
    .read = event_read,
    .write = event_write,
    .stat = event_stat,
    .ready = event_ready,
};

vibeos_file_t *vibeos_open_eventfd(uint64_t count, uint32_t event_flags, uint32_t flags) {
    vibeos_file_t *f = vibeos_file_alloc(&vibeos_fops_eventfd, flags);

    if (f) {
        f->event_count = count;
        f->event_flags = event_flags;
    }
    return f;
}
