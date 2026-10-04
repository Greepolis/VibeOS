/* A description that names a process (docs/abi/ L2 step 5).
 *
 * Linux's pidfd - and what a Windows process handle is, which is why it lives
 * with the other file types and not in the Linux layer. It remembers the
 * process's id and which tenancy of the task slot it had, so that a pid reused
 * after the process was reaped is not mistaken for it: the whole reason a
 * program holds a pidfd rather than a number.
 *
 * It is readable - in poll's sense - once the process has ended. Nothing is
 * read from it. */

#include "files_internal.h"

/* The slot the description's process is in, if it is still that process -
 * running or ended and not yet reaped. -1 if it is gone. Under the scheduler's
 * lock, which the caller holds. */
int vibeos_pidfd_slot(const vibeos_file_t *f) {
    int slot;

    if (!f || f->ops != &vibeos_fops_pidfd) {
        return -1;
    }
    slot = ks_task_by_tid(f->proc_pid);
    if (slot < 0 || ks_seq(slot) != f->proc_seq || ks_id(slot)->tgid != f->proc_pid) {
        return -1;
    }
    return slot;
}

static int pidfd_stat(vibeos_file_t *f, vibeos_file_stat_t *out) {
    out->mode = VIBEOS_S_IFREG | 0600u;   /* Linux's anonymous inode */
    out->size = 0;
    out->ino = 0x20000u + (uint64_t)f->proc_pid;
    return 0;
}

/* Ready once the process has ended - a zombie, or reaped already. */
static uint32_t pidfd_ready(vibeos_file_t *f) {
    int slot;
    uint32_t r;

    ks_lock(ks_sched_lock(), __func__);
    slot = vibeos_pidfd_slot(f);
    r = (slot < 0 || vibeos_task_state((uint32_t)slot) == VIBEOS_TASK_ZOMBIE) ? VIBEOS_READY_IN : 0u;
    ks_unlock(ks_sched_lock());
    return r;
}

const vibeos_file_ops_t vibeos_fops_pidfd = {
    .name = "pidfd",
    .stat = pidfd_stat,
    .ready = pidfd_ready,
};

vibeos_file_t *vibeos_open_pidfd(uint32_t pid, uint32_t seq, uint32_t flags) {
    vibeos_file_t *f = vibeos_file_alloc(&vibeos_fops_pidfd, VIBEOS_O_RDWR | (flags & VIBEOS_O_NONBLOCK));

    if (f) {
        f->proc_pid = pid;
        f->proc_seq = seq;
    }
    return f;
}
