/* splice, tee and vmsplice (docs/abi/ L7 step 5): moving bytes into, out of
 * and between pipes without a trip through the program.
 *
 * Linux moves page references; this kernel's pipe is a ring of bytes, so it
 * copies through a kernel page - which a program cannot tell from the other,
 * except by speed. What it can tell is what happens when the far side takes
 * less than was offered, and the answer here is Linux's: nothing is lost. The
 * bytes are looked at in the pipe (vibeos_pipe_peek), written, and only as
 * many as were written are taken.
 *
 * SPLICE_F_NONBLOCK makes the pipe side not wait, whatever the descriptor's
 * own O_NONBLOCK says; the file side waits as it always would. */

#include "linux_internal.h"

static int splice_kcopy(void *dst, const void *src, uint64_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;

    while (n-- > 0u) {
        *d++ = *s++;
    }
    return 0;
}

static int is_pipe(const vibeos_file_t *f) {
    return f->ops == &vibeos_fops_pipe;
}

typedef struct {
    vibeos_file_t *in;        /* a pipe's read end */
    vibeos_file_t *out;       /* a pipe's write end, for tee and pipe to pipe */
    int nonblock;
} splice_wait_t;

/* Ready when the input pipe has bytes or can never have more, and - when the
 * output is a pipe - when there is room in it or nobody to read it. */
static long splice_look(void *ctx) {
    splice_wait_t *w = (splice_wait_t *)ctx;
    int ready_in = 1, ready_out = 1;

    if (w->in) {
        ready_in = vibeos_pipe_pending(w->in->pipe) > 0u || vibeos_pipe_writers(w->in->pipe) == 0u;
    }
    if (w->out) {
        ready_out = vibeos_pipe_pending(w->out->pipe) < vibeos_pipe_capacity(w->out->pipe) ||
                    vibeos_pipe_readers(w->out->pipe) == 0u;
    }
    if (ready_in && ready_out) {
        return 1;
    }
    return w->nonblock ? -VIBEOS_EAGAIN : 0;
}

static long splice_wait(vibeos_file_t *in, vibeos_file_t *out, int nonblock) {
    splice_wait_t w;
    long r;

    w.in = in;
    w.out = out;
    w.nonblock = nonblock;
    r = linux_wait_ready(splice_look, &w, -1, 0);
    return r == 1 ? 0 : r;
}

/* From a pipe to anything that writes: look, write, take what was written - a
 * page at a time, for as long as the pipe has bytes, the call has length left
 * and the far side takes all it is given. Linux moves everything the pipe holds
 * up to `len` in one call, and a program that splices exactly what it put in
 * (LTP's vmsplice01, 64 KiB at a time) counts on it. Only the first look waits. */
static long splice_from_pipe(vibeos_file_t *in, vibeos_file_t *out, uint64_t *off, uint64_t len,
                             int nonblock) {
    uint8_t *page;
    vibeos_pipe_status_t st;
    uint64_t done = 0;
    long r;

    if ((r = splice_wait(in, is_pipe(out) ? out : 0, nonblock)) != 0) {
        return r;
    }
    if (!(page = (uint8_t *)ks_page_alloc())) {
        return -VIBEOS_ENOMEM;
    }
    while (done < len) {
        uint64_t want = len - done > 4096u ? 4096u : len - done;
        long n, put;

        if (is_pipe(out)) {
            /* Into a pipe, no more than it has room for: its write must not
             * wait with bytes this call has already looked at. */
            uint64_t room = vibeos_pipe_room(out->pipe);   /* one look: M-111 */

            if (room == 0u) {
                break;
            }
            if (want > room) {
                want = room;
            }
        }
        n = vibeos_pipe_peek(in->pipe, 0, page, want, splice_kcopy, &st);
        if (n == 0) {
            break;   /* empty, or end of file: every writer has gone */
        }
        if (off) {
            put = out->ops->pwrite ? out->ops->pwrite(out, (uint64_t)(uintptr_t)page, (uint64_t)n, *off)
                                   : -VIBEOS_EINVAL;
        } else {
            put = out->ops->write ? out->ops->write(out, (uint64_t)(uintptr_t)page, (uint64_t)n)
                                  : -VIBEOS_EINVAL;
        }
        if (put <= 0) {
            if (done == 0u) {
                done = 0;
                r = put;
            }
            break;
        }
        (void)vibeos_pipe_read(in->pipe, page, (uint64_t)put, splice_kcopy, &st);
        ks_wake_waiters();   /* a writer to the input may have room now */
        if (off) {
            *off += (uint64_t)put;
        }
        done += (uint64_t)put;
        if (put < n) {
            break;   /* the far side took less: stop where it stopped */
        }
    }
    ks_page_free(page, "splice bounce");
    return done > 0u ? (long)done : r;
}

