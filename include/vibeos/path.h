#ifndef VIBEOS_PATH_H
#define VIBEOS_PATH_H

/* Paths (docs/abi/ phase A4): one walk, from a working directory and a root,
 * through the mount table.
 *
 * Every handler used to hand the root filesystem whatever string the program
 * gave it: there was no working directory, "." and ".." meant whatever the FAT
 * driver made of them, and a path under /ext2 reached the boot volume anyway
 * because nothing consulted the mount table. Now a path is made absolute here
 * first - against the process's working directory, or a directory descriptor's
 * path - and then resolved through the mount table, one component at a time,
 * so a missing directory is ENOENT and a file used as a directory is ENOTDIR,
 * as Linux reports them.
 *
 * Since docs/abi/ L1 the walk resolves symbolic links as it goes, one
 * component at a time: a link's target is spliced in front of what is left of
 * the path, an absolute target starts again from the process's root, and more
 * than VIBEOS_PATH_SYMLINK_MAX links in one walk is ELOOP. ".." removes a
 * component of the path resolved so far - so "link/.." is the parent of where
 * the link points, as on Linux, not the directory holding the link - and never
 * climbs above the root. vibeos_path_normalize stays lexical; it joins paths,
 * it does not resolve them.
 *
 * The errors are the kernel's (vibeos/abi_linux.h), negated. Portable and
 * host-tested. */

#include <stdint.h>

#include "vibeos/vfs.h"

/* Longest path, NUL included, and longest component. Linux's PATH_MAX is 4096;
 * this is smaller because paths live on kernel stacks here, and a path longer
 * than it is ENAMETOOLONG, as a longer-than-4096 one is on Linux. */
#define VIBEOS_PATH_MAX 256u
#define VIBEOS_NAME_MAX 255u

/* `path` made absolute and normal: against `cwd` if it is relative, against
 * `root` if it is absolute; no ".", "..", repeated or trailing slash; never
 * above `root`. `cwd` and `root` are absolute and normal already. 0, -ENOENT for
 * an empty path, -ENAMETOOLONG. */
int vibeos_path_normalize(const char *root, const char *cwd, const char *path,
                          char *out, uint32_t cap);

/* What a walk found. */
typedef struct {
    /* Absolute, normal, every symbolic link resolved - the last component's
     * too, unless NOFOLLOW asked for the link itself. */
    char path[VIBEOS_PATH_MAX];
    vibeos_fsmount_t *mnt;        /* the mount the last component is on       */
    const char *tail;             /* the path inside it, into `path`; "" is its root */
    vibeos_fs_node_t node;        /* valid when `exists`                      */
    int exists;
    int trailing_slash;           /* the caller wrote "name/"                 */
} vibeos_path_t;

#define VIBEOS_PATH_NOFOLLOW 0x1u /* a last component that is a link is the answer */
#define VIBEOS_PATH_CREATE   0x2u /* a missing last component is an answer: exists = 0 */
#define VIBEOS_PATH_SYMLINK_MAX 40u   /* Linux's MAXSYMLINKS */

/* `path` - absolute, or relative to `base` - resolved under `root` (both
 * absolute and normal already). Every component before the last must be a
 * directory that exists; a trailing slash demands the last be one too.
 * 0 with `out` filled; -ENOENT, -ENOTDIR, -ELOOP, -ENAMETOOLONG.
 *
 * With CREATE a missing last component answers 0 with exists = 0, and `out`
 * says where it would be created: the parent has been walked and is a
 * directory. The path's own root cannot be missing. */
int vibeos_path_walk(const char *root, const char *base, const char *path,
                     uint32_t flags, vibeos_path_t *out);

#endif
