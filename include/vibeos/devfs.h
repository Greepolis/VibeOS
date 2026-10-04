#ifndef VIBEOS_DEVFS_H
#define VIBEOS_DEVFS_H

#include <stdint.h>

#include "vibeos/vfs.h"

/* /dev (docs/abi/ L2 step 6).
 *
 * The names programs open without asking whether they exist: /dev/null,
 * /dev/zero, /dev/full, /dev/random, /dev/urandom, /dev/tty and
 * /dev/console, and the links /dev/fd, /dev/stdin, /dev/stdout and
 * /dev/stderr into /proc/self/fd. A read-only filesystem with no state, like
 * /proc: it only names things.
 *
 * A node here is a character device, which is a type and a number and nothing
 * else. What opening one gives is decided by the number, in the file types
 * (vibeos_open_chrdev, kernel/abi/files/chrdev.c) - wherever the node is, so a
 * filesystem that keeps device nodes will open the same devices. None does
 * yet: mknod refuses a device node (kernel/abi/linux/names.c).
 *
 * Numbers are Linux's, encoded as Linux encodes a small one: major in bits 8
 * and up, minor in the low byte. The kernel's errors are numbered as Linux's for
 * the same reason; a personality with other numbers translates them. */

#define VIBEOS_MKDEV(major, minor) (((uint32_t)(major) << 8) | (uint32_t)(minor))
#define VIBEOS_DEV_MAJOR(d) ((uint32_t)(d) >> 8)
#define VIBEOS_DEV_MINOR(d) ((uint32_t)(d) & 0xffu)

#define VIBEOS_DEV_NULL    VIBEOS_MKDEV(1, 3)
#define VIBEOS_DEV_ZERO    VIBEOS_MKDEV(1, 5)
#define VIBEOS_DEV_FULL    VIBEOS_MKDEV(1, 7)
#define VIBEOS_DEV_RANDOM  VIBEOS_MKDEV(1, 8)
#define VIBEOS_DEV_URANDOM VIBEOS_MKDEV(1, 9)
#define VIBEOS_DEV_TTY     VIBEOS_MKDEV(5, 0)
#define VIBEOS_DEV_CONSOLE VIBEOS_MKDEV(5, 1)

/* The operations, for vibeos_fs_mount. No state: `fs` is unused. */
const vibeos_fs_ops_t *vibeos_devfs_ops(void);

#endif
