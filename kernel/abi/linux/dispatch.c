/* Linux ABI: the dispatcher - find the syscall's row, then run it.
 *
 * There is no list of syscalls here. Each file that implements some declares them
 * in its own table (LINUX_DEFINE_SYSCALLS, linux_internal.h), and this file only
 * knows which tables exist. Adding a syscall to an existing file therefore never
 * touches this one; adding a *file* of them adds one line to g_tables below.
 */

#include "linux_internal.h"

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
};

/* The single call site of hw_user_range_ok. Rows declare their pointer arguments
 * with USER_OUT / USER_IN / USER_OUT_OPT (linux_internal.h) and handlers whose
 * range depends on data they have only just read (an iovec base, a string, a
 * sockaddr) ask here; nothing else in the kernel calls the check itself. */
int linux_user_ok(uint64_t base, uint64_t len, int write) {
    return hw_user_range_ok(base, len, write);
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
            hw_log(VIBEOS_LOG_ERROR, 60u, i, 0,
                   "linux abi: a syscall number is claimed twice, or a row is malformed");
            hw_panic("linux abi: syscall table refused");
        }
    }
    if (vibeos_abi_linux_missing() != VIBEOS_OP_NONE) {
        hw_log(VIBEOS_LOG_ERROR, 61u, (uint64_t)vibeos_abi_linux_missing(), 0,
               "linux abi: an operation declared in abi.h has no syscall");
        hw_panic("linux abi: a declared operation has no handler");
    }
}

/* Linux ABI entry: nr in rax, args in rdi/rsi/rdx/r10/r8/r9, with the full
 * trapframe available (fork needs it). Reached from both the native `syscall`
 * trampoline and the int 0x80 gate. */
long vibeos_x86_64_linux_syscall(vibeos_x86_64_isr_frame_t *frame,
                                 uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3) {
    /* The ABI was bound to this task when it was created; it is not looked up
     * per call. A task with no ABI recorded (there is none on the live path) is
     * treated as Linux, the only one there is. */
    const vibeos_abi_t *abi = (g_current_task >= 0 && g_tasks[g_current_task].abi)
                                  ? g_tasks[g_current_task].abi
                                  : vibeos_abi_linux();
    const vibeos_row_t *row = abi->lookup(nr);

    if (row) {
        vibeos_call_t call;

        call.a[0] = a1;
        call.a[1] = a2;
        call.a[2] = a3;
        call.a[3] = frame->r10;
        call.a[4] = frame->r8;
        call.a[5] = frame->r9;
        call.frame = frame;
        return row->handler(&call);
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
    vibeos_x86_64_serial_lock();
    vibeos_x86_64_serial_puts("[HW][SYS] unimplemented Linux syscall nr=0x");
    vibeos_x86_64_serial_print_hex(nr);
    vibeos_x86_64_serial_puts("\n");
    vibeos_x86_64_serial_unlock();
    return -VIBEOS_ENOSYS;
}
