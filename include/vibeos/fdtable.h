#ifndef VIBEOS_FDTABLE_H
#define VIBEOS_FDTABLE_H

#include <stdint.h>

#include "vibeos/file.h"

/* A process's descriptors (C5; rebuilt in docs/abi/ phase A3).
 *
 * A descriptor is a number and a reference to an open file description
 * (vibeos/file.h), plus the one flag that belongs to the number rather than the
 * file: close-on-exec. Two descriptors naming one description share its offset -
 * which is what `dup`, `dup2` and `fork` promise and what this table could not do
 * while an entry held the file by value.
 *
 * Descriptors 0, 1 and 2 are ordinary entries. They used to be a separate array
 * meaning "the console unless redirected", so closing 1 and opening a file gave
 * the file descriptor 3 instead of the 1 every shell's redirection relies on.
 *
 * The table holds up to VIBEOS_FD_MAX descriptors (Linux's default RLIMIT_NOFILE)
 * and grows a page of VIBEOS_FD_PER_PAGE at a time, from pages the kernel supplies
 * (vibeos_fdtable_set_pages): a process that opens three files costs nothing for
 * the other thousand. It was four entries, so any program opening a fifth file
 * failed.
 *
 * Not locked here: the table is the process's and the caller holds the process's
 * files_lock around every change. Nothing in this file releases a description
 * except vibeos_fdtable_destroy and vibeos_fdtable_drop_cloexec, which run on a
 * table nobody else can reach; everything else hands the description back so the
 * caller releases it after unlocking - a release writes files back and wakes
 * pipe readers, and neither belongs under a spinlock. */

#define VIBEOS_FD_MAX 1024u
#define VIBEOS_FD_PER_PAGE 256u
#define VIBEOS_FD_PAGES (VIBEOS_FD_MAX / VIBEOS_FD_PER_PAGE)
#define VIBEOS_FD_CLOEXEC 1u    /* Linux's FD_CLOEXEC */

/* Why an install failed. */
#define VIBEOS_FDT_FULL  (-1)   /* no free descriptor below the limit: EMFILE */
#define VIBEOS_FDT_NOMEM (-2)   /* a page for the table could not be had      */
#define VIBEOS_FDT_BADFD (-3)   /* the number is outside the table: EBADF     */

typedef struct vibeos_fdent {
    vibeos_file_t *file;
    uint32_t flags;
    uint32_t reserved;
} vibeos_fdent_t;

typedef struct vibeos_fdtable {
    vibeos_fdent_t *page[VIBEOS_FD_PAGES];
    uint32_t limit;   /* RLIMIT_NOFILE: descriptors below this may be installed */
    uint32_t open;    /* how many are installed                                 */
} vibeos_fdtable_t;

/* Where the table's pages come from: at least 4096 bytes each. Registered once
 * (a registration function, not a weak symbol - see CLAUDE.md on mingw). */
void vibeos_fdtable_set_pages(void *(*alloc)(void), void (*release)(void *));

/* Empty, with the default limit and no pages. For a table that has never held
 * anything or has been destroyed - it forgets, it does not release. */
void vibeos_fdtable_init(vibeos_fdtable_t *t);

/* The description `fd` names, or 0. No reference is taken: the caller either holds
 * the table's lock for as long as it uses the result or takes one itself. */
vibeos_file_t *vibeos_fdtable_get(const vibeos_fdtable_t *t, uint64_t fd);
uint32_t vibeos_fdtable_flags(const vibeos_fdtable_t *t, uint64_t fd);
int vibeos_fdtable_set_flags(vibeos_fdtable_t *t, uint64_t fd, uint32_t flags);

/* Install `f` at the lowest free descriptor at or above `min`, taking over the
 * caller's reference. The descriptor, or VIBEOS_FDT_FULL / VIBEOS_FDT_NOMEM (the
 * reference is then still the caller's). */
int vibeos_fdtable_install(vibeos_fdtable_t *t, vibeos_file_t *f, uint32_t flags,
                           uint32_t min);

/* Install `f` at exactly `fd`, replacing what was there - dup2. `*old` receives the
 * replaced description, whose reference the caller now owns and releases. 0, or
 * VIBEOS_FDT_BADFD / VIBEOS_FDT_NOMEM. */
int vibeos_fdtable_install_at(vibeos_fdtable_t *t, uint64_t fd, vibeos_file_t *f,
                              uint32_t flags, vibeos_file_t **old);

/* Take `fd` out of the table; its description (the caller's reference now), or 0. */
vibeos_file_t *vibeos_fdtable_remove(vibeos_fdtable_t *t, uint64_t fd);

/* Make `dst` (empty) name every description `src` names, one more reference each -
 * fork and exec. 0, or VIBEOS_FDT_NOMEM with `dst` left empty. */
int vibeos_fdtable_copy(vibeos_fdtable_t *dst, const vibeos_fdtable_t *src);

/* Release every close-on-exec descriptor; how many. For a table nobody else uses. */
uint32_t vibeos_fdtable_drop_cloexec(vibeos_fdtable_t *t);

/* Release every descriptor and give the pages back; the table is empty after. */
void vibeos_fdtable_destroy(vibeos_fdtable_t *t);

/* The highest descriptor number in use, or -1. For close_range and walks. */
int vibeos_fdtable_highest(const vibeos_fdtable_t *t);

#endif
