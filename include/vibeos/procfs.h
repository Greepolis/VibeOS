#ifndef VIBEOS_PROCFS_H
#define VIBEOS_PROCFS_H

#include <stdint.h>

#include "vibeos/vfs.h"

/* /proc, as far as a program has asked for it (docs/abi/ L3 step 3).
 *
 * Not a view of the processes yet - that is L2's - but the files a C library
 * or a test harness opens before it does anything else, generated when they
 * are read. A read-only filesystem like any other: nothing in the handlers
 * knows a path under /proc is special, which is the point of having it as a
 * filesystem and not as a list of names in open().
 *
 * What is here:
 *
 *   /proc/meminfo   MemTotal, MemFree, MemAvailable, Buffers, Cached,
 *                   SwapTotal, SwapFree, in Linux's own layout - LTP's
 *                   harness reads MemAvailable before every test.
 *   /proc/sys/kernel/pid_max
 *                   one more than the largest pid there can be, as a line.
 *
 * /proc/self/exe is still answered by the handlers that take it (execve,
 * readlink): it names a process, and this filesystem does not know who asks.
 */

/* Kibibytes, as /proc/meminfo reports them. */
typedef struct {
    uint64_t total_kb;
    uint64_t free_kb;
    uint64_t available_kb;   /* what could be had without swapping */
    uint64_t cached_kb;
    uint64_t swap_total_kb;
    uint64_t swap_free_kb;
} vibeos_procfs_mem_t;

/* The filesystem's state, which is who knows the numbers: the caller's to
 * declare and fill, and passed to vibeos_fs_mount as `fs`. The filesystem is
 * portable and the frame counts are not its to read; and it keeps nothing of
 * its own, so there is nothing in it for two cores to disagree about. A null
 * `mem` reports zeros - a machine with no memory, which a caller will notice. */
typedef struct {
    void (*mem)(vibeos_procfs_mem_t *out);
    uint64_t pid_max;        /* what /proc/sys/kernel/pid_max says */
} vibeos_procfs_t;

/* The operations, for vibeos_fs_mount. */
const vibeos_fs_ops_t *vibeos_procfs_ops(void);

#endif
