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
 * Lexical: ".." removes the previous component, and never climbs above the
 * process's root. That is exact for filesystems without symbolic links, which
 * is every one this kernel mounts today; the day one has them, the walk resolves
 * them component by component (ELOOP after a bound) instead.
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

/* What an absolute, normal path names: its mount, the path inside that mount
 * (`*tail` points into `abs`), and the node. Every component before the last must
 * be a directory. 0, -ENOENT, -ENOTDIR. */
int vibeos_path_lookup(const char *abs, vibeos_fsmount_t **mnt, const char **tail,
                       vibeos_fs_node_t *node);

/* The same for a path about to be created: its parent must exist and be a
 * directory, the name itself need not. 0, -ENOENT, -ENOTDIR, -EEXIST is not
 * judged here (a create that replaces is the caller's decision). */
int vibeos_path_parent(const char *abs, vibeos_fsmount_t **mnt, const char **tail);

#endif
