#ifndef VIBEOS_NT_INTERNAL_H
#define VIBEOS_NT_INTERNAL_H

/* What the NT handlers share (docs/abi/windows.md W1).
 *
 * The same rules as the Linux handlers': nothing here names the architecture -
 * the kernel is reached through vibeos/ksvc.h alone, so these files run on the
 * host against tests/kernel/ksvc_fake.c - every user pointer is judged before it
 * is used, and every structure copied to or from a program is declared in
 * vibeos/nt_layout.h. */

#include <stdint.h>

#include "vibeos/ksvc.h"
#include "vibeos/abi.h"
#include "vibeos/abi_nt.h"
#include "vibeos/abi_rows.h"
#include "vibeos/nt_layout.h"
#include "vibeos/procstate.h"
#include "vibeos/fdtable.h"
#include "vibeos/file.h"
/* For the kernel's error numbers (VIBEOS_E*), which the file layer answers in
 * whichever personality asked - they are Linux's numbers because Linux was the
 * first to need them, and nt_status_of_errno is where NT translates them. */
#include "vibeos/abi_linux.h"

/* A table of rows, as LINUX_DEFINE_SYSCALLS makes one: X(nr, name, op, ptrs,
 * expr), with the number spelled VIBEOS_NT_NR_<name>. */
#define NT_DEFINE_SERVICES(topic, LIST) VIBEOS_DEFINE_SYSCALLS(nt, topic, LIST)

/* A buffer whose length is another argument. Windows' lengths are ULONG, and in
 * its calling convention the upper half of a register holding a 32-bit
 * argument is whatever the caller left there - so the length is read as 32
 * bits, never 64, by the descriptor and by the handler alike. */
#define NT_OUT_BUF(a, la) PTR_(a, VIBEOS_PTR_WRITE | VIBEOS_PTR_LEN32, (la) + 1, 1, 0, 0, 0)
#define NT_IN_BUF(a, la)  PTR_(a, VIBEOS_PTR_LEN32, (la) + 1, 1, 0, 0, 0)
#define NT_ARG32(i)       ((uint64_t)(uint32_t)ARG(i))

/* An NTSTATUS as a handler returns it: zero-extended, as rax carries it. */
#define NT_STATUS(s) ((long)(uint32_t)(s))

/* The one call site of ks_user_ok for this personality (dispatch.c). */
int nt_user_ok(uint64_t base, uint64_t len, int write);

/* A handle, as W1 has them: a descriptor of the process's table, (fd + 1) * 4.
 * Windows' handles are multiples of four counted from four, and the program's
 * standard handles - set from descriptors 0, 1 and 2 when it starts - are the
 * ones W1 uses. W2's object manager replaces this with handles of their own;
 * until then a handle and a descriptor are the same entry, which is why NT and
 * Linux code may share the table without disagreeing about it. The reference
 * returned is the caller's to put. */
vibeos_file_t *nt_file_get(uint64_t handle);
uint64_t nt_handle_of_fd(uint32_t fd);

/* An errno a file operation answered, as the NTSTATUS Windows gives for it. */
uint32_t nt_status_of_errno(long err);

#endif
