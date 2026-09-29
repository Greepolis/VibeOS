#ifndef VIBEOS_ABI_ROWS_H
#define VIBEOS_ABI_ROWS_H

/* How a personality declares its syscalls: one row per number, beside the
 * handler it names. Shared by every ABI under kernel/abi/ - the Linux one today -
 * so a second personality writes rows, not a second copy of these macros. Only
 * for files that implement syscalls: the macros are short names on purpose. */

#include <stdint.h>
#include "vibeos/abi.h"
#include "vibeos/ksvc.h"

/* ---- declaring a syscall ------------------------------------------------------
 *
 * A syscall is one line in a list at the bottom of the file that holds its handler:
 *
 *     X(0, read, READ, PTRS(OUT_BUF(1, 2)), linux_sys_read(ARG(0), ARG(1), ARG(2)))
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
 * a line in abi.h and a line in that file. */
#define ARG(i) (c->a[(i)])
#define FRAME ((ks_regs_t *)c->frame)

/* ---- declaring a pointer argument ---------------------------------------------
 *
 * The fourth column of a row is NOPTR, or PTRS(...) listing the pointer arguments
 * the kernel will touch (see vibeos_ptr_t in abi.h):
 *
 *     X(0, read, READ, PTRS(OUT_BUF(1, 2)), linux_sys_read(ARG(0), ARG(1), ARG(2)))
 *
 * OUT / IN are a range of fixed size the kernel writes / reads; the _BUF forms take
 * their length from another argument; the _OPT forms let a null pointer through; IN_VEC
 * is an array of `scale`-byte records whose count is an argument, with a cap above
 * which the handler's own EINVAL applies; the _IF forms apply only when argument
 * `wa` equals `wv`. Arguments are numbered from 0, in the order of ARG(). The
 * dispatcher runs them in the order written, so list them as the handler used to. */
#define PTR_(a, fl, la, n, cp, wa, wv) \
    { (uint8_t)(a), (uint8_t)((fl) | VIBEOS_PTR_LIVE), (uint8_t)(la), (uint8_t)(wa), \
      (uint32_t)(n), (uint32_t)(cp), (uint64_t)(wv), 0, 0 }
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

#define VIBEOS_ABI_ADAPTER(nr, name, op, ptrs, expr) \
    static long abi_h_##name(const vibeos_call_t *c) { (void)c; return (long)(expr); }
#define VIBEOS_ABI_ROW(nr, name, op, ptrs, expr) \
    { (nr), #name, VIBEOS_OP_##op, abi_h_##name, ptrs },
/* `abi` and `topic` name the table: linux + fs is linux_fs_rows and
 * linux_fs_row_count, which the personality's dispatcher registers. */
#define VIBEOS_DEFINE_SYSCALLS(abi, topic, LIST) \
    LIST(VIBEOS_ABI_ADAPTER) \
    const vibeos_row_t abi##_##topic##_rows[] = { LIST(VIBEOS_ABI_ROW) }; \
    const uint32_t abi##_##topic##_row_count = \
        (uint32_t)(sizeof(abi##_##topic##_rows) / sizeof(abi##_##topic##_rows[0]));

#endif
