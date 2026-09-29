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

/* The descriptor numbers the table covers: 0-2 are the console (or what they
 * were redirected to), 3 up are entries. */
#define LINUX_MAX_FDS ((int)VIBEOS_FD_SLOTS)

/* A network wait gives up after ten seconds of ticks. */
#define LINUX_NET_TIMEOUT_SECONDS 10u

/* The exec staging cache (common.c): which image the staging buffer holds. */
extern char g_exec_cached[128];
extern long g_exec_cached_len;
extern uint32_t g_exec_cached_id;
void linux_exec_cache_drop(void);

/* Shared between fs.c and net.c: a stream on a socket, and claiming a descriptor. */
long linux_net_recv(vibeos_fd_t *f, uint64_t buf, uint64_t len);
long linux_net_send(vibeos_fd_t *f, uint64_t buf, uint64_t len);
int linux_fd_alloc(int slot);
vibeos_fd_t *linux_fd_get(uint64_t fd);
void linux_pipe_release(vibeos_fd_t *f);

#endif
