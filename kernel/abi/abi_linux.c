/* The Linux x86-64 ABI: the registry of its syscalls.
 *
 * A syscall is a row - number, kernel operation, handler - that lives beside the
 * handler it names, in whichever file that is. This file holds no list of syscalls
 * of its own: the files register their tables once at boot, and this keeps them
 * findable by number. Which operation a number is, and which checks it performs,
 * is declared once in include/vibeos/abi.h and is not repeated here.
 *
 * It is written only during boot, on one core, before any user task exists, and
 * read-only afterwards - so it needs no lock, and the readers take none. That
 * property is what keeps the lookup a plain array load on the syscall path.
 */

#include "vibeos/abi.h"

#define LINUX_LOW_NUMBERS 512u    /* every Linux number implemented is below this */
#define LINUX_HIGH_MAX 8u         /* VibeOS's own calls, outside the Linux number space */
#define LINUX_TABLES_MAX 16u

static const vibeos_row_t *g_by_nr[LINUX_LOW_NUMBERS];
static const vibeos_row_t *g_high[LINUX_HIGH_MAX];
static uint32_t g_high_count;

static const vibeos_row_t *g_tables[LINUX_TABLES_MAX];
static uint32_t g_table_len[LINUX_TABLES_MAX];
static uint32_t g_table_count;

static uint32_t g_rows_for_op[VIBEOS_OP_COUNT];
static uint32_t g_total_rows;

void vibeos_abi_linux_reset(void) {
    uint32_t i;
    for (i = 0; i < LINUX_LOW_NUMBERS; i++) {
        g_by_nr[i] = 0;
    }
    for (i = 0; i < LINUX_HIGH_MAX; i++) {
        g_high[i] = 0;
    }
    for (i = 0; i < (uint32_t)VIBEOS_OP_COUNT; i++) {
        g_rows_for_op[i] = 0;
    }
    g_high_count = 0;
    g_table_count = 0;
    g_total_rows = 0;
}

static const vibeos_row_t *find_row(uint64_t nr) {
    uint32_t i;
    if (nr < LINUX_LOW_NUMBERS) {
        return g_by_nr[nr];
    }
    for (i = 0; i < g_high_count; i++) {
        if (g_high[i]->nr == nr) {
            return g_high[i];
        }
    }
    return 0;
}

int vibeos_abi_linux_register(const vibeos_row_t *rows, uint32_t count) {
    uint32_t i;

    if (!rows || g_table_count >= LINUX_TABLES_MAX) {
        return -1;
    }
    /* Validate the whole table before taking any of it, so a refusal leaves the
     * registry as it was rather than half-populated. A number claimed twice - by
     * this table against itself, or against one registered earlier - is the only
     * thing that can make the ABI ambiguous, and it is refused here rather than
     * resolved by whichever file happened to register last. */
    for (i = 0; i < count; i++) {
        uint32_t j;
        if (!rows[i].handler || (uint32_t)rows[i].op >= (uint32_t)VIBEOS_OP_COUNT ||
            rows[i].op == VIBEOS_OP_NONE) {
            return -1;
        }
        if (find_row(rows[i].nr)) {
            return -1;
        }
        /* A descriptor naming an argument that does not exist would read whatever
         * lies past the call's six - refuse it here, at boot, not at first use. */
        for (j = 0; j < VIBEOS_PTR_MAX; j++) {
            const vibeos_ptr_t *d = &rows[i].ptr[j];
            if ((d->flags & VIBEOS_PTR_LIVE) &&
                (d->arg >= 6u || d->len_arg > 6u || d->when_arg > 6u)) {
                return -1;
            }
        }
        for (j = 0; j < i; j++) {
            if (rows[j].nr == rows[i].nr) {
                return -1;
            }
        }
        if (rows[i].nr >= LINUX_LOW_NUMBERS && g_high_count + 1u > LINUX_HIGH_MAX) {
            return -1;
        }
    }
    for (i = 0; i < count; i++) {
        if (rows[i].nr < LINUX_LOW_NUMBERS) {
            g_by_nr[rows[i].nr] = &rows[i];
        } else {
            g_high[g_high_count++] = &rows[i];
        }
        g_rows_for_op[rows[i].op]++;
        g_total_rows++;
    }
    g_tables[g_table_count] = rows;
    g_table_len[g_table_count] = count;
    g_table_count++;
    return 0;
}

vibeos_op_id_t vibeos_abi_linux_missing(void) {
    uint32_t i;
    for (i = 1; i < (uint32_t)VIBEOS_OP_COUNT; i++) {
        if (g_rows_for_op[i] == 0u) {
            return (vibeos_op_id_t)i;
        }
    }
    return VIBEOS_OP_NONE;
}

uint32_t vibeos_abi_linux_row_count(void) {
    return g_total_rows;
}

const vibeos_row_t *vibeos_abi_linux_row(uint32_t index) {
    uint32_t t;
    for (t = 0; t < g_table_count; t++) {
        if (index < g_table_len[t]) {
            return &g_tables[t][index];
        }
        index -= g_table_len[t];
    }
    return 0;
}

static const vibeos_row_t *linux_lookup(uint64_t nr) {
    return find_row(nr);
}

/* Const, so it needs no lock: nothing writes it after link time. (The linker
 * files a const struct that holds a function pointer under initialised data,
 * and check-subsystem.py, which reads that, wants the discipline stated.) */
static const vibeos_abi_t g_linux = { "linux-x86_64", linux_lookup };

const vibeos_abi_t *vibeos_abi_linux(void) {
    return &g_linux;
}
