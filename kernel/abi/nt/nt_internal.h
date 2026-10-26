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
/* Copies to and from the calling program's memory: 0, or -1 for a range that
 * is not the program's or a page that faulted (dispatch.c). */
int nt_copy_in(void *dst, uint64_t src, uint64_t len);
int nt_copy_out(uint64_t dst, const void *src, uint64_t len);

/* A handle, as W1 has them: a descriptor of the process's table, (fd + 1) * 4.
 * Windows' handles are multiples of four counted from four, and the program's
 * standard handles - set from descriptors 0, 1 and 2 when it starts - are the
 * ones W1 uses. W2's object manager replaces this with handles of their own;
 * until then a handle and a descriptor are the same entry, which is why NT and
 * Linux code may share the table without disagreeing about it. The reference
 * returned is the caller's to put. */
vibeos_file_t *nt_file_get(uint64_t handle, uint32_t need, uint32_t *granted);
uint64_t nt_handle_of_fd(uint32_t fd);

/* An errno a file operation answered, as the NTSTATUS Windows gives for it. */
uint32_t nt_status_of_errno(long err);

/* ---- the object manager (object.c, W2) --------------------------------------------- */

#define NT_OB_MAX 256u
/* The most objects one process may have made and still have alive: a quarter of
 * the table, as M-092 bounds the timers - one process making events in a loop
 * took all 256 and every other process's NtCreate failed (M-110). */
#define NT_OB_PER_PROCESS (NT_OB_MAX / 4u)
#define NT_NAME_MAX 128u   /* characters of a whole path; more is refused */

struct nt_ob;

/* An NT object type: a description type's operations carry one. */
typedef struct vibeos_nt_type {
    const char *name;            /* "Event", as NtQueryObject names it        */
    uint32_t all_access;         /* what GENERIC_ALL and MAXIMUM_ALLOWED give */
    uint32_t generic_read;
    uint32_t generic_write;
    uint32_t generic_execute;
    /* Waiting, under the object lock: is it signalled for thread `tid`, and
     * what acquiring it costs. Null for a type nobody waits on. */
    int (*signaled)(struct nt_ob *o, uint32_t tid);
    uint32_t (*acquire)(struct nt_ob *o, uint32_t tid);   /* a WAIT_0 or ABANDONED_WAIT_0 offset */
} nt_type_t;

/* One object's state. The description is the object; this is what it holds. */
typedef struct nt_ob {
    uint32_t used;
    uint32_t owner;               /* the process that made it, for its share     */
    vibeos_file_t *file;          /* no reference: the description owns the slot */
    uint16_t name[NT_NAME_MAX];
    uint32_t name_len;            /* characters; 0 for an unnamed object         */
    uint32_t name_ci;
    uint32_t waiters;             /* threads in a wait naming it now             */
    union {
        struct {
            uint32_t kind;        /* VIBEOS_NT_NOTIFICATION_EVENT or _SYNCHRONIZATION_ */
            int32_t state;
            uint32_t pulses;      /* every NtPulseEvent: a waiter that saw it begin
                                   * and end while it waited is satisfied by it    */
        } ev;
        struct {
            uint32_t owner;       /* thread id, 0 when free                     */
            uint32_t count;       /* recursion                                   */
            uint32_t abandoned;
        } mu;
        struct {
            int32_t count;
            int32_t max;
        } sem;
        struct {
            vibeos_file_t *file;  /* a reference: the image's file             */
            uint64_t image_size;  /* an image's span, a data section's size */
            uint32_t data;        /* a data section (W3), not an image        */
            uint32_t shared;      /* its views map the file's own pages       */
            uint32_t protect;     /* the section's page protection            */
            uint32_t attributes;  /* SEC_*, as NtQuerySection reports them    */
        } sec;
    } u;
} nt_ob_t;

typedef struct {
    uint16_t chars[NT_NAME_MAX];
    uint32_t len;
    int insensitive;
} nt_name_t;

void nt_ob_lock(void);
void nt_ob_unlock(void);
void nt_ob_reset(void);
nt_ob_t *nt_ob_of(const vibeos_file_t *f);
void nt_ob_release(vibeos_file_t *f);   /* a type's release */

/* OBJECT_ATTRIBUTES from a program: the name and the attributes. A status. */
uint32_t nt_read_attributes(uint64_t oa, nt_name_t *out, uint32_t *attributes);
/* A new object of `ops`'s type, or - for a name that exists - the one there,
 * with OBJECT_NAME_EXISTS; the reference is the caller's. */
uint32_t nt_ob_create(const vibeos_file_ops_t *ops, const nt_name_t *name, uint32_t attributes,
                      vibeos_file_t **out, int *existed,
                      void (*init)(struct nt_ob *o, const void *ctx), const void *ctx);
uint32_t nt_ob_open(const vibeos_file_ops_t *ops, const nt_name_t *name, vibeos_file_t **out);
/* What `want` grants of `ops`'s type: the generic rights mapped onto its own. */
uint32_t nt_map_access(const vibeos_file_ops_t *ops, uint32_t want);
uint32_t nt_map_access_with(const nt_type_t *t, uint32_t want);
/* Reset (set = 0) or set an event handle for an I/O (sync.c). */
uint32_t nt_io_event(uint64_t h, int set);
/* An NT path - "\??\C:\..." - as the VFS path it names. A status. */
uint32_t nt_vfs_path(const nt_name_t *n, char *out, uint32_t cap);
/* A handle for `f`, taking over the caller's reference (released on failure). */
uint32_t nt_handle_install(vibeos_file_t *f, uint32_t access, uint32_t attributes,
                           uint64_t *handle);
/* The description `h` names - of `ops`'s type, if not null - and the handle's
 * granted rights, which must include `need`. The reference is the caller's. */
uint32_t nt_handle_ref(uint64_t h, const vibeos_file_ops_t *ops, uint32_t need,
                       vibeos_file_t **out, uint32_t *granted);
uint32_t nt_close_handle(uint64_t h);

/* ---- memory (memory.c, W3) ---------------------------------------------------------- */

/* A PAGE_* protection as a region's, or -1 for one Windows refuses. `copy_ok`:
 * whether the copy-on-write pair is allowed - by views and protection changes,
 * not by private memory being allocated. */
int nt_prot_to_region(uint32_t page, int copy_ok, vibeos_prot_t *out);

/* ---- creating objects and waiting on them (sync.c) ---------------------------------- */

/* NtCreate<type> and NtOpen<type>'s shared part. `init` sets a new object's
 * state from `args`; an existing named object keeps its own. */
long nt_create_common(const vibeos_file_ops_t *ops, uint64_t handle_u, uint32_t want,
                      uint64_t oa, int open_only,
                      void (*init)(nt_ob_t *o, const void *ctx), const void *ctx);
/* Wait for `n` handles - all, or any - until the timeout (a user pointer to an
 * NT time, 0 for none). An NTSTATUS. */
long nt_wait(uint32_t n, const uint64_t *handles, int all, uint64_t timeout_u);
extern const vibeos_file_ops_t g_nt_event_ops;

#endif
