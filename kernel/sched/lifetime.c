/* The decisions a task's death involves. Phase S-P3 of docs/sched/. */

#include "vibeos/lifetime.h"

/* ---- wait status -------------------------------------------------------- */

int vibeos_wait_status_make(uint32_t exit_code, uint32_t signal) {
    if (signal != 0u) {
        /* A task killed by a signal carries the signal and leaves the exit
         * byte clear. An init that read only the code byte reported a segfault
         * as a clean stop, and the crashing service came back STOPPED. */
        return (int)(signal & 0x7Fu);
    }
    return (int)((exit_code & 0xFFu) << 8);
}

int vibeos_wait_status_exited(int status) {
    return ((uint32_t)status & 0x7Fu) == 0u;
}

uint32_t vibeos_wait_status_code(int status) {
    return ((uint32_t)status >> 8) & 0xFFu;
}

uint32_t vibeos_wait_status_signal(int status) {
    return (uint32_t)status & 0x7Fu;
}

/* ---- reaping ------------------------------------------------------------ */

vibeos_reap_verdict_t vibeos_reap_check(vibeos_task_ref_t child,
                                        uint32_t child_tgid,
                                        uint32_t child_ppid,
                                        uint32_t parent_tgid,
                                        uint32_t want_tgid) {
    /* The reference first. A wait4 that dropped the scheduler lock and came
     * back with a bare slot index would be asking about whoever holds the slot
     * now, which is the shape of three defects in this kernel. */
    if (!vibeos_task_ref_valid(child)) {
        return VIBEOS_REAP_GONE;
    }
    if (child_ppid != parent_tgid) {
        return VIBEOS_REAP_NOT_A_CHILD;
    }
    if (want_tgid != 0u && child_tgid != want_tgid) {
        return VIBEOS_REAP_NOT_A_CHILD;
    }
    if (vibeos_task_state(child.slot) != VIBEOS_TASK_ZOMBIE) {
        return VIBEOS_REAP_NOT_DEAD;
    }
    return VIBEOS_REAP_OK;
}

/* ---- teardown order ------------------------------------------------------ */

#define LIFETIME_MAX_SLOTS 64u

static uint8_t g_step[LIFETIME_MAX_SLOTS];

void vibeos_teardown_reset(uint32_t slot) {
    if (slot < LIFETIME_MAX_SLOTS) {
        g_step[slot] = (uint8_t)VIBEOS_TEARDOWN_BEGIN;
    }
}

int vibeos_teardown_step(uint32_t slot, vibeos_teardown_step_t step) {
    if (slot >= LIFETIME_MAX_SLOTS) {
        return -1;
    }
    /* Steps advance by one, in order. Skipping one is the defect: publishing a
     * slot before its address space is gone is the silent wedge, and publishing
     * before harvesting is what let a reaper free a kernel stack a fork had
     * just allocated into the same slot.
     *
     * Repeating a step is allowed. A path that releases an address space it
     * turns out to share still passes through the same point, and refusing
     * that would make the check something callers work around. */
    if ((uint32_t)step < (uint32_t)g_step[slot] ||
        (uint32_t)step > (uint32_t)g_step[slot] + 1u) {
        vibeos_task_stats()->use_after_publish++;
        return -1;
    }
    g_step[slot] = (uint8_t)step;
    return 0;
}
