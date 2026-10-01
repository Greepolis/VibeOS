#ifndef VIBEOS_FILE_H
#define VIBEOS_FILE_H

/* Open file descriptions (docs/abi/ phase A3).
 *
 * What a descriptor refers to. Linux, POSIX and Windows handles all separate the
 * number a process uses from the object behind it: `dup`, `dup2` and `fork` give a
 * second number for the *same* open file, and the two share its offset and its
 * flags. This kernel had no such object. A descriptor table entry was the file -
 * its offset, its buffer, its pipe end - and dup copied it by value, so two
 * descriptors that every program expects to share an offset did not, and each
 * copy had to remember to take a pipe end or the pipeline hung.
 *
 * A description is counted by the descriptors that name it. The last one to let
 * go runs the type's `release`: a pipe end is given back, a socket closed, a
 * written file committed. That is also why a pipe's reader and writer counts
 * now move only when a description is made or released, never on dup or fork.
 *
 * What each type does is its `ops`, one table per type (regular file, directory,
 * pipe end, socket, console - kernel/abi/files/). The fields below are the union of
 * what those types keep; a type uses its own and ignores the rest. This layer is
 * portable and host-tested; it does not touch user memory or sleep. It locks
 * itself (vibeos_file_set_lock). */

#include <stdint.h>

#include "vibeos/path.h"
#include "vibeos/vfs.h"

#define VIBEOS_FILE_MAX 256u
#define VIBEOS_FILE_PATH VIBEOS_PATH_MAX

/* Open flags a description keeps, in Linux's numbering - the numbers a Linux
 * program passes. Another personality translates its own into these. */
#define VIBEOS_O_ACCMODE  0x3u
#define VIBEOS_O_RDONLY   0x0u
#define VIBEOS_O_WRONLY   0x1u
#define VIBEOS_O_RDWR     0x2u
#define VIBEOS_O_CREAT    0x40u
#define VIBEOS_O_EXCL     0x80u
#define VIBEOS_O_TRUNC    0x200u
#define VIBEOS_O_APPEND   0x400u
#define VIBEOS_O_NONBLOCK 0x800u
#define VIBEOS_O_DIRECTORY 0x10000u
#define VIBEOS_O_NOFOLLOW 0x20000u
#define VIBEOS_O_CLOEXEC  0x80000u   /* a descriptor's, never kept here */

/* What kind of file fstat says it is: VIBEOS_S_IF* in vibeos/vfs.h. */

/* Where a seek is measured from: Linux's SEEK_SET, SEEK_CUR and SEEK_END, which
 * are POSIX's and C's too. */
#define VIBEOS_SEEK_SET 0
#define VIBEOS_SEEK_CUR 1
#define VIBEOS_SEEK_END 2

typedef struct vibeos_file vibeos_file_t;

/* What fstat reports, filled by the type. A type that has no owner or times
 * leaves them zero; vibeos_file_stat_clear is what makes that true. */
typedef struct vibeos_file_stat {
    uint32_t mode;       /* S_IF* | permissions */
    uint64_t size;
    uint64_t ino;
    uint32_t nlink;      /* 0 is reported as 1 */
    uint32_t uid;
    uint32_t gid;
    uint64_t atime_ns;
    uint64_t mtime_ns;
    uint64_t ctime_ns;
} vibeos_file_stat_t;

void vibeos_file_stat_clear(vibeos_file_stat_t *st);
/* What a filesystem node says, as stat reports it: a directory's size is 0,
 * and a node without an identity is inode 2, the root's. */
void vibeos_file_stat_from_node(vibeos_file_stat_t *st, const vibeos_fs_node_t *node);

/* One directory entry, as a directory description hands it out: the name, and
 * what stat would say of it - the same inode number, and the type in the mode.
 * What a personality makes of it (Linux's dirent64, a Windows directory
 * information record) is the personality's. */
typedef struct {
    char name[VIBEOS_NAME_MAX + 1u];
    uint64_t ino;
    uint32_t mode;       /* VIBEOS_S_IF*, and the permission bits when known */
} vibeos_dirent_t;

#define VIBEOS_READDIR_END  1   /* no entry at this position or after it       */
#define VIBEOS_READDIR_SKIP 2   /* none at this position; the next may have one */

