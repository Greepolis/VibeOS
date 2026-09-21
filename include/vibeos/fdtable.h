#ifndef VIBEOS_FDTABLE_H
#define VIBEOS_FDTABLE_H

#include <stdint.h>

/* C5: a task's open files, apart from the machine that runs it.
 *
 * Descriptors 0-2 are the console unless a shell has redirected them; 3 and up are
 * table entries. That split is the ABI's, and this type is where it is stated once:
 * `fds[]` holds descriptor 3 onwards (index = fd - 3) and `std[]` holds what 0, 1 and
 * 2 currently mean (`used` clear means the console). Every caller used to know it
 * and index the arrays itself, in nine places, and fork copied them by hand twice.
 *
 * Entries are plain data - a pipe or socket is an index into a table the kernel
 * owns, not a pointer - so the table copies by value, which is what fork does. */

#define VIBEOS_FD_WBUF 512u
#define VIBEOS_FD_SLOTS 4u      /* descriptors 3 .. 3 + SLOTS - 1 */
#define VIBEOS_FD_STD 3u        /* descriptors 0, 1, 2 */
#define VIBEOS_FD_FIRST 3u      /* the number of the first table entry */

typedef struct vibeos_fd {
    int used;
    int writable;
    int dirty;
    /* Index into the pipe table, or -1. A descriptor is a pipe end when this is set;
     * `writable` then says which end. */
    int pipe;
    uint32_t cluster;
    uint64_t size;        /* 64-bit: a >4 GiB file must not wrap in fstat/lseek (M-033) */
    uint64_t pos;
    int net_sock;         /* index into the TCP/IP stack, or -1 for a file */
    uint32_t dir_index;   /* for getdents64 on a directory fd */
    /* Whether this descriptor names a directory. Determined when it is opened
     * rather than guessed later: opendir() opens the path and then fstats the
     * descriptor, and a descriptor that claims to be a regular file is refused
     * with ENOTDIR no matter what stat said about the path a moment earlier. */
    int isdir;
    char name[24];
    uint8_t wbuf[VIBEOS_FD_WBUF];
    uint32_t wlen;
} vibeos_fd_t;

typedef struct vibeos_fdtable {
    vibeos_fd_t fds[VIBEOS_FD_SLOTS];
    vibeos_fd_t std[VIBEOS_FD_STD];
} vibeos_fdtable_t;

/* One entry in the state a fresh descriptor starts in: not used, no pipe, no socket,
 * every other field zero. */
void vibeos_fd_clear(vibeos_fd_t *f);

/* Every entry cleared. The one place a task's table is initialised, so a recycled
 * slot cannot start believing the previous tenant's redirection - which sends a
 * write into a pipe that does not exist and leaves the task waiting there. */
void vibeos_fdtable_reset(vibeos_fdtable_t *t);

/* The entry for descriptor `fd` (3 or more) if it is in use, else NULL. */
vibeos_fd_t *vibeos_fdtable_get(vibeos_fdtable_t *t, uint64_t fd);

/* What descriptor 0, 1 or 2 has been redirected to, or NULL for the console. */
vibeos_fd_t *vibeos_fdtable_redirect(vibeos_fdtable_t *t, uint64_t fd);

/* The lowest free table index, or -1 when full. The descriptor number is
 * VIBEOS_FD_FIRST + the index. */
int vibeos_fdtable_free_index(const vibeos_fdtable_t *t);

/* Claim the lowest free table entry: cleared, marked used, its index returned (or -1
 * when full). The descriptor number is VIBEOS_FD_FIRST + the index. */
int vibeos_fdtable_claim(vibeos_fdtable_t *t);

/* Inherit `src` into `dst`, replacing whatever `dst` held. */
void vibeos_fdtable_copy(vibeos_fdtable_t *dst, const vibeos_fdtable_t *src);

/* Every entry, table and redirections alike: index 0 .. count-1. For the walks that
 * treat them the same (give each inherited pipe end an owner; release them at exit). */
uint32_t vibeos_fdtable_count(void);
vibeos_fd_t *vibeos_fdtable_entry(vibeos_fdtable_t *t, uint32_t index);

#endif
