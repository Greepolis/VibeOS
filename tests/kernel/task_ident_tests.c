/* Host tests for the task identity (C5). Each check names the defect it stands for. */

#include <stdio.h>
#include <string.h>

#include "vibeos/task_ident.h"

int test_task_ident(void);

static int expect(int cond, const char *what) {
    if (!cond) {
        printf("FAIL:task_ident %s\n", what);
    }
    return cond;
}

int test_task_ident(void) {
    vibeos_task_t t;
    uint32_t i;
    unsigned char *raw = (unsigned char *)&t;

    /* A recycled slot holds the previous tenant's identity. Fill every byte with a
     * pattern the reset must not leave anywhere - a field added to the struct and
     * forgotten in the reset survives it and is caught here, which a field-by-field
     * expectation would not be. */
    memset(&t, 0xA5, sizeof(t));
    vibeos_task_identity_reset(&t);
    for (i = 0; i < sizeof(t); i++) {
        if (raw[i] != 0u) {
            printf("FAIL:task_ident reset left byte %u of the identity set (a field the reset forgot)\n", i);
            return -1;
        }
    }

    /* pid is the thread, tgid the process; the leader is the one that is both. */
    t.pid = 7; t.tgid = 7;
    if (!expect(vibeos_task_is_group_leader(&t), "pid == tgid is the group leader")) { return -1; }
    t.pid = 8;
    if (!expect(!vibeos_task_is_group_leader(&t), "a thread with its own tid is not the leader")) { return -1; }
    if (!expect(!vibeos_task_is_group_leader(NULL), "NULL is not a leader")) { return -1; }

    /* The fork guard's accounting: children, and threads of the group - a thread's
     * ppid is its creator's parent, so ppid alone would miss it. */
    vibeos_task_identity_reset(&t);
    t.ppid = 3;
    if (!expect(vibeos_task_accountable_to(&t, 3), "a child is accountable to its parent")) { return -1; }
    if (!expect(!vibeos_task_accountable_to(&t, 4), "a stranger is not")) { return -1; }
    vibeos_task_identity_reset(&t);
    t.ppid = 1; t.tgid = 3; t.is_thread = 1;
    if (!expect(vibeos_task_accountable_to(&t, 3), "a thread is accountable to its group")) { return -1; }
    t.is_thread = 0;
    if (!expect(!vibeos_task_accountable_to(&t, 3),
                "same tgid without is_thread is the leader itself, not a thread of it")) { return -1; }

    /* comm: bounded, terminated, and no stale tail. */
    vibeos_task_identity_reset(&t);
    vibeos_task_set_comm(&t, "abcdefghijklmnopqrstuvwxyz", 26);
    if (!expect(t.comm[15] == 0 && strlen(t.comm) == 15u, "an over-long name is cut at 15 and terminated")) { return -1; }
    vibeos_task_set_comm(&t, "ab\0zzzzzzzzzzzz", 16);
    if (!expect(strcmp(t.comm, "ab") == 0 && t.comm[3] == 0 && t.comm[10] == 0,
                "a shorter name leaves no tail of the previous one")) { return -1; }
    vibeos_task_set_comm(&t, "x", 0);
    if (!expect(t.comm[0] == 0, "a zero-length name clears it")) { return -1; }
    return 0;
}
