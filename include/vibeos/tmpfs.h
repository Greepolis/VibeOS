#ifndef VIBEOS_TMPFS_H
#define VIBEOS_TMPFS_H

/* tmpfs: a filesystem in memory, with everything POSIX asks of one (docs/abi/ L1,
 * step 2).
 *
 * Every filesystem this kernel had was either read-only or FAT, and FAT has no
 * owners, no permission bits, no links of either kind and writes a whole file at
 * a time. So stat, chmod, symlink, rename-over and a write in the middle of a
 * file had nothing true to report. tmpfs has all of it: modes, owners, hard and
 * symbolic links, three timestamps, sparse files, rename over an existing name,
 * and writes at any offset. It is mounted at /tmp, which is where programs put
 * the files they make.
 *
 * Portable and host-tested. It allocates pages through the functions it is given
 * and locks itself with the lock it is given, because it has many callers on
 * many cores (CLAUDE.md: "a layer that is serialised by accident is not
 * serialised") - and its own lock, since it allocates.
 *
 * Inodes are a fixed table. A file's data is pages, found through sixteen direct
 * pointers, one page of pointers and one page of pages of pointers; a page never
 * written is a hole and reads as zeros. A directory is a file of fixed-size
 * records.
 *
 * Known gap: an inode whose last name goes is freed at once, not when the last
 * description on it closes - the VFS does not tell a driver about opens. Its
 * generation moves on, so a description left holding it gets ENOENT from every
 * read and write rather than another file's bytes. */

#include <stdint.h>

#include "vibeos/vfs.h"

#define VIBEOS_TMPFS_INODES 1024u
#define VIBEOS_TMPFS_DIRECT 16u
#define VIBEOS_TMPFS_PAGE 4096u
#define VIBEOS_TMPFS_MAGIC 0x01021994ull   /* Linux's TMPFS_MAGIC */

typedef struct {
    uint32_t mode;                /* 0: a free slot */
    uint32_t nlink;
    uint32_t uid;
    uint32_t gid;
    uint32_t gen;                 /* moves on every free: a stale id is refused */
    uint32_t parent;              /* a directory's "..", as an inode index      */
    uint64_t size;
    uint64_t atime_ns;
    uint64_t mtime_ns;
    uint64_t ctime_ns;
    uint8_t *direct[VIBEOS_TMPFS_DIRECT];
    uint8_t **ind;                /* one page of page pointers                  */
    uint8_t ***dind;              /* one page of pages of page pointers         */
} vibeos_tmpfs_inode_t;

typedef struct {
    vibeos_tmpfs_inode_t inode[VIBEOS_TMPFS_INODES];
    void *(*page_alloc)(void);    /* a page, or 0; contents unspecified         */
    void (*page_free)(void *page);
    void (*lock)(void);
    void (*unlock)(void);
    uint64_t pages_max;           /* data and pointer pages together            */
    uint64_t pages_used;
    uint32_t inodes_used;
} vibeos_tmpfs_t;

/* An empty filesystem - a root directory, mode 1777 as /tmp is - allowed at most
 * `pages_max` pages. 0, or -1 when a function is missing. */
int vibeos_tmpfs_init(vibeos_tmpfs_t *t, uint64_t pages_max,
                      void *(*page_alloc)(void), void (*page_free)(void *page),
                      void (*lock)(void), void (*unlock)(void));

/* Everything freed; the filesystem is empty again. For tests. */
void vibeos_tmpfs_destroy(vibeos_tmpfs_t *t);

/* The operations, for vibeos_fs_mount. */
const vibeos_fs_ops_t *vibeos_tmpfs_ops(void);

#endif
