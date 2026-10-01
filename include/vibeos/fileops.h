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

/* Sockets. A new one of `kind` (VIBEOS_INET_SOCK_*) owned by process `owner`;
 * the calls that wait - connect, accept, recvfrom - wait here, with the socket's
 * tenancy re-checked on every pass, so a personality only translates addresses.
 * Byte counts, 0, or a negated errno. accept hands back a new description. */
vibeos_file_t *vibeos_sockfile_create(int kind, uint32_t owner, long *err);
long vibeos_sockfile_bind(vibeos_file_t *f, uint16_t port);
long vibeos_sockfile_listen(vibeos_file_t *f);
long vibeos_sockfile_connect(vibeos_file_t *f, uint32_t ip, uint16_t port);
long vibeos_sockfile_accept(vibeos_file_t *f, uint32_t owner, vibeos_file_t **child,
                            uint32_t *ip, uint16_t *port);
long vibeos_sockfile_sendto(vibeos_file_t *f, uint64_t buf, uint64_t len,
                            uint32_t ip, uint16_t port);
long vibeos_sockfile_recvfrom(vibeos_file_t *f, uint64_t buf, uint64_t len,
                              uint32_t *ip, uint16_t *port);

/* A regular file's buffered bytes are committed when its last descriptor goes,
 * and whatever caches a file's contents must forget it then - the exec staging
 * cache, or a rewritten program keeps running as its old self. Registered by the
 * personality that owns the cache. */
void vibeos_files_on_write_back(void (*fn)(void));

/* Console writes whose leading bytes read as NUL: see the console's write. The
 * boot's MUSTBEZERO line reports it. */
extern uint64_t g_ring3_write_nul;

#endif
