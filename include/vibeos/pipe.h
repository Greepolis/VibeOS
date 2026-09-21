#ifndef VIBEOS_PIPE_H
#define VIBEOS_PIPE_H

#include <stdint.h>

#include "vibeos/fdtable.h"

/* C5: pipes, as a module.
 *
 * A pipe is a ring of bytes and two counts - how many descriptors hold its read end,
 * how many its write end. The ends are counted, not flagged, because a descriptor can
 * be duplicated and inherited: `ls | wc` gives the write end to a child, and the
 * parent must close its own copy or the reader never sees end of file. That is the
 * classic way a shell pipeline hangs, and it is a refcount bug, not a pipe bug - which
 * is why the counts live here, behind one interface, and are checked: every place that
 * used to adjust `writers` and `readers` by hand (dup2, fork, clone, close, exit) now
 * goes through acquire and release, and a release with nothing to release is counted
 * (VIBEOS_MBZ_PIPE_END_UNDERFLOW) instead of being clamped away in silence.
 *
 * This layer is portable. It does not sleep, raise signals or touch user memory: it
 * makes one attempt and says what happened, and the caller - which knows about the
 * scheduler and about signals - decides whether to wait, retry or fail. The copy to
 * and from user memory is the caller's function, passed in. Serialised by its *own*
 * lock, supplied by the architecture (vibeos_pipe_set_lock): "remember to hold the
 * lock" is not a property a compiler checks. */

#define VIBEOS_PIPE_MAX 8u
#define VIBEOS_PIPE_BYTES 4096u

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

void vibeos_pipe_reset(void);   /* boot and tests: every pipe free */

/* Create a pipe with one reader and one writer; its slot, or -1 when none is free. */
int vibeos_pipe_create(void);
/* Give back a pipe that was created and never handed to a descriptor. */
void vibeos_pipe_abandon(int slot);

/* One more / one fewer descriptor holds an end. `writable` selects which end. A
 * release that brings both counts to zero frees the pipe and drops its bytes: a reader
 * may still drain after every writer has closed, so it lives until both are gone. */
void vibeos_pipe_add_end(int slot, int writable);
void vibeos_pipe_release_end(int slot, int writable);

/* The same, for a descriptor: acquire when it is a pipe end in use; release also
 * detaches it (pipe = -1) so it cannot release twice. */
void vibeos_pipe_end_acquire(const vibeos_fd_t *f);
void vibeos_pipe_end_release(vibeos_fd_t *f);

/* One attempt. Returns the bytes moved (0 with a status that says why), and never
 * blocks. A fault after some bytes moved returns those bytes with VIBEOS_PIPE_OK. */
long vibeos_pipe_read(int slot, void *dst, uint64_t len, vibeos_pipe_copy_fn copy,
                      vibeos_pipe_status_t *status);
long vibeos_pipe_write(int slot, const void *src, uint64_t len, vibeos_pipe_copy_fn copy,
                       vibeos_pipe_status_t *status);

/* How many pipes exist, and the ends counted for one - for the tests and the view. */
uint32_t vibeos_pipe_in_use(void);
uint32_t vibeos_pipe_readers(int slot);
uint32_t vibeos_pipe_writers(int slot);

#endif
