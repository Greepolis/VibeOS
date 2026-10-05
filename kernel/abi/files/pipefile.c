/* A pipe end as a file (docs/abi/ A3).
 *
 * The ring and its end counts are kernel/ipc/pipe.c; this is what a descriptor
 * naming one end does - wait for data or room, stop waiting for a signal, raise
 * SIGPIPE - moved from the Linux read and write handlers. Each end is its own
 * description, holding its end of the pipe for as long as any descriptor names
 * it: dup and fork no longer touch the pipe's counts, and the end is given back
 * exactly once, by the description's release. */

#include "files_internal.h"

static long pipe_read(vibeos_file_t *f, uint64_t buf, uint64_t len) {
    if (f->pipe_write) {
        return -VIBEOS_EBADF;
    }
    for (;;) {
        vibeos_pipe_status_t st;
        long n = vibeos_pipe_read(f->pipe, (void *)(uintptr_t)buf, len, vibeos_uaccess_copy, &st);

        if (n > 0) {
            ks_wake_waiters();   /* a blocked writer may now have room */
            return n;
        }
        if (st == VIBEOS_PIPE_FAULT) {
            return -VIBEOS_EFAULT;
        }
        if (st == VIBEOS_PIPE_EOF) {
            return 0;   /* end of file: empty, and nobody can ever write again */
        }
        if (f->flags & VIBEOS_O_NONBLOCK) {
            return -VIBEOS_EAGAIN;
        }
        /* Nothing yet, and somebody could still write. Park instead of
         * spinning, so the writer actually gets a chance to run - unless a
         * signal needs acting on. This wait never marks the task BLOCKED, so
         * the timer returns it here each tick and the check cannot be missed. */
        if (ks_current() >= 0 && ks_signal_interrupts(ks_current())) {
            return -VIBEOS_RESTART_CALL;
        }
        ks_block_point();
    }
}

static long pipe_write(vibeos_file_t *f, uint64_t buf, uint64_t len) {
    uint64_t written = 0;

    if (!f->pipe_write) {
        return -VIBEOS_EBADF;
    }
    while (written < len) {
        vibeos_pipe_status_t st;
        long n = vibeos_pipe_write(f->pipe, (const void *)(uintptr_t)(buf + written), len - written,
                                   vibeos_uaccess_copy, &st);

        if (n > 0) {
            written += (uint64_t)n;
            ks_wake_waiters();   /* a blocked reader now has data */
            continue;
        }
        /* Writing into a pipe nobody will read. Linux raises SIGPIPE and returns
         * EPIPE; with no handler the default action ends the process, which is what
         * stops a pipeline from filling memory after its reader has gone. */
        if (st == VIBEOS_PIPE_NO_READER) {
            if (ks_current() >= 0) {
                (void)ks_signal_raise(ks_current(), VIBEOS_SIGPIPE);
            }
            return written > 0u ? (long)written : -VIBEOS_EPIPE;
        }
        if (st == VIBEOS_PIPE_FAULT) {
            return written > 0u ? (long)written : -VIBEOS_EFAULT;
        }
        if (f->flags & VIBEOS_O_NONBLOCK) {
            return written > 0u ? (long)written : -VIBEOS_EAGAIN;
        }
        /* Full, and a signal needs acting on: report what was written, or EINTR if
         * nothing was. Same shape as the read side, and the same reason the check
         * cannot be missed. */
        if (ks_current() >= 0 && ks_signal_interrupts(ks_current())) {
            return written > 0u ? (long)written : -VIBEOS_RESTART_CALL;
        }
        ks_block_point();
    }
    return (long)written;
}

/* A FIFO. It used to report a regular file of size zero, which is what the table
 * entry looked like to fstat when nothing said it was a pipe. */
static int pipe_stat(vibeos_file_t *f, vibeos_file_stat_t *out) {
    out->mode = VIBEOS_S_IFIFO | 0600u;
    out->size = 0;
    out->ino = 0x10000u + (uint64_t)(uint32_t)f->pipe;
    return 0;
}

/* The last descriptor naming this end has gone. The pipe itself lives until both
 * ends are gone, because a reader may still have data to drain after every
 * writer has closed - and somebody may be waiting for the data, the room or the
 * end of file that just became true. */
/* A read end is ready when there is something to read or nobody left to write
 * it - end of file is an answer, and a reader waiting for one must be told. A
 * write end is ready while there is room, and "ready" with nobody left to read:
 * the write will not wait, it will fail, which the caller has to find out. */
static uint32_t pipe_ready(vibeos_file_t *f) {
    uint32_t pending = vibeos_pipe_pending(f->pipe);

    if (f->pipe_write) {
        uint32_t r = pending < VIBEOS_PIPE_BYTES ? VIBEOS_READY_OUT : 0u;
        /* No reader: a write fails at once with EPIPE, which is an error to
         * poll (POLLERR, as Linux's pipe_poll says), not a hangup. */
        return vibeos_pipe_readers(f->pipe) == 0u ? (r | VIBEOS_READY_OUT | VIBEOS_READY_ERR) : r;
    }
    if (pending > 0u) {
        return VIBEOS_READY_IN;
    }
    /* Empty with no writer is a hangup and only that, as Linux's pipe_poll says
     * (LTP's poll03): a read returns 0 at once, which select counts as readable
     * because a hangup is in its read set - poll's caller asked and is told. */
    return vibeos_pipe_writers(f->pipe) == 0u ? VIBEOS_READY_HUP : 0u;
}

static void pipe_release(vibeos_file_t *f) {
    if (f->pipe < 0) {
        return;
    }
    vibeos_pipe_release_end(f->pipe, f->pipe_write);
    f->pipe = -1;
    ks_wake_waiters();
}

const vibeos_file_ops_t vibeos_fops_pipe = {
    .name = "pipe",
    .read = pipe_read,
    .write = pipe_write,
    .stat = pipe_stat,
    .release = pipe_release,
    .ready = pipe_ready,
};

int vibeos_open_pipe(uint32_t flags, vibeos_file_t **rd, vibeos_file_t **wr) {
    int slot = vibeos_pipe_create();   /* one reader, one writer */
    vibeos_file_t *r, *w;

    *rd = *wr = 0;
    if (slot < 0) {
        return -VIBEOS_EMFILE;
    }
    r = vibeos_file_alloc(&vibeos_fops_pipe, VIBEOS_O_RDONLY | (flags & VIBEOS_O_NONBLOCK));
    w = r ? vibeos_file_alloc(&vibeos_fops_pipe, VIBEOS_O_WRONLY | (flags & VIBEOS_O_NONBLOCK)) : 0;
    if (!r || !w) {
        /* Neither description owns an end yet, so neither release may give one
         * back: the pipe is abandoned whole. */
        if (r) {
            vibeos_file_put(r);
        }
        vibeos_pipe_abandon(slot);
        return -VIBEOS_ENFILE;
    }
    r->pipe = slot;
    r->pipe_write = 0;
    w->pipe = slot;
    w->pipe_write = 1;
    *rd = r;
    *wr = w;
    return 0;
}
