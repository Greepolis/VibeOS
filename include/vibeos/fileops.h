#ifndef VIBEOS_FILEOPS_H
#define VIBEOS_FILEOPS_H

/* The file types a descriptor can name, and how each is opened (docs/abi/ A3).
 *
 * One operations table per type - regular file, directory, pipe end, socket,
 * console - so read, write, lseek, fstat, ioctl and getdents dispatch by type.
 * They used to be `if` chains in each syscall, which is how a pipe descriptor
 * came to fstat as a regular file and a socket as a character device.
 *
 * Written against the kernel services (vibeos/ksvc.h) and not against any one
 * personality, in kernel/abi/files/: Linux uses them today and a Windows
 * personality is meant to use the same ones. The errors they return are the
 * kernel's, which are numbered as Linux's (vibeos/abi_linux.h); a personality with
 * other numbers translates at its boundary, as Linux's sigset numbering is
 * translated at its own. */

#include <stdint.h>

#include "vibeos/file.h"
#include "vibeos/fdtable.h"

/* The requests the console's ioctl answers, numbered as Linux's TIOCGPGRP and
 * TIOCSPGRP: the ioctl operation takes the request a Linux program passes, and
 * another personality translates its own into these, as it does open flags. */
#define VIBEOS_IOCTL_GET_PGRP 0x540Fu
#define VIBEOS_IOCTL_SET_PGRP 0x5410u

extern const vibeos_file_ops_t vibeos_fops_regular;
extern const vibeos_file_ops_t vibeos_fops_dir;
extern const vibeos_file_ops_t vibeos_fops_pipe;
extern const vibeos_file_ops_t vibeos_fops_socket;
extern const vibeos_file_ops_t vibeos_fops_console;
extern const vibeos_file_ops_t vibeos_fops_pidfd;
extern const vibeos_file_ops_t vibeos_fops_chrdev;
extern const vibeos_file_ops_t vibeos_fops_eventfd;
extern const vibeos_file_ops_t vibeos_fops_timerfd;

/* An event counter (docs/abi/ L4 step 2): `count` to start from, and with
 * VIBEOS_EVENT_SEMAPHORE a read takes one at a time instead of all of it.
 * NULL if the table is full. */
#define VIBEOS_EVENT_SEMAPHORE 0x1u
vibeos_file_t *vibeos_open_eventfd(uint64_t count, uint32_t event_flags, uint32_t flags);

/* A timer (docs/abi/ L4 step 3), in clock ticks: `clock` is kept only to be
 * reported. Set arms it to expire at tick `next` (0 disarms) and every
 * `interval` after, clears the count of unread expiries, and hands back what
 * was left and the period it had; get reads both. NULL if the table is full. */
vibeos_file_t *vibeos_open_timerfd(int32_t clock, uint32_t flags);
void vibeos_timerfd_set(vibeos_file_t *f, uint64_t next, uint64_t interval, uint64_t *old_left,
                        uint64_t *old_interval);
void vibeos_timerfd_get(vibeos_file_t *f, uint64_t *left, uint64_t *interval);

/* What a character device node opens as, by its number (vibeos/devfs.h): the
 * console for a terminal, a description of its own for null, zero, full and
 * the random devices. -ENXIO for a number with no driver, -ENFILE when the
 * table is full; *err is 0 otherwise. */
vibeos_file_t *vibeos_open_chrdev(uint32_t rdev, uint32_t flags, long *err);

/* A description naming a process (L2 step 5): its pid and the tenancy of its
 * slot, so that a reused pid is not it. NULL if the table is full. */
vibeos_file_t *vibeos_open_pidfd(uint32_t pid, uint32_t seq, uint32_t flags);
/* The slot of the process a pidfd names, under the scheduler's lock, or -1
 * once it has been reaped. */
int vibeos_pidfd_slot(const vibeos_file_t *f);

/* What vibeos_open_path needs the walk to have done for `flags` (VIBEOS_O_*):
 * VIBEOS_PATH_* flags for vibeos_path_walk. */
uint32_t vibeos_open_walk_flags(uint32_t flags);

/* A walked path (vibeos/path.h, walked with vibeos_open_walk_flags): a directory
 * or a regular file, one reference. `mode` is the permission bits a created
 * file gets, the caller's umask already applied.
 *
 * The flags mean what Linux means by them: O_CREAT makes the file now, O_EXCL
 * refuses an existing one, O_TRUNC empties it, O_APPEND writes at its end,
 * O_RDWR reads and writes. Opening to write on a filesystem that writes
 * nothing is EROFS. 0 and a negated errno in *err otherwise. */
vibeos_file_t *vibeos_open_path(const vibeos_path_t *w, uint32_t flags, uint32_t mode,
                                long *err);

/* A new pipe: its read end and its write end, one reference each. 0 or -EMFILE. */
int vibeos_open_pipe(uint32_t flags, vibeos_file_t **rd, vibeos_file_t **wr);

/* A description for a socket the network stack already made; its tenancy is
 * taken now, so a later wait can tell the socket from a successor (M-020). */
vibeos_file_t *vibeos_open_socket(int sock);

/* Descriptors 0, 1 and 2 of a fresh table: one console description, three
 * references. 0, or -1. */
int vibeos_files_std_console(vibeos_fdtable_t *t);

/* Whether the socket a description was opened on is still that socket - not
 * closed under it, not a successor in its slot (M-020). Its index, or -1. */
int vibeos_sockfile_stable(const vibeos_file_t *f);

/* Sockets. A new IP one of `kind` (VIBEOS_INET_SOCK_*) owned by process `owner`,
 * or -EAFNOSUPPORT, -ENOBUFS, -ENFILE in *err. What it does is its type's
 * sockops (vibeos/sockops.h): the calls that wait - connect, accept, the reads -
 * wait there, with the socket's tenancy re-checked on every pass, so a
 * personality only translates addresses. */
vibeos_file_t *vibeos_sockfile_create(int kind, uint32_t owner, long *err);

/* Local sockets (unixsock.c, docs/abi/ L5): a new one of `type`
 * (VIBEOS_SOCK_STREAM or _DGRAM) with description flags `flags`, or two
 * connected to each other. A pathname a personality binds or connects to is
 * absolute, and for a bind the personality has made its socket node. */
extern const vibeos_file_ops_t vibeos_fops_unix;
vibeos_file_t *vibeos_unix_create(int type, uint32_t flags, long *err);
long vibeos_unix_pair(int type, uint32_t flags, vibeos_file_t **a, vibeos_file_t **b);
void vibeos_unix_reset(void);

/* A regular file's buffered bytes are committed when its last descriptor goes,
 * and whatever caches a file's contents must forget it then - the exec staging
 * cache, or a rewritten program keeps running as its old self. Registered by the
 * personality that owns the cache. */
void vibeos_files_on_write_back(void (*fn)(void));

/* Console writes whose leading bytes read as NUL: see the console's write. The
 * boot's MUSTBEZERO line reports it. */
extern uint64_t g_ring3_write_nul;

#endif
