#ifndef VIBEOS_PIPE_H
#define VIBEOS_PIPE_H

#include <stdint.h>

/* C5: pipes, as a module.
 *
 * A pipe is a ring of bytes and two counts - how many descriptors hold its read end,
 * how many its write end. The ends are counted, not flagged, because a descriptor can
 * be duplicated and inherited: `ls | wc` gives the write end to a child, and the
 * parent must close its own copy or the reader never sees end of file. That is the
 * classic way a shell pipeline hangs, and it is a refcount bug, not a pipe bug - which
 * is why the counts live here, behind one interface, and are checked: a release with
 * nothing to release is counted (VIBEOS_MBZ_PIPE_END_UNDERFLOW) instead of being
 * clamped away in silence.
 *
 * Since docs/abi/ A3 an end is held by an open file description, not a descriptor:
 * dup, dup2 and fork share the description and leave these counts alone, and the
 * description's release gives its end back. Five call sites that each had to
 * remember an acquire became none.
 *
 * This layer is portable. It does not sleep, raise signals or touch user memory: it
 * makes one attempt and says what happened, and the caller - which knows about the
 * scheduler and about signals - decides whether to wait, retry or fail. The copy to
 * and from user memory is the caller's function, passed in. Serialised by its *own*
 * lock, supplied by the architecture (vibeos_pipe_set_lock): "remember to hold the
 * lock" is not a property a compiler checks. */

/* How many pipes there may be, and how much one holds. Linux's are 64 KiB unless
 * a program asks for another size (F_SETPIPE_SZ), up to pipe-max-size, 1 MiB; this
 * pipe held a fixed 4 KiB until docs/abi/ L7, when LTP's splice tests wrote more
 * than that into a pipe before reading it and waited for ever. A pipe's ring is
 * pages, allocated when it is made or resized and never under the lock, so a pipe
 * nobody fills costs its sixteen pages and an unused slot costs nothing. */
#define VIBEOS_PIPE_MAX 64u
#define VIBEOS_PIPE_BYTES 65536u          /* a new pipe's capacity     */
#define VIBEOS_PIPE_MAX_BYTES 1048576u    /* the largest one may become */
#define VIBEOS_PIPE_PAGES (VIBEOS_PIPE_MAX_BYTES / 4096u)

typedef enum {
    VIBEOS_PIPE_OK = 0,      /* bytes were moved                                     */
    VIBEOS_PIPE_EMPTY,       /* read: nothing yet, and a writer could still write    */
    VIBEOS_PIPE_EOF,         /* read: empty and no writer can ever write again       */
    VIBEOS_PIPE_FULL,        /* write: no room yet, and a reader could still read    */
    VIBEOS_PIPE_NO_READER,   /* write: nobody will ever read (Linux: SIGPIPE, EPIPE) */
    VIBEOS_PIPE_FAULT        /* the copy callback refused (a user address went away) */
} vibeos_pipe_status_t;

/* Copy n bytes from src to dst; 0 on success. vibeos_uaccess_copy has this shape. */
typedef int (*vibeos_pipe_copy_fn)(void *dst, const void *src, uint64_t n);

/* The lock the layer serialises itself with. Registered once, before any pipe exists.
 * (A registration function, not a weak symbol: a weak definition in another object
 * does not resolve on the Windows build.) */
void vibeos_pipe_set_lock(void (*lock)(void), void (*unlock)(void));
/* Where a pipe's pages come from: one 4096-byte page a call, 0 when there is none.
 * Registered with the lock; a pipe cannot be made before both are. */
void vibeos_pipe_set_pages(void *(*alloc)(void), void (*release)(void *page));

void vibeos_pipe_reset(void);   /* boot and tests: every pipe free */

/* Create a pipe with one reader and one writer, VIBEOS_PIPE_BYTES long; its slot,
 * -1 when none is free, -2 when its pages could not be had. */
int vibeos_pipe_create(void);
/* Give back a pipe that was created and never handed to a descriptor. */
void vibeos_pipe_abandon(int slot);

/* One more / one fewer descriptor holds an end. `writable` selects which end. A
 * release that brings both counts to zero frees the pipe and drops its bytes: a reader
 * may still drain after every writer has closed, so it lives until both are gone. */
void vibeos_pipe_add_end(int slot, int writable);
void vibeos_pipe_release_end(int slot, int writable);

/* One attempt. Returns the bytes moved (0 with a status that says why), and never
 * blocks. A fault after some bytes moved returns those bytes with VIBEOS_PIPE_OK. */
long vibeos_pipe_read(int slot, void *dst, uint64_t len, vibeos_pipe_copy_fn copy,
                      vibeos_pipe_status_t *status);
long vibeos_pipe_write(int slot, const void *src, uint64_t len, vibeos_pipe_copy_fn copy,
                       vibeos_pipe_status_t *status);
/* vibeos_pipe_read without taking the bytes, from `skip` bytes past the front:
 * tee, a page at a time, and splice before it knows how much the other side will
 * take (docs/abi/ L7). */
long vibeos_pipe_peek(int slot, uint64_t skip, void *dst, uint64_t len, vibeos_pipe_copy_fn copy,
                      vibeos_pipe_status_t *status);

/* How much the pipe holds, and a new size for it: `bytes` rounded up to a power of
 * two pages, as Linux rounds it. The new size, or -1 if it is too small for what the
 * pipe holds now (Linux's EBUSY), -2 if the pages could not be had, -3 for a size
 * past VIBEOS_PIPE_MAX_BYTES or a slot that is no pipe. */
uint32_t vibeos_pipe_capacity(int slot);
long vibeos_pipe_resize(int slot, uint64_t bytes);

/* How many pipes exist, and the ends counted for one - for the tests and the view. */
uint32_t vibeos_pipe_in_use(void);
/* Bytes waiting to be read, for whoever asks whether a read would wait. */
uint32_t vibeos_pipe_pending(int slot);
/* Room for more, the size less what it holds, read under one hold of the lock:
 * the two read apart could be from either side of an F_SETPIPE_SZ that shrank
 * the pipe, and their difference then wrapped (M-111). */
uint32_t vibeos_pipe_room(int slot);
uint32_t vibeos_pipe_readers(int slot);
uint32_t vibeos_pipe_writers(int slot);

#endif
