#ifndef VIBEOS_ABI_H
#define VIBEOS_ABI_H

#include <stdint.h>

/* The kernel's own operations, and the ABIs that translate onto them.
 *
 * Named "op", not "syscall", because the word already belonged to the dispatcher
 * C3 deleted: its vocabulary (syscall.h, syscall_abi.h, syscall_policy.h) outlived
 * it as 1,000 lines nobody used, and was removed with C3's last increment. There
 * is one vocabulary now, and this is it.
 *
 * docs/core/phases.md, C4. Until now the only vocabulary was Linux's numbers:
 * the dispatcher switched on them, and *which checks a call performs* was a
 * property of whatever its handler happened to do - visible only by reading the
 * handler. A syscall that quietly stopped validating its pointer was a hole
 * that nothing else in the tree would notice.
 *
 * Here a syscall is declared once, in the list below, with the checks that
 * apply to it. The list is an X-macro so the enum, the name table and the
 * check table cannot drift apart, and so a line without its checks does not
 * compile. scripts/dev/check-syscall-checks.py then holds the declaration
 * against the code in both directions: a check declared and not run, or run and
 * not declared, fails the build's check rather than a review.
 *
 * The ABI is bound to a process when it is created, not looked up per call
 * (C0's [PERF] syscall histogram is what shows that claim is true).
 */

/* What a syscall must do before it trusts its arguments. These name the
 * kernel's choke points (scripts/dev/check-chokepoints.py), not a wish list:
 * a declaration is verified against the handler's call graph. */
#define VIBEOS_CHECK_NONE          0u
#define VIBEOS_CHECK_USER_MEMORY   (1u << 0)   /* reaches hw_user_range_ok / _why / hw_user_addr_ok */
#define VIBEOS_CHECK_SIGNAL_PERMIT (1u << 1)   /* reaches hw_signal_permitted: who may signal whom  */
#define VIBEOS_CHECK_TASK_GUARD    (1u << 2)   /* reaches hw_task_alloc_guarded: the fork guard     */

