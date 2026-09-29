/* Linux ABI: the dispatcher - find the syscall's row, then run it.
 *
 * There is no list of syscalls here. Each file that implements some declares them
 * in its own table (LINUX_DEFINE_SYSCALLS, linux_internal.h), and this file only
 * knows which tables exist. Adding a syscall to an existing file therefore never
 * touches this one; adding a *file* of them adds one line to g_tables below.
 */

#include "linux_internal.h"

/* The counters the boot's [ABI] MUSTBEZERO line reports. Defined by the layer
 * that counts them; the architecture only prints them.
 *
 * The ABI surface's must-be-zero: a syscall number the kernel does not
 * implement. musl probes some and tolerates ENOSYS, but nothing the boot runs
 * should reach one, and a program that does gets -ENOSYS and carries on
 * believing something worked. The boot asks for VIBEOS_ABI_PROBE_NR on purpose
 * (user/prog/hello.c) so the count is seen moving; the gate asserts
 * unimplemented == probes, and last_nr names the number when it is not. */
volatile uint64_t g_abi_unimplemented;
/* The two other ways a number has no row (kernel/abi/linux_syscalls.def): a
 * refusal by decision, expected and counted, and a deferred call, which the
 * gate reports by number because nothing planned for a program to ask. */
volatile uint64_t g_abi_refused;
volatile uint64_t g_abi_deferred;
volatile uint64_t g_abi_deferred_nr;
volatile uint64_t g_abi_probes;
volatile uint64_t g_abi_last_nr;

extern const vibeos_row_t linux_fs_rows[];
extern const uint32_t linux_fs_row_count;
extern const vibeos_row_t linux_mm_rows[];
extern const uint32_t linux_mm_row_count;
extern const vibeos_row_t linux_proc_rows[];
extern const uint32_t linux_proc_row_count;
extern const vibeos_row_t linux_sig_rows[];
extern const uint32_t linux_sig_row_count;
extern const vibeos_row_t linux_misc_rows[];
extern const uint32_t linux_misc_row_count;
extern const vibeos_row_t linux_net_rows[];
extern const uint32_t linux_net_row_count;
extern const vibeos_row_t linux_futex_rows[];
extern const uint32_t linux_futex_row_count;

static const struct {
    const char *name;
    const vibeos_row_t *rows;
    const uint32_t *count;
} g_tables[] = {
    { "fs",   linux_fs_rows,   &linux_fs_row_count },
    { "mm",   linux_mm_rows,   &linux_mm_row_count },
    { "proc", linux_proc_rows, &linux_proc_row_count },
    { "sig",  linux_sig_rows,  &linux_sig_row_count },
    { "misc", linux_misc_rows, &linux_misc_row_count },
    { "net",  linux_net_rows,  &linux_net_row_count },
    { "futex", linux_futex_rows, &linux_futex_row_count },
};

/* The single call site of ks_user_ok. Rows declare their pointer arguments
 * with OUT / IN / OUT_OPT (vibeos/abi_rows.h) and handlers whose
 * range depends on data they have only just read (an iovec base, a string, a
 * sockaddr) ask here; nothing else in the kernel calls the check itself. */
int linux_user_ok(uint64_t base, uint64_t len, int write) {
    return ks_user_ok(base, len, write);
}

/* Register every table, once, before the first user task exists. A number claimed
 * twice, or a kernel operation declared in abi.h that no file implements, is a
 * kernel that would answer some syscall wrongly for the rest of its life, so it
 * stops here with the reason rather than booting. */
