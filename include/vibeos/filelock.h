#ifndef VIBEOS_FILELOCK_H
#define VIBEOS_FILELOCK_H

/* Advisory locks on files (docs/abi/ L1 step 6).
 *
 * Two kinds, kept apart because the systems that have them keep them apart:
 *
 *   - record locks: a range of bytes, held by an owner. POSIX's fcntl locks
 *     are these with a process for an owner, Linux's open-file-description
 *     locks the same with a description for one, and a Windows LockFileEx is
 *     the same shape again. An owner's own locks never conflict with each
 *     other: asking again for a range replaces what the owner held there,
 *     splitting and joining as needed, so an owner's locks on one file are
 *     always disjoint and never two where one would do.
 *   - whole-file locks: BSD's flock. Held by an open file description, and in
 *     a space of their own - a flock and a record lock on one file do not see
 *     each other, as on Linux.
 *
 * Shared locks coexist; an exclusive one coexists with nothing held by another
 * owner. They are advisory: nothing here stops a read or a write.
 *
 * This layer never waits. A lock that cannot be had is refused with the owner
 * in the way, and the caller decides whether to return or to try again later;
 * vibeos_flk_wait records who is waiting for whom so that a wait which could
 * never end - A for B while B waits for A - is refused instead of entered.
 *
 * A file is named by two opaque values: which filesystem (the mount) and the
 * filesystem's identity for the file. An owner is one opaque non-zero value;
 * the two macros below keep a process and a description from ever being the
 * same one. The table is fixed - no allocation on a path a close runs - and
 * locks itself with a lock of its own (vibeos_flk_set_lock). */

#include <stdint.h>

#define VIBEOS_FLK_MAX 256u       /* locks, all files and owners together */
#define VIBEOS_FLK_WAITERS 32u

#define VIBEOS_FLK_UNLOCK 0u
#define VIBEOS_FLK_SHARED 1u
#define VIBEOS_FLK_EXCL   2u

#define VIBEOS_FLK_RECORD 0u      /* byte ranges: fcntl, OFD locks          */
#define VIBEOS_FLK_WHOLE  1u      /* flock                                  */

#define VIBEOS_FLK_END (~0ull)    /* a range that runs to the end of the file, wherever that goes */

#define VIBEOS_FLK_OWNER_PROC(tgid) ((1ull << 62) | (uint64_t)(tgid))
#define VIBEOS_FLK_OWNER_FILE(f)    ((2ull << 62) | ((uint64_t)(uintptr_t)(f) >> 2))

typedef struct {
    uint32_t type;                /* VIBEOS_FLK_SHARED or _EXCL */
    uint64_t start, end;          /* inclusive */
    uint64_t owner;
    uint32_t pid;                 /* what the holder said its process was */
} vibeos_flk_info_t;

void vibeos_flk_set_lock(void (*lock)(void), void (*unlock)(void));
void vibeos_flk_reset(void);      /* boot and tests: no locks, nobody waiting */

/* Take, change or give back [start, end] for `owner`. `type` UNLOCK gives it
 * back, and never fails for a conflict.
 *
 * 0; -EAGAIN when another owner's lock is in the way, with that owner in
 * `*blocker` (may be null); -ENOLCK when the table has no room - checked
 * before anything is changed, so a refusal leaves the owner's locks as they
 * were; -EINVAL for a range that ends before it starts. */
int vibeos_flk_set(uint32_t space, const void *fs, uint64_t node, uint64_t owner,
                   uint32_t pid, uint32_t type, uint64_t start, uint64_t end,
                   uint64_t *blocker);

/* Would `type` on [start, end] be refused for `owner`? 1 and the lock in the
 * way, or 0. */
int vibeos_flk_test(uint32_t space, const void *fs, uint64_t node, uint64_t owner,
                    uint32_t type, uint64_t start, uint64_t end, vibeos_flk_info_t *out);

/* Everything an owner holds: on one file, or everywhere. Both also end any
 * wait the owner had recorded. */
void vibeos_flk_drop_file(uint64_t owner, const void *fs, uint64_t node);
void vibeos_flk_drop_owner(uint64_t owner);

/* `waiter` is about to wait for `blocker`. -EDEADLK if `blocker` is, through
 * however many others, waiting for `waiter`; otherwise 0 and the wait is on
 * record until vibeos_flk_wait_done. */
int vibeos_flk_wait(uint64_t waiter, uint64_t blocker);
void vibeos_flk_wait_done(uint64_t waiter);

uint32_t vibeos_flk_count(void);  /* locks held, for reporting and tests */

#endif