/* What a description can do now without waiting (ready, below). */
#define VIBEOS_READY_IN  0x1u    /* a read returns at once: data, or end of file  */
#define VIBEOS_READY_OUT 0x2u    /* a write would be taken                         */
#define VIBEOS_READY_HUP 0x4u    /* the other side has gone                        */

/* One file type. Every entry may be NULL, and the caller answers for a missing
 * one with what Linux answers: EINVAL for read/write, ESPIPE for seek and the
 * positional calls, ENOTTY for ioctl, ENOTDIR for getdents, EINVAL for truncate
 * and sync. Return values are byte counts or negated errno. Tables are written
 * with designated initialisers: the type grew four entries in L1. */
typedef struct vibeos_file_ops {
    const char *name;
    long (*read)(vibeos_file_t *f, uint64_t ubuf, uint64_t len);
    long (*write)(vibeos_file_t *f, uint64_t ubuf, uint64_t len);
    long (*seek)(vibeos_file_t *f, int64_t off, int whence);
    int (*stat)(vibeos_file_t *f, vibeos_file_stat_t *out);
    long (*ioctl)(vibeos_file_t *f, uint64_t req, uint64_t arg);
    /* The entry at `index`, positions counted from 0: "." and ".." first,
     * whatever the filesystem itself lists. 0 and the entry, VIBEOS_READDIR_END,
     * VIBEOS_READDIR_SKIP, or a negated errno. The position is the caller's:
     * the description's `pos`, which lseek moves, so rewinding a directory is
     * seeking it to 0. */
    int (*readdir)(vibeos_file_t *f, uint64_t index, vibeos_dirent_t *out);
    /* The last reference went: give back what this description holds. */
    void (*release)(vibeos_file_t *f);
    /* At an offset, leaving the description's own position alone (L1). */
    long (*pread)(vibeos_file_t *f, uint64_t ubuf, uint64_t len, uint64_t off);
    long (*pwrite)(vibeos_file_t *f, uint64_t ubuf, uint64_t len, uint64_t off);
    int (*truncate)(vibeos_file_t *f, uint64_t size);
    /* What it can do now without waiting: VIBEOS_READY_*. A type without the
     * entry is always ready for both, which is true of a file on a
     * filesystem and is what Linux assumes of a file that cannot say. */
    uint32_t (*ready)(vibeos_file_t *f);
    /* What this description wrote is on the medium. */
    int (*sync)(vibeos_file_t *f);
} vibeos_file_ops_t;

struct vibeos_file {
    volatile uint32_t refs;       /* descriptors naming it; 0 = free          */
    uint32_t gen;                 /* which tenancy of this slot               */
    const vibeos_file_ops_t *ops;
    uint32_t flags;               /* VIBEOS_O_*, less O_CLOEXEC               */
    uint64_t pos;                 /* shared by every descriptor naming it     */
    /* pipe end */
    int pipe;
    int pipe_write;
    /* socket: its index and the tenancy it was opened on (M-020) */
    int sock;
    uint32_t sock_gen;
    /* regular file or directory: which mount, and the absolute path - the
     * part inside the mount starts at path + tail (docs/abi/ A4) */
    vibeos_fsmount_t *mnt;
    uint32_t tail;
    uint64_t node;                /* the filesystem's identity, opaque        */
    uint64_t size;
    int isdir;
    char path[VIBEOS_FILE_PATH];
    /* Set at the first write through this description, which is when anything
     * cached from the file stops being true (vibeos_files_on_write_back). */
    int dirty;
};

void vibeos_file_set_lock(void (*lock)(void), void (*unlock)(void));
void vibeos_file_reset(void);   /* boot and tests: every description free */

/* A new description with one reference, every type field in its empty state
 * (no pipe, no socket), or 0 when the pool is exhausted. */
vibeos_file_t *vibeos_file_alloc(const vibeos_file_ops_t *ops, uint32_t flags);

/* One more reference - a descriptor duplicated, a table copied at fork. */
void vibeos_file_get(vibeos_file_t *f);

/* One fewer. The last runs ops->release, then frees the slot. A put with no
 * reference to give back is counted (VIBEOS_MBZ_FILE_PUT_UNDERFLOW), not clamped:
 * it is a descriptor released twice, and clamping would hide the second owner. */
void vibeos_file_put(vibeos_file_t *f);

uint32_t vibeos_file_in_use(void);

#endif
