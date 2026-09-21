#ifndef VIBEOS_TASK_IDENT_H
#define VIBEOS_TASK_IDENT_H

#include <stdint.h>

/* C5: what a task *is*, apart from what a CPU needs to run it.
 *
 * `hw_task_t` used to be one 200-line structure holding both, so every question
 * about identity - who is the parent, is this the group leader, what is it called,
 * is a signal pending - was answered by code that could equally have reached the
 * saved registers, and the fields were reset by whichever of nine paths remembered
 * to. This is the identity half: portable, host-testable, and cleared in one
 * function that every way of making a task passes through. The architecture keeps
 * the register file, the kernel stack, the address space and the descriptors, and
 * embeds one of these.
 *
 * `pid` is the thread id (Linux's tid) and `tgid` the thread group - the number a
 * program calls its process id. For a single-threaded process they are equal.
 * getpid() returns tgid and gettid() returns pid; getting that backwards makes a C
 * library signal itself instead of another thread. */
typedef struct vibeos_task {
    uint32_t pid;
    uint32_t tgid;
    uint32_t ppid;
    uint32_t pgid;
    uint32_t sid;
    uint32_t service_id;

    uint8_t is_thread;   /* shares its creator's address space */
    int is_user;
    int is_idle;         /* per-CPU idle task: only run when nothing else is ready */
    int wait_input;      /* blocked in read() on stdin */
    uint8_t signal_stopped;   /* stopped by SIGSTOP until SIGCONT */

    /* How it ended. A task killed by a signal carries the signal here and leaves
     * the code byte zero, which is what wait() encodes: a parent that cannot tell
     * the two apart reads a crash as a clean exit. */
    uint64_t exit_code;
    uint32_t exit_signal;
    uint64_t clear_child_tid;   /* CLONE_CHILD_CLEARTID: zeroed and woken at exit */

    char comm[16];   /* prctl(PR_SET_NAME); always NUL-terminated */

    /* Raised-but-undelivered and deferred signals. The thread's, not the
     * process's: the dispositions live with the process. */
    uint64_t sig_pending;
    uint64_t sig_blocked;
} vibeos_task_t;

/* Put every identity field into a defined state. The one place that does it, so no
 * way of making a task can forget a field - the shape of every recycled-slot bug
 * here (a field written on one path and read on all of them). */
void vibeos_task_identity_reset(vibeos_task_t *t);

/* pid == tgid: the thread that names the process. */
int vibeos_task_is_group_leader(const vibeos_task_t *t);

/* Does `parent_pid` answer for this task? It does when the task is its child, or
 * when it is one of its threads. A thread inherits its creator's *parent* rather
 * than becoming its child, so counting by ppid alone bounds fork and does not
 * bound threads at all (the fork guard's ceiling once did nothing for them). */
int vibeos_task_accountable_to(const vibeos_task_t *t, uint32_t parent_pid);

/* Set the name from `src` (at most 16 bytes, not necessarily terminated): copies up
 * to the first NUL or 15 bytes and always terminates. */
void vibeos_task_set_comm(vibeos_task_t *t, const char *src, uint32_t n);

#endif