/* X(ID, "name", checks). The order is the enum order; ids are not ABI. */
#define VIBEOS_OP_LIST(X) \
    /* processes */ \
    X(FORK,            "fork",            VIBEOS_CHECK_TASK_GUARD) \
    X(THREAD_CREATE,   "thread_create",   VIBEOS_CHECK_TASK_GUARD | VIBEOS_CHECK_USER_MEMORY) \
    X(EXEC,            "exec",            VIBEOS_CHECK_USER_MEMORY) \
    X(EXIT,            "exit",            VIBEOS_CHECK_USER_MEMORY) \
    X(EXIT_GROUP,      "exit_group",      VIBEOS_CHECK_USER_MEMORY) \
    X(WAIT,            "wait",            VIBEOS_CHECK_USER_MEMORY) \
    X(GETPID,          "getpid",          VIBEOS_CHECK_NONE) \
    X(GETTID,          "gettid",          VIBEOS_CHECK_NONE) \
    X(GETPPID,         "getppid",         VIBEOS_CHECK_NONE) \
    X(SETPGID,         "setpgid",         VIBEOS_CHECK_NONE) \
    X(GETPGRP,         "getpgrp",         VIBEOS_CHECK_NONE) \
    X(SETSID,          "setsid",          VIBEOS_CHECK_NONE) \
    X(GETSID,          "getsid",          VIBEOS_CHECK_NONE) \
    X(YIELD,           "yield",           VIBEOS_CHECK_NONE) \
    X(SET_TID_ADDRESS, "set_tid_address", VIBEOS_CHECK_NONE) \
    X(ARCH_PRCTL,      "arch_prctl",      VIBEOS_CHECK_USER_MEMORY) \
    X(PRCTL,           "prctl",           VIBEOS_CHECK_USER_MEMORY) \
    X(PRLIMIT,         "prlimit",         VIBEOS_CHECK_USER_MEMORY) \
    X(IDENTITY_GET,    "identity_get",    VIBEOS_CHECK_NONE) \
    X(IDENTITY_SET,    "identity_set",    VIBEOS_CHECK_NONE) \
    /* files and descriptors */ \
    X(OPEN,            "open",            VIBEOS_CHECK_USER_MEMORY) \
    X(OPEN_AT,         "open_at",         VIBEOS_CHECK_USER_MEMORY) \
    X(CLOSE,           "close",           VIBEOS_CHECK_NONE) \
    X(READ,            "read",            VIBEOS_CHECK_USER_MEMORY) \
    X(WRITE,           "write",           VIBEOS_CHECK_USER_MEMORY) \
    X(READV,           "readv",           VIBEOS_CHECK_USER_MEMORY) \
    X(WRITEV,          "writev",          VIBEOS_CHECK_USER_MEMORY) \
    X(LSEEK,           "lseek",           VIBEOS_CHECK_NONE) \
    X(GETDENTS,        "getdents",        VIBEOS_CHECK_USER_MEMORY) \
    X(UNLINK,          "unlink",          VIBEOS_CHECK_USER_MEMORY) \
    X(MKDIR,           "mkdir",           VIBEOS_CHECK_USER_MEMORY) \
    X(FSTAT,           "fstat",           VIBEOS_CHECK_USER_MEMORY) \
    X(STAT_AT,         "stat_at",         VIBEOS_CHECK_USER_MEMORY) \
    X(READLINK_AT,     "readlink_at",     VIBEOS_CHECK_USER_MEMORY) \
    X(GETCWD,          "getcwd",          VIBEOS_CHECK_USER_MEMORY) \
    X(IOCTL,           "ioctl",           VIBEOS_CHECK_USER_MEMORY) \
    X(DUP,             "dup",             VIBEOS_CHECK_NONE) \
    X(DUP2,            "dup2",            VIBEOS_CHECK_NONE) \
    X(PIPE,            "pipe",            VIBEOS_CHECK_USER_MEMORY) \
    X(PIPE2,           "pipe2",           VIBEOS_CHECK_USER_MEMORY) \
    X(SENDFILE,        "sendfile",        VIBEOS_CHECK_NONE) \
    /* memory */ \
    X(BRK,             "brk",             VIBEOS_CHECK_NONE) \
    X(MAP,             "map",             VIBEOS_CHECK_NONE) \
    X(PROTECT,         "protect",         VIBEOS_CHECK_NONE) \
    X(UNMAP,           "unmap",           VIBEOS_CHECK_NONE) \
    X(PAGEINFO,        "pageinfo",        VIBEOS_CHECK_USER_MEMORY) \
    /* signals */ \
    X(SIG_ACTION,      "sig_action",      VIBEOS_CHECK_USER_MEMORY) \
    X(SIG_PROCMASK,    "sig_procmask",    VIBEOS_CHECK_USER_MEMORY) \
    X(SIG_RETURN,      "sig_return",      VIBEOS_CHECK_USER_MEMORY) \
    X(KILL,            "kill",            VIBEOS_CHECK_SIGNAL_PERMIT) \
    X(TKILL,           "tkill",           VIBEOS_CHECK_SIGNAL_PERMIT) \
    X(TGKILL,          "tgkill",          VIBEOS_CHECK_SIGNAL_PERMIT) \
    /* synchronisation and time */ \
    X(FUTEX,           "futex",           VIBEOS_CHECK_USER_MEMORY) \
    X(SET_ROBUST_LIST, "set_robust_list", VIBEOS_CHECK_NONE) \
    X(RSEQ,            "rseq",            VIBEOS_CHECK_NONE) \
    X(CLOCK_GETTIME,   "clock_gettime",   VIBEOS_CHECK_USER_MEMORY) \
    X(TIME,            "time",            VIBEOS_CHECK_USER_MEMORY) \
    X(UNAME,           "uname",           VIBEOS_CHECK_USER_MEMORY) \
    X(GETRANDOM,       "getrandom",       VIBEOS_CHECK_NONE) \
    /* network */ \
    X(SOCKET,          "socket",          VIBEOS_CHECK_NONE) \
    X(CONNECT,         "connect",         VIBEOS_CHECK_USER_MEMORY) \
    X(ACCEPT,          "accept",          VIBEOS_CHECK_USER_MEMORY) \
    X(SENDTO,          "sendto",          VIBEOS_CHECK_USER_MEMORY) \
    X(RECVFROM,        "recvfrom",        VIBEOS_CHECK_USER_MEMORY) \
    X(BIND,            "bind",            VIBEOS_CHECK_USER_MEMORY) \
    X(LISTEN,          "listen",          VIBEOS_CHECK_NONE) \
    X(NETCTL,          "netctl",          VIBEOS_CHECK_USER_MEMORY)

typedef enum vibeos_op_id {
    VIBEOS_OP_NONE = 0,   /* the ABI has no such call: the caller gets ENOSYS */
#define VIBEOS_OP_ENUM(id, name, checks) VIBEOS_OP_##id,
    VIBEOS_OP_LIST(VIBEOS_OP_ENUM)
#undef VIBEOS_OP_ENUM
    VIBEOS_OP_COUNT
} vibeos_op_id_t;

typedef struct vibeos_op_entry {
    vibeos_op_id_t id;
    const char *name;
    uint32_t checks;   /* VIBEOS_CHECK_* */
} vibeos_op_entry_t;

/* The declaration for an id, or NULL for NONE / out of range. */
const vibeos_op_entry_t *vibeos_op_entry(vibeos_op_id_t id);

/* An ABI: how a foreign syscall number becomes the kernel's vocabulary.
 * `classify` decides which of the kernel's operations a call means; it may look at
 * the first argument, because Linux's clone() is a process or a thread by its
 * flags. Argument marshalling stays with the
 * ABI's dispatcher for now, and moves behind `marshal` when a second ABI needs
 * it, not before. */
typedef struct vibeos_abi {
    const char *name;
    vibeos_op_id_t (*classify)(uint64_t nr, uint64_t a1);
} vibeos_abi_t;

/* The Linux x86-64 ABI, the first and today only translator. */
const vibeos_abi_t *vibeos_abi_linux(void);

#endif
