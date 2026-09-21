#ifndef VIBEOS_LINUX_INTERNAL_H
#define VIBEOS_LINUX_INTERNAL_H

/* What the files in kernel/abi/linux share with each other.
 *
 * The Linux syscall handlers used to live in arch_hw.c as file-scope statics. They
 * still reach the architecture through arch_hw_internal.h - a task, its address
 * space, its descriptors are x86-64 objects today - and this header is only what
 * the handlers themselves pass between files. */

#include "../../arch/x86_64/arch_hw_internal.h"
#include <stdint.h>
#include "vibeos/arch_x86_64.h"
#include "vibeos/trap.h"
#include "vibeos/boot.h"
#include "vibeos/mm.h"
#include "vibeos/inet.h"
#include "vibeos/elf.h"
#include "vibeos/services.h"
#include "vibeos/exec_stats.h"
#include "vibeos/account.h"
#include "vibeos/forkguard.h"
#include "vibeos/sched_policy.h"
#include "vibeos/pageinfo.h"
#include "vibeos/rmap.h"
#include "vibeos/reclaim.h"
#include "vibeos/mbz.h"
#include "vibeos/abi.h"
#include "vibeos/abi_linux.h"
#include "vibeos/ceildiv.h"
#include "vibeos/blkdev.h"
#include "vibeos/io_stats.h"
#include "vibeos/blockdev.h"
#include "vibeos/partition.h"
#include "vibeos/parttab.h"
#include "vibeos/ext2.h"
#include "vibeos/iso9660.h"
#include "vibeos/exfat.h"
#include "vibeos/ntfs.h"
#include "vibeos/logsink.h"
#include "vibeos/storage.h"
#include "vibeos/swapmap.h"
#include "vibeos/anon.h"
#include "vibeos/swaparea.h"
#include "vibeos/log.h"
#include "vibeos/mm_model.h"
#include "vibeos/frame.h"
#include "vibeos/vmspace.h"
#include "vibeos/vma.h"
#include "vibeos/backing.h"
#include "vibeos/task_stats.h"
#include "vibeos/task.h"
#include "vibeos/runq.h"
#include "vibeos/lifetime.h"
#include "vibeos/vfs.h"

/* ---- declaring a syscall ------------------------------------------------------
 *
 * A syscall is one line in a list at the bottom of the file that holds its handler:
 *
 *     X(0, read, READ, hw_sys_read(ARG(0), ARG(1), ARG(2)))
 *
 * the Linux number, a name, the kernel operation (declared, with its checks, in
 * include/vibeos/abi.h) and the call that runs it. ARG(i) is the i-th argument in
 * the order the ABI passes them - rdi, rsi, rdx, r10, r8, r9 - and FRAME is the
 * trapframe for the calls that need it. Writing the marshalling out in the row
 * is the point: which register becomes which parameter is right there, and is
 * what a reader checks against the manual.
 *
 * LINUX_DEFINE_SYSCALLS turns the list into an adapter per row and the table of
 * rows the dispatcher registers. Adding a syscall to an existing file is therefore
 * a line in abi.h and a line here. */
#define ARG(i) (c->a[(i)])
#define FRAME ((vibeos_x86_64_isr_frame_t *)c->frame)

/* ---- checking a pointer argument in the row ------------------------------------
 *
 * A row may wrap its call in USER_OUT / USER_IN / USER_OUT_OPT to have the user
 * pointer validated *before* the handler runs, so the handler (and anything that
 * reaches it) can assume the range is good:
 *
 *     X(63, uname, UNAME, USER_OUT(ARG(0), 390, hw_sys_uname(ARG(0))))
 *
 * OUT is a range the kernel will write, IN one it only reads, OPT accepts a null
 * pointer (the handler then skips it). A refusal is -EFAULT, the same answer the
 * handler gave, at the same point: only a check that was the first thing a
 * handler did, with nothing before it that could return a different error, is
 * hoisted, because moving one past an EBADF or EINVAL changes which error a
 * program sees. A handler another handler also calls (write, from writev) keeps
 * its own check. The check is written out in the row, so a reader sees what the
 * syscall promises about its arguments without opening the handler. */
#define USER_OUT(ptr, len, call)     (linux_user_ok((ptr), (len), 1) ? (long)(call) : (long)-VIBEOS_EFAULT)
#define USER_IN(ptr, len, call)     (linux_user_ok((ptr), (len), 0) ? (long)(call) : (long)-VIBEOS_EFAULT)
#define USER_OUT_OPT(ptr, len, call)     (((ptr) == 0u || linux_user_ok((ptr), (len), 1)) ? (long)(call) : (long)-VIBEOS_EFAULT)

#define LINUX_ADAPTER(nr, name, op, expr) \
    static long linux_h_##name(const vibeos_call_t *c) { (void)c; return (long)(expr); }
#define LINUX_ROW(nr, name, op, expr) { (nr), #name, VIBEOS_OP_##op, linux_h_##name },
#define LINUX_DEFINE_SYSCALLS(topic, LIST) \
    LIST(LINUX_ADAPTER) \
    const vibeos_row_t linux_##topic##_rows[] = { LIST(LINUX_ROW) }; \
    const uint32_t linux_##topic##_row_count = \
        (uint32_t)(sizeof(linux_##topic##_rows) / sizeof(linux_##topic##_rows[0]));

#define VIBEOS_ARG_INT(v)  ((int)(uint32_t)(v))

extern char g_exec_cached[128];
extern long g_exec_cached_len;
extern uint32_t g_exec_cached_id;
void hw_exec_cache_drop(void);
void hw_mm_lock(hw_procstate_t *ps);
void hw_mm_unlock(hw_procstate_t *ps);
long vibeos_x86_64_linux_syscall(vibeos_x86_64_isr_frame_t *frame,
                                 uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3);

#endif
