/* NT personality: the dispatcher (docs/abi/windows.md W1).
 *
 * Find the service's row, gather its arguments where Windows' calling
 * convention leaves them, judge every pointer the row declares, run it. A
 * service with no row is answered STATUS_NOT_IMPLEMENTED and counted - as an
 * unassigned Linux number is answered ENOSYS - and a number past the table,
 * which names no service, STATUS_INVALID_SYSTEM_SERVICE. */

#include "nt_internal.h"

volatile uint64_t g_nt_unimplemented;
volatile uint64_t g_nt_refused;
volatile uint64_t g_nt_last_nr;

extern const vibeos_row_t nt_process_rows[];
extern const uint32_t nt_process_row_count;
extern const vibeos_row_t nt_file_rows[];
extern const uint32_t nt_file_row_count;
extern const vibeos_row_t nt_system_rows[];
extern const uint32_t nt_system_row_count;
extern const vibeos_row_t nt_object_rows[];
extern const uint32_t nt_object_row_count;
extern const vibeos_row_t nt_sync_rows[];
extern const uint32_t nt_sync_row_count;
extern const vibeos_row_t nt_section_rows[];
extern const uint32_t nt_section_row_count;
extern const vibeos_row_t nt_memory_rows[];
extern const uint32_t nt_memory_row_count;
extern const vibeos_row_t nt_thread_rows[];
extern const uint32_t nt_thread_row_count;

static const struct {
    const vibeos_row_t *rows;
    const uint32_t *count;
} g_tables[] = {
    { nt_process_rows, &nt_process_row_count },
    { nt_file_rows, &nt_file_row_count },
    { nt_system_rows, &nt_system_row_count },
    { nt_object_rows, &nt_object_row_count },
    { nt_sync_rows, &nt_sync_row_count },
    { nt_section_rows, &nt_section_row_count },
    { nt_memory_rows, &nt_memory_row_count },
    { nt_thread_rows, &nt_thread_row_count },
};

int nt_user_ok(uint64_t base, uint64_t len, int write) {
    return ks_user_ok(base, len, write);
}

/* The only two places this personality touches a program's memory: the range
 * judged as the program's, then copied fault-safely. The row's check covers
 * the arguments it declares; a pointer found inside a structure - a
 * UNICODE_STRING's buffer, a rename's name - is a program's too, and the
 * copy alone is fault-safe, not range-checked: ring 0 reads any mapped page,
 * so an unjudged kernel address read back through a listing's mask made it an
 * oracle on kernel memory (v7 review). check-nt-user-copy.py keeps every
 * other copy out of kernel/abi/nt. */
int nt_copy_in(void *dst, uint64_t src, uint64_t len) {
    if (len == 0u) {
        return 0;
    }
    if (!nt_user_ok(src, len, 0)) {
        return -1;
    }
    return vibeos_uaccess_copy(dst, (const void *)(uintptr_t)src, len);
}

int nt_copy_out(uint64_t dst, const void *src, uint64_t len) {
    if (len == 0u) {
        return 0;
    }
    if (!nt_user_ok(dst, len, 1)) {
        return -1;
    }
    return vibeos_uaccess_copy((void *)(uintptr_t)dst, src, len);
}

/* Every table, once, before the first NT process exists - and the rows held
 * against the registry in both directions, as the Linux tables are: a row for a
 * service the .def calls MISSING, or a DONE line with no row, is a kernel whose
 * documents and behaviour disagree, and it stops here rather than booting. */
void vibeos_nt_abi_init(void) {
    uint32_t i;

    vibeos_abi_nt_reset();
    nt_ob_reset();
    for (i = 0; i < (uint32_t)(sizeof(g_tables) / sizeof(g_tables[0])); i++) {
        if (vibeos_abi_nt_register(g_tables[i].rows, *g_tables[i].count) != 0) {
            ks_log(VIBEOS_LOG_ERROR, 64u, i, 0,
                   "nt abi: a service is claimed twice, or a row is malformed");
            ks_panic("nt abi: service table refused");
        }
    }
    i = vibeos_abi_nt_disagreement();
    if (i != (uint32_t)VIBEOS_NT_SERVICES) {
        ks_log(VIBEOS_LOG_ERROR, 65u, i, 0,
               "nt abi: a row and nt_services.def disagree about this service");
        ks_panic("nt abi: registry and rows disagree");
    }
}

long nt_syscall(struct ks_regs *frame, uint64_t nr, const uint64_t a[6]) {
    const vibeos_nt_service_t *s = vibeos_nt_service(nr);
    const vibeos_row_t *row;
    vibeos_call_t call;
    uint32_t i;

    if (!s) {
        return NT_STATUS(VIBEOS_STATUS_INVALID_SYSTEM_SERVICE);
    }
    row = vibeos_abi_nt()->lookup(nr);
    if (!row) {
        if (s->state == VIBEOS_NT_REFUSED) {
            __sync_fetch_and_add(&g_nt_refused, 1u);
            return NT_STATUS(VIBEOS_STATUS_NOT_IMPLEMENTED);
        }
        /* Not written yet: counted, and named on the console, because a program
         * asking for it is news - the step that owns it is the next to plan. */
        __sync_fetch_and_add(&g_nt_unimplemented, 1u);
        g_nt_last_nr = nr;
        ks_con_lock();
        ks_con_puts("[HW][SYS] unimplemented NT service nr=0x");
        ks_con_hex(nr);
        ks_con_puts(" ");
        ks_con_puts(s->name);
        ks_con_puts("\n");
        ks_con_unlock();
        return NT_STATUS(VIBEOS_STATUS_NOT_IMPLEMENTED);
    }

    for (i = 0; i < VIBEOS_CALL_ARGS; i++) {
        call.a[i] = i < 4u ? a[i] : 0u;
    }
    call.frame = frame;
    /* Past the fourth, the arguments are on the caller's stack, where Windows'
     * convention leaves them: above the return address the stub's caller
     * pushed and the thirty-two bytes of space it reserved for the first four.
     * Copied in one piece, judged first; a stack that is not there is the
     * caller's access violation, as on Windows. */
    if (s->args > 4u) {
        uint64_t at = ks_regs_sp(frame) + 0x28u;
        uint64_t len = (uint64_t)(s->args - 4u) * 8u;

        if (s->args > VIBEOS_CALL_ARGS || !nt_user_ok(at, len, 0) ||
            nt_copy_in(&call.a[4], at, len) != 0) {
            return NT_STATUS(VIBEOS_STATUS_ACCESS_VIOLATION);
        }
    }
    if (vibeos_abi_check_pointers(row, &call, nt_user_ok, 1) != 0) {
        return NT_STATUS(VIBEOS_STATUS_ACCESS_VIOLATION);
    }
    return row->handler(&call);
}