/* Into a pipe from anything that reads, a page at a time and never more than
 * the pipe has room for, so its write never waits for what was already read.
 * From a file it goes on while the file has bytes; from anything else - a
 * socket - once, because a second read would wait for data the first call
 * was not asked to wait for. */
static long splice_to_pipe(vibeos_file_t *in, uint64_t *off, vibeos_file_t *out, uint64_t len,
                           int nonblock) {
    uint8_t *page;
    uint64_t done = 0;
    long r;

    if ((r = splice_wait(0, out, nonblock)) != 0) {
        return r;
    }
    if (vibeos_pipe_readers(out->pipe) == 0u) {
        (void)ks_signal_raise(ks_current(), VIBEOS_SIGPIPE);
        return -VIBEOS_EPIPE;
    }
    if (!(page = (uint8_t *)ks_page_alloc())) {
        return -VIBEOS_ENOMEM;
    }
    while (done < len) {
        uint64_t room = vibeos_pipe_room(out->pipe);   /* one look: M-111 */
        uint64_t want = len - done;
        long got, put;

        if (want > room) {
            want = room;
        }
        if (want > 4096u) {
            want = 4096u;   /* the bounce is one page */
        }
        if (want == 0u) {
            break;
        }
        if (off) {
            got = in->ops->pread ? in->ops->pread(in, (uint64_t)(uintptr_t)page, want, *off) : -VIBEOS_EINVAL;
        } else {
            got = in->ops->read ? in->ops->read(in, (uint64_t)(uintptr_t)page, want) : -VIBEOS_EINVAL;
        }
        if (got <= 0) {
            if (done == 0u) {
                r = got;
            }
            break;
        }
        put = out->ops->write(out, (uint64_t)(uintptr_t)page, (uint64_t)got);
        if (put <= 0) {
            if (done == 0u) {
                r = put;
            }
            break;
        }
        if (off) {
            *off += (uint64_t)put;
        }
        done += (uint64_t)put;
        if (put < got || (uint64_t)got < want || in->ops != &vibeos_fops_regular) {
            break;
        }
    }
    ks_page_free(page, "splice bounce");
    return done > 0u ? (long)done : r;
}

