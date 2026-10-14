/* The NT personality's registry (docs/abi/windows.md W1). See vibeos/abi_nt.h.
 *
 * Portable and host-tested, like abi_linux.c beside it: the table of services
 * comes from nt_services.def, the rows from kernel/abi/nt/, and nothing here
 * knows the architecture. */

#include "vibeos/abi.h"
#include "vibeos/abi_nt.h"

static const vibeos_nt_service_t g_services[] = {
#define NT(nr, name, args, state, step) \
    { (nr), #name, (args), VIBEOS_NT_##state, #step },
#include "nt_services.def"
#undef NT
};
#define SERVICES ((uint32_t)(sizeof(g_services) / sizeof(g_services[0])))

#define NT_TABLES_MAX 16u

static const vibeos_row_t *g_by_nr[SERVICES];
static uint32_t g_table_count;

const vibeos_nt_service_t *vibeos_nt_service(uint64_t nr) {
    /* The .def is written in order, and the number is the line: the lookup is
     * an index, and the entry says whether the file still agrees. */
    if (nr >= SERVICES || g_services[nr].nr != nr) {
        return 0;
    }
    return &g_services[nr];
}

static const vibeos_row_t *nt_lookup(uint64_t nr) {
    return nr < SERVICES ? g_by_nr[nr] : 0;
}

static const vibeos_abi_t g_nt_abi = { "nt", nt_lookup };

const vibeos_abi_t *vibeos_abi_nt(void) {
    return &g_nt_abi;
}

void vibeos_abi_nt_reset(void) {
    uint32_t i;
    for (i = 0; i < SERVICES; i++) {
        g_by_nr[i] = 0;
    }
    g_table_count = 0;
}

int vibeos_abi_nt_register(const vibeos_row_t *rows, uint32_t count) {
    uint32_t i, j;

    if (!rows || g_table_count >= NT_TABLES_MAX) {
        return -1;
    }
    /* All of it checked before any of it is taken, as abi_linux.c does. */
    for (i = 0; i < count; i++) {
        const vibeos_nt_service_t *s = vibeos_nt_service(rows[i].nr);

        if (!s || !rows[i].handler || rows[i].op == VIBEOS_OP_NONE ||
            (uint32_t)rows[i].op >= (uint32_t)VIBEOS_OP_COUNT || g_by_nr[rows[i].nr]) {
            return -1;
        }
        for (j = 0; j < i; j++) {
            if (rows[j].nr == rows[i].nr) {
                return -1;
            }
        }
        /* A descriptor naming an argument the service does not take would
         * judge a register or a stack slot the caller never filled. */
        for (j = 0; j < VIBEOS_PTR_MAX; j++) {
            const vibeos_ptr_t *d = &rows[i].ptr[j];
            if ((d->flags & VIBEOS_PTR_LIVE) &&
                (d->arg >= s->args || d->len_arg > s->args || d->when_arg > s->args)) {
                return -1;
            }
        }
    }
    for (i = 0; i < count; i++) {
        g_by_nr[rows[i].nr] = &rows[i];
    }
    g_table_count++;
    return 0;
}

uint32_t vibeos_abi_nt_disagreement(void) {
    uint32_t i;

    for (i = 0; i < SERVICES; i++) {
        int says = g_services[i].state == VIBEOS_NT_DONE ||
                   g_services[i].state == VIBEOS_NT_PARTIAL;
        if (g_services[i].nr != i || says != (g_by_nr[i] != 0)) {
            return i;
        }
    }
    return SERVICES;
}
