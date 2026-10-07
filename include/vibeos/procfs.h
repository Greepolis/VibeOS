#ifndef VIBEOS_PROCFS_H
#define VIBEOS_PROCFS_H

#include <stdint.h>

#include "vibeos/vfs.h"

/* /proc (docs/abi/ L3 step 3, L2 step 6).
 *
 * A read-only filesystem like any other: nothing in the handlers knows a path
 * under /proc is special, which is the point of having it as a filesystem and
 * not as a list of names in open(). Every file is generated when it is read,
 * from facts asked of the mount's owner at that moment; the filesystem keeps
 * nothing, formats everything, and is host-tested with a source of its own.
 *
 * What is here:
 *
 *   meminfo, cpuinfo, version, uptime, loadavg, mounts (a link to
 *   self/mounts, as in Linux), sys/kernel/pid_max,
 *   sys/kernel/random/entropy_avail, sys/fs/inotify/max_queued_events,
 *   max_user_instances and max_user_watches
 *   self            a link to the asking process's directory
 *   <pid>/          one per process: stat, statm, status, cmdline, comm,
 *                   maps, mounts, the links exe, cwd and root, and fd/ - a
 *                   link per open descriptor, to what it was opened as
 *   <pid>/task/     a directory per thread, each the same as <tid>/ - which
 *                   exists for every thread, as on Linux, and is not listed
 *
 * Who asks is the source's to say (`self`), because the filesystem is portable
 * and the task table is not; the same goes for every fact about a process.
 * Whatever the source gives is copied out under its own locks and formatted
 * after they are released, so nothing here runs under somebody else's lock. */

/* Kibibytes, as /proc/meminfo reports them. */
typedef struct {
    uint64_t total_kb;
    uint64_t free_kb;
    uint64_t available_kb;   /* what could be had without swapping */
    uint64_t cached_kb;
    uint64_t swap_total_kb;
    uint64_t swap_free_kb;
} vibeos_procfs_mem_t;

/* One process, as /proc/<pid> shows it - or one thread of it, as <tid>/ and
 * task/<tid>/ do: then `pid` is the thread's id, `tgid` its process's, and the
 * state, name and signals are the thread's own. Times are in clock ticks of
 * 100 a second - Linux's USER_HZ, which is what a program divides by - and
 * signal sets in Linux's numbering, bit 0 for signal 1: the source converts. */
typedef struct {
    uint32_t pid, ppid, pgid, sid;
    uint32_t tgid;                /* the process; 0 is read as `pid` */
    uint32_t threads;
    uint32_t uid[4], gid[4];      /* real, effective, saved, filesystem */
    char state;                   /* R running, S waiting, T stopped, Z ended */
    char comm[16];
    int32_t nice;
    uint32_t umask;
    uint32_t tty;                 /* the terminal's device number, 0 for none */
    uint32_t tty_pgid;            /* its foreground group */
    uint64_t utime, stime, cutime, cstime;
    uint64_t start;               /* ticks after boot */
    uint64_t vsize;               /* bytes */
    uint64_t rss;                 /* pages */
    uint64_t rss_limit;           /* bytes */
    uint64_t sig_pending, sig_blocked, sig_ignored, sig_caught;
} vibeos_procfs_proc_t;

/* One region of a process's address space, for maps. */
#define VIBEOS_PROCFS_MAP_R      0x1u
#define VIBEOS_PROCFS_MAP_W      0x2u
#define VIBEOS_PROCFS_MAP_X      0x4u
#define VIBEOS_PROCFS_MAP_SHARED 0x8u
typedef struct {
    uint64_t start, end, offset;
    uint32_t prot;
    char name[16];                /* "[heap]", "[stack]", or empty */
} vibeos_procfs_map_t;

/* The links a process directory has besides its descriptors. */
#define VIBEOS_PROCFS_LINK_EXE  0u
#define VIBEOS_PROCFS_LINK_CWD  1u
#define VIBEOS_PROCFS_LINK_ROOT 2u
#define VIBEOS_PROCFS_LINK_FD   3u

/* The processor, as cpuinfo describes each one online. */
typedef struct {
    char vendor[16];
    char model_name[64];
    uint32_t family, model, stepping;
    uint32_t cpuid_level;
    uint32_t phys_bits, virt_bits;
    const char *flags;            /* Linux's names for the feature bits */
} vibeos_procfs_cpu_t;

/* The filesystem's state, which is who knows the numbers: the caller's to
 * declare and fill, and passed to vibeos_fs_mount as `fs`. Any entry may be
 * null: a null `mem` reports zeros, a null `proc` a machine with no processes,
 * and a file whose facts nobody supplies says so by being empty. */
typedef struct {
    void (*mem)(vibeos_procfs_mem_t *out);
    uint64_t pid_max;        /* what /proc/sys/kernel/pid_max says */
    vibeos_procfs_cpu_t cpu;
    uint32_t (*cpu_khz)(void);         /* 0 when not yet measured */
    uint32_t (*cpus)(void);            /* online now: the others start after /proc */
    uint64_t (*uptime_ms)(void);
    uint32_t (*entropy_avail)(void);   /* bits, as sys/kernel/random/entropy_avail */
    /* inotify's limits, as sys/fs/inotify/max_* say them: the events a queue
     * holds before its overflow record, the instances and the watches there may
     * be. All three are the whole machine's here, where Linux's are a user's. */
    uint32_t inotify_queued, inotify_instances, inotify_watches;
    const char *version;               /* /proc/version's line, without the newline */

    /* The process asking, by pid; 0 for none (the kernel, for itself). */
    uint32_t (*self)(void);
    /* A process by pid, or a thread by its id: 0 and the snapshot, or
     * negative for none. */
    int (*proc)(uint32_t pid, vibeos_procfs_proc_t *out);
    /* The smallest pid above `after` that is a process, or 0. */
    uint32_t (*next_pid)(uint32_t after);
    /* The smallest id above `after` of a thread of process `pid`, or 0. */
    uint32_t (*next_tid)(uint32_t pid, uint32_t after);
    /* Its arguments, each ended by a NUL; bytes. */
    long (*cmdline)(uint32_t pid, char *buf, uint32_t cap);
    /* Where one of its links points (VIBEOS_PROCFS_LINK_*, `fd` for the
     * descriptor's); bytes, or a negated errno. */
    long (*link)(uint32_t pid, uint32_t which, uint32_t fd, char *buf, uint32_t cap);
    /* Its lowest open descriptor at or above `from`: 0 and *fd, or negative. */
    int (*next_fd)(uint32_t pid, uint32_t from, uint32_t *fd);
    /* Its `index`th region, in address order: 0, or negative past the last. */
    int (*map)(uint32_t pid, uint32_t index, vibeos_procfs_map_t *out);
    /* Whether the caller may look inside process `pid` - its maps and where
     * its links point: 0, or a negated errno (EACCES). Null lets everybody,
     * which is what a machine with one user and no source of credentials is. */
    int (*may_inspect)(uint32_t pid);
} vibeos_procfs_t;

/* The operations, for vibeos_fs_mount. */
const vibeos_fs_ops_t *vibeos_procfs_ops(void);

/* The process whose directory `node` is on the /proc mount `mnt` - Linux takes
 * an open /proc/<pid> as a pidfd - or 0 for any other node or mount. */
uint32_t vibeos_procfs_node_pid(const vibeos_fsmount_t *mnt, uint64_t node);

#endif