static long linux_sys_splice(uint64_t fd_in, uint64_t off_in_uptr, uint64_t fd_out, uint64_t off_out_uptr,
                             uint64_t len, uint64_t flags) {
    vibeos_file_t *in, *out;
    uint64_t off = 0;
    int nonblock;
    long r = 0;

    if (flags & ~(uint64_t)LINUX_SPLICE_F_KNOWN) {
        return -VIBEOS_EINVAL;
    }
    in = linux_file_get(fd_in);
    out = linux_file_get(fd_out);
    if (!in || !out || (in->flags & VIBEOS_O_ACCMODE) == VIBEOS_O_WRONLY ||
        (out->flags & VIBEOS_O_ACCMODE) == VIBEOS_O_RDONLY) {
        r = -VIBEOS_EBADF;
    } else if (!is_pipe(in) && !is_pipe(out)) {
        r = -VIBEOS_EINVAL;   /* one side has to be a pipe */
    } else if (is_pipe(in) && is_pipe(out)) {
        if (off_in_uptr != 0u || off_out_uptr != 0u) {
            r = -VIBEOS_ESPIPE;
        } else if (in->pipe == out->pipe) {
            r = -VIBEOS_EINVAL;
        }
    } else if (is_pipe(in) ? off_in_uptr != 0u : off_out_uptr != 0u) {
        r = -VIBEOS_ESPIPE;   /* a pipe has no position to start from */
    } else if (!is_pipe(out) && (out->flags & VIBEOS_O_APPEND)) {
        r = -VIBEOS_EINVAL;
    }
    if (r == 0 && (is_pipe(in) ? off_out_uptr : off_in_uptr) != 0u) {
        uint64_t u = is_pipe(in) ? off_out_uptr : off_in_uptr;
        vibeos_file_t *side = is_pipe(in) ? out : in;

        if (side->ops != &vibeos_fops_regular) {
            r = -VIBEOS_ESPIPE;   /* only a file has offsets */
        } else if (linux_ipc_in(&off, u, 8u) != 0) {
            r = -VIBEOS_EFAULT;
        } else if ((int64_t)off < 0) {
            r = -VIBEOS_EINVAL;
        }
    }
    if (r == 0 && len != 0u) {
        const int has_off = (is_pipe(in) ? off_out_uptr : off_in_uptr) != 0u;

        nonblock = (flags & LINUX_SPLICE_F_NONBLOCK) != 0u;
        if (is_pipe(in)) {
            nonblock |= (in->flags & VIBEOS_O_NONBLOCK) != 0u;
            r = splice_from_pipe(in, out, has_off ? &off : 0, len, nonblock);
        } else {
            nonblock |= (out->flags & VIBEOS_O_NONBLOCK) != 0u;
            r = splice_to_pipe(in, has_off ? &off : 0, out, len, nonblock);
        }
        if (r >= 0 && has_off && linux_ipc_out(is_pipe(in) ? off_out_uptr : off_in_uptr, &off, 8u) != 0) {
            r = -VIBEOS_EFAULT;
        }
    }
    if (in) {
        vibeos_file_put(in);
    }
    if (out) {
        vibeos_file_put(out);
    }
    return r;
}

/* tee: the bytes of one pipe copied into another, and left where they were. */
static long linux_sys_tee(uint64_t fd_in, uint64_t fd_out, uint64_t len, uint64_t flags) {
    vibeos_file_t *in, *out;
    vibeos_pipe_status_t st;
    uint8_t *page = 0;
    uint64_t room;
    long r = 0, n;

    if (flags & ~(uint64_t)LINUX_SPLICE_F_KNOWN) {
        return -VIBEOS_EINVAL;
    }
    in = linux_file_get(fd_in);
    out = linux_file_get(fd_out);
    if (!in || !out || (in->flags & VIBEOS_O_ACCMODE) == VIBEOS_O_WRONLY ||
        (out->flags & VIBEOS_O_ACCMODE) == VIBEOS_O_RDONLY) {
        r = -VIBEOS_EBADF;
    } else if (!is_pipe(in) || !is_pipe(out) || in->pipe == out->pipe) {
        r = -VIBEOS_EINVAL;
    }
    if (r == 0 && len != 0u) {
        r = splice_wait(in, out, (flags & LINUX_SPLICE_F_NONBLOCK) != 0u ||
                                 ((in->flags | out->flags) & VIBEOS_O_NONBLOCK) != 0u);
    }
    if (r == 0 && len != 0u && !(page = (uint8_t *)ks_page_alloc())) {
        r = -VIBEOS_ENOMEM;
    }
    if (page) {
        /* A page at a time, each looked at further into the input, as much as
         * the input holds, the output has room for and the call asked for. */
        uint64_t done = 0;

        room = vibeos_pipe_room(out->pipe);   /* one look: M-111 */
        if (len > room) {
            len = room;
        }
        while (done < len) {
            long put;

            n = vibeos_pipe_peek(in->pipe, done, page, len - done > 4096u ? 4096u : len - done, splice_kcopy, &st);
            if (n <= 0) {
                break;
            }
            put = out->ops->write(out, (uint64_t)(uintptr_t)page, (uint64_t)n);
            if (put <= 0) {
                if (done == 0u) {
                    done = 0;
                    r = put;
                }
                break;
            }
            done += (uint64_t)put;
            if (put < n) {
                break;
            }
        }
        r = done > 0u ? (long)done : r;
        ks_page_free(page, "tee bounce");
    }
    if (in) {
        vibeos_file_put(in);
    }
    if (out) {
        vibeos_file_put(out);
    }
    return r;
}