void vibeos_linux_abi_init(void) {
    uint32_t i;


    vibeos_abi_linux_reset();
    for (i = 0; i < (uint32_t)(sizeof(g_tables) / sizeof(g_tables[0])); i++) {
        if (vibeos_abi_linux_register(g_tables[i].rows, *g_tables[i].count) != 0) {
            ks_log(VIBEOS_LOG_ERROR, 60u, i, 0,
                   "linux abi: a syscall number is claimed twice, or a row is malformed");
            ks_panic("linux abi: syscall table refused");
        }
    }
    /* The registry and the rows are two statements of what is implemented, and
     * they must agree in both directions: a row whose line says MISSING is a
     * table nobody updated, and a DONE line with no row answers ENOSYS while
     * every document says the call works. check-syscall-checks.py holds the
     * same rule against the sources; this holds it against what was built. */
    for (i = 0; i < vibeos_linux_syscall_count(); i++) {
        const vibeos_sys_entry_t *e = vibeos_linux_syscall_at(i);
        int has_row = vibeos_abi_linux()->lookup(e->nr) != 0;
        int says_row = e->state == VIBEOS_SYS_DONE || e->state == VIBEOS_SYS_PARTIAL;
        if (has_row != says_row) {
            ks_log(VIBEOS_LOG_ERROR, 62u, e->nr, (uint64_t)has_row,
                   "linux abi: a row and the registry disagree about this number");
            ks_panic("linux abi: registry and rows disagree");
        }
    }
    if (vibeos_abi_linux_missing() != VIBEOS_OP_NONE) {
        ks_log(VIBEOS_LOG_ERROR, 61u, (uint64_t)vibeos_abi_linux_missing(), 0,
               "linux abi: an operation declared in abi.h has no syscall");
        ks_panic("linux abi: a declared operation has no handler");
    }
}

/* One syscall. The architecture's entry reads the number and the six
 * arguments out of its registers - nr in rax, the rest in rdi, rsi, rdx, r10,
 * r8 and r9 on x86-64 - and hands them here with the trap frame, which fork,
 * execve and rt_sigreturn need and nothing else reads. Reached from both the
 * native `syscall` trampoline and the int 0x80 gate, and from the host tests. */
long linux_syscall(struct ks_regs *frame, uint64_t nr, const uint64_t a[6]) {
    /* The ABI was bound to this task when it was created; it is not looked up
     * per call. A task with no ABI recorded (there is none on the live path) is
     * treated as Linux, the only one there is. */
    int cur = ks_current();
    const vibeos_abi_t *abi = (cur >= 0 && ks_abi(cur)) ? ks_abi(cur)
                                                        : vibeos_abi_linux();
    const vibeos_row_t *row = abi->lookup(nr);

    if (row) {
        vibeos_call_t call;

        uint32_t i;

        for (i = 0; i < 6u; i++) {
            call.a[i] = a[i];
        }
        call.frame = frame;
        {
            long refused = vibeos_abi_check_pointers(row, &call, linux_user_ok,
                                                     VIBEOS_EFAULT);
            if (refused) {
                return refused;
            }
        }
        return row->handler(&call);
    }

    /* No row. The registry says which of the three kinds of no this is. */
    {
        const vibeos_sys_entry_t *e = vibeos_linux_syscall(nr);

        if (e && e->state == VIBEOS_SYS_REFUSED) {
            /* A decision, answered with its errno and counted. Not a failure
             * and not a log line: a C library probing for a facility this
             * kernel refused is behaving correctly. */
            __sync_fetch_and_add(&g_abi_refused, 1u);
            return -(long)e->err;
        }
        if (e && e->state == VIBEOS_SYS_DEFERRED) {
            /* Planned for nobody yet - so a program asking is news: the gate
             * reports the number, and the plan moves it into a phase. */
            __sync_fetch_and_add(&g_abi_deferred, 1u);
            g_abi_deferred_nr = nr;
            ks_con_lock();
            ks_con_puts("[HW][SYS] deferred Linux syscall nr=0x");
            ks_con_hex(nr);
            ks_con_puts(" ");
            ks_con_puts(e->name);
            ks_con_puts("\n");
            ks_con_unlock();
            return -(long)e->err;
        }
    }
    __sync_fetch_and_add(&g_abi_unimplemented, 1u);
    if (nr == VIBEOS_ABI_PROBE_NR) {
        __sync_fetch_and_add(&g_abi_probes, 1u);
        return -VIBEOS_ENOSYS;   /* asked for on purpose; no log line */
    }
    /* The witness: which number, not just how many - and only ever an
     * *unexpected* one. It used to be written before the probe test, so the
     * deliberate call for 1999 overwrote an accidental call for another
     * number and the report named the probe instead of the culprit. */
    g_abi_last_nr = nr;
    /* One line, one critical section: puts and print_hex each take the console
     * lock on their own. */
    ks_con_lock();
    ks_con_puts("[HW][SYS] unimplemented Linux syscall nr=0x");
    ks_con_hex(nr);
    ks_con_puts("\n");
    ks_con_unlock();
    return -VIBEOS_ENOSYS;
}
