#ifndef VIBEOS_LINUX_INTERNAL_H
#define VIBEOS_LINUX_INTERNAL_H

/* What the files in kernel/abi/linux share with each other.
 *
 * The Linux syscall handlers used to live in arch_hw.c as file-scope statics, and
 * after they were lifted out (C4) they still reached the architecture through its
 * private header. They do not any more (docs/abi/ phase A2): everything a handler
 * asks of the kernel is in vibeos/ksvc.h, and nothing here names a register, a
 * page-table bit or the task table. That is what lets the same files build into
 * the host test binary against tests/kernel/ksvc_fake.c. */

#include <stdint.h>
#include "vibeos/ksvc.h"
#include "vibeos/linux_exports.h"
#include "vibeos/abi_linux.h"
#include "vibeos/abi_rows.h"
#include "vibeos/procstate.h"
#include "vibeos/fdtable.h"
#include "vibeos/fileops.h"
#include "vibeos/filelock.h"
#include "vibeos/linux_layout.h"
#include "vibeos/pipe.h"
#include "vibeos/vma.h"
#include "vibeos/mm_stats.h"
#include "vibeos/swapmap.h"
#include "vibeos/swaparea.h"
#include "vibeos/task_stats.h"
#include "vibeos/lifetime.h"
#include "vibeos/ceildiv.h"
#include "vibeos/frame.h"

/* Rows are declared with the macros in vibeos/abi_rows.h, which every
 * personality shares; LINUX_DEFINE_SYSCALLS is the Linux spelling. */
#define LINUX_DEFINE_SYSCALLS(topic, LIST) VIBEOS_DEFINE_SYSCALLS(linux, topic, LIST)

#define VIBEOS_ARG_INT(v)  ((int)(uint32_t)(v))

/* How many descriptors a process may have: RLIMIT_NOFILE, as prlimit reports it. */
#define LINUX_MAX_FDS ((int)VIBEOS_FD_MAX)


/* The exec staging cache (common.c): which image the staging buffer holds. */
extern char g_exec_cached[128];
extern long g_exec_cached_len;
extern uint32_t g_exec_cached_id;
void linux_exec_cache_drop(void);

/* Descriptors (fs.c), shared with the socket calls: a reference to what fd names
 * (the caller puts it), a description installed at the lowest free number at or
 * above min (the reference is taken over, or released on failure), and a close. */
vibeos_file_t *linux_file_get(uint64_t fd);
long linux_fd_install(vibeos_file_t *f, uint32_t fdflags, uint32_t min);
long linux_fd_close(uint64_t fd);

/* A path argument walked (fs.c, docs/abi/ A4 and L1): relative to the directory
 * `dirfd` names, or the working directory for LINUX_AT_FDCWD, never above the root,
 * symbolic links followed as `flags` (VIBEOS_PATH_*) says. 0, or a negated errno. */
long linux_walk_at(uint64_t dirfd, uint64_t upath, uint32_t flags, vibeos_path_t *w);

/* The file a descriptor names, walked from the path its description remembers
 * (AT_FDCWD: the working directory), and a path that may be empty under
 * AT_EMPTY_PATH - then `dirfd` is the file. */
long linux_walk_fd(uint64_t fd, vibeos_path_t *w);
long linux_walk_at_empty(uint64_t dirfd, uint64_t upath, uint64_t atflags, uint32_t flags,
                         vibeos_path_t *w);

/* What fs.c and names.c share (docs/abi/ L1 step 5): what stat reports for a
 * name or a descriptor and the device it is on, open by another name, the
 * umask, and rmdir for unlinkat's AT_REMOVEDIR. */
long linux_stat_get(uint64_t dirfd, uint64_t path_uptr, uint64_t atflags,
                    vibeos_file_stat_t *st, uint64_t *dev);
uint64_t linux_dev_of(const vibeos_fsmount_t *mnt);
long linux_sys_openat(uint64_t dirfd, uint64_t path_uptr, uint64_t flags, uint64_t mode);
uint32_t linux_umask(void);
long linux_rmdir_at(uint64_t dirfd, uint64_t path_uptr);

#endif