/* Memory into a pipe as Linux's vmsplice puts it: once there is room, as much
 * as fits, and no more - a short count rather than a wait for the rest, which
 * writev's would be. A program that offers 128 KiB to a pipe it will empty
 * itself (LTP's vmsplice01) is told what went in and drains it; waiting for
 * all of it would wait for ever. */
static long vmsplice_in(vibeos_file_t *f, uint64_t iov_uptr, uint64_t nr, int nonblock) {
    uint64_t i, total = 0;
    long r;

    if (nr > 1024u) {
        return -VIBEOS_EINVAL;   /* UIO_MAXIOV, as writev */
    }
    if ((r = splice_wait(0, f, nonblock || (f->flags & VIBEOS_O_NONBLOCK) != 0u)) != 0) {
        return r;
    }
    for (i = 0; i < nr; i++) {
        linux_iovec_t v;
        uint64_t room = vibeos_pipe_room(f->pipe);   /* one look: M-111 */
        long n;

        if (vibeos_uaccess_copy(&v, (const void *)(uintptr_t)(iov_uptr + i * sizeof(v)), sizeof(v)) != 0) {
            return total > 0u ? (long)total : -VIBEOS_EFAULT;
        }
        if (v.iov_len == 0u) {
            continue;
        }
        if (room == 0u) {
            break;
        }
        if (v.iov_len > room) {
            v.iov_len = room;
        }
        if (!linux_user_ok(v.iov_base, v.iov_len, 0)) {
            return total > 0u ? (long)total : -VIBEOS_EFAULT;
        }
        n = f->ops->write(f, v.iov_base, v.iov_len);
        if (n <= 0) {
            return total > 0u ? (long)total : n;
        }
        total += (uint64_t)n;
        if ((uint64_t)n < v.iov_len) {
            break;
        }
    }
    return (long)total;
}

/* vmsplice: a program's memory into a pipe, or a pipe into it. This kernel's
 * pipe holds bytes, not pages, so out of the pipe it is readv, and into it
 * vmsplice_in - what fits, once there is room - with SPLICE_F_NONBLOCK
 * answering for the pipe end's O_NONBLOCK in both. SPLICE_F_GIFT
 * gives Linux pages to keep; here the bytes are copied and the pages stay the
 * program's, which is what a gift that was not taken looks like. */
static long linux_sys_vmsplice(uint64_t fd, uint64_t iov_uptr, uint64_t nr, uint64_t flags) {
    vibeos_file_t *f;
    long r = 0;
    int write;

    if (flags & ~(uint64_t)LINUX_SPLICE_F_KNOWN) {
        return -VIBEOS_EINVAL;
    }
    if (!(f = linux_file_get(fd))) {
        return -VIBEOS_EBADF;
    }
    if (!is_pipe(f)) {
        r = -VIBEOS_EBADF;
    }
    write = f->pipe_write;
    if (r == 0 && (flags & LINUX_SPLICE_F_NONBLOCK)) {
        splice_wait_t w;

        w.in = write ? 0 : f;
        w.out = write ? f : 0;
        w.nonblock = 1;
        r = splice_look(&w) == 1 ? 0 : -VIBEOS_EAGAIN;
    }
    if (r != 0 || nr == 0u || !write) {
        vibeos_file_put(f);
        return (r != 0 || nr == 0u) ? r : linux_vec_io(fd, iov_uptr, nr, 0);
    }
    r = vmsplice_in(f, iov_uptr, nr, (flags & LINUX_SPLICE_F_NONBLOCK) != 0u);
    vibeos_file_put(f);
    return r;
}

/* ---- the syscalls this file implements --------------------------------------- */
#define LINUX_SPLICE_SYSCALLS(X) \
    X(275, splice,   SPLICE, NOPTR, linux_sys_splice(ARG(0), ARG(1), ARG(2), ARG(3), ARG(4), ARG(5))) \
    X(276, tee,      SPLICE, NOPTR, linux_sys_tee(ARG(0), ARG(1), ARG(2), ARG(3))) \
    X(278, vmsplice, SPLICE, PTRS(IN_VEC(1, 2, 16, 1024)), linux_sys_vmsplice(ARG(0), ARG(1), ARG(2), ARG(3)))

LINUX_DEFINE_SYSCALLS(splice, LINUX_SPLICE_SYSCALLS)
