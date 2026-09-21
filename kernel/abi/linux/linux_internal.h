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
 *     X(0, read, READ, PTRS(OUT_BUF(1, 2)), hw_sys_read(ARG(0), ARG(1), ARG(2)))
 *
 * the Linux number, a name, the kernel operation (declared, with its checks, in
 * include/vibeos/abi.h), its pointer arguments (below; NOPTR for none) and the call
 * that runs it. ARG(i) is the i-th argument in
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

/* ---- declaring a pointer argument ---------------------------------------------
 *
 * The fourth column of a row is NOPTR, or PTRS(...) listing the pointer arguments
 * the kernel will touch (see vibeos_ptr_t in abi.h):
 *
 *     X(0, read, READ, PTRS(OUT_BUF(1, 2)), hw_sys_read(ARG(0), ARG(1), ARG(2)))
 *
 * OUT / IN are a range of fixed size the kernel writes / reads; the _BUF forms take
 * their length from another argument; the _OPT forms let a null pointer through; IN_VEC
 * is an array of `scale`-byte records whose count is an argument, with a cap above
 * which the handler's own EINVAL applies; the _IF forms apply only when argument
 * `wa` equals `wv`. Arguments are numbered from 0, in the order of ARG(). The
 * dispatcher runs them in the order written, so list them as the handler used to. */
#define PTR_(a, fl, la, n, cp, wa, wv) \
    { (uint8_t)(a), (uint8_t)((fl) | VIBEOS_PTR_LIVE), (uint8_t)(la), (uint8_t)(wa), \
      (uint32_t)(n), (uint32_t)(cp), (uint64_t)(wv) }
#define OUT(a, n)              PTR_(a, VIBEOS_PTR_WRITE, 0, n, 0, 0, 0)
#define IN(a, n)               PTR_(a, 0, 0, n, 0, 0, 0)
#define OUT_OPT(a, n)          PTR_(a, VIBEOS_PTR_WRITE | VIBEOS_PTR_OPT, 0, n, 0, 0, 0)
#define IN_OPT(a, n)           PTR_(a, VIBEOS_PTR_OPT, 0, n, 0, 0, 0)
#define OUT_BUF(a, la)         PTR_(a, VIBEOS_PTR_WRITE, (la) + 1, 1, 0, 0, 0)
#define IN_BUF(a, la)          PTR_(a, 0, (la) + 1, 1, 0, 0, 0)
#define IN_VEC(a, la, scale, cp) PTR_(a, 0, (la) + 1, scale, cp, 0, 0)
#define OUT_IF(wa, wv, a, n)   PTR_(a, VIBEOS_PTR_WRITE, 0, n, 0, (wa) + 1, wv)
#define IN_IF(wa, wv, a, n)    PTR_(a, 0, 0, n, 0, (wa) + 1, wv)
/* Applies when (argument wa & mask) == wv, and refuses with `e` instead of EFAULT. */
#define IN_IFM_ERR(wa, mask, wv, a, n, e)     { (uint8_t)(a), (uint8_t)(VIBEOS_PTR_LIVE), 0, (uint8_t)((wa) + 1),       (uint32_t)(n), 0, (uint64_t)(wv), (uint64_t)(mask), (uint32_t)(e) }
#define PTRS(...)              { __VA_ARGS__ }
#define NOPTR                  { { 0 } }

#define LINUX_ADAPTER(nr, name, op, ptrs, expr) \
    static long linux_h_##name(const vibeos_call_t *c) { (void)c; return (long)(expr); }
#define LINUX_ROW(nr, name, op, ptrs, expr) \
    { (nr), #name, VIBEOS_OP_##op, linux_h_##name, ptrs },
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
