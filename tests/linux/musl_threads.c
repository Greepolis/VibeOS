/* Threads, through a real C library.
 *
 * Written against POSIX, not against VibeOS: pthread_create becomes clone()
 * with CLONE_VM|CLONE_THREAD and the rest, and pthread_join sleeps on a futex
 * until the exiting thread's kernel-side clear_child_tid write wakes it.
 *
 * This covers one thread, created and joined, which is a smaller claim than it
 * sounds. It needs two tasks in one address space, a thread-local base of its
 * own, a kernel that schedules the new task at all, an exit that clears the
 * word the joiner sleeps on, and a futex whose wait and wake actually meet.
 * Any one of those missing and this hangs rather than failing.
 *
 * Several threads at once is a separate program - musl_threads_many.c - and it
 * does not pass yet. Keeping them apart is deliberate: a single test that does
 * both fails as one word, and the first version of this did exactly that.
 */
#include <pthread.h>
#include <stdio.h>

static __thread int mine;   /* thread-local: the child must see its own */

static void *child(void *arg)
{
    mine = (int)(long)arg;
    return (void *)(long)mine;
}

int main(void)
{
    pthread_t t;
    void *ret = 0;

    if (pthread_create(&t, 0, child, (void *)42L) != 0) {
        printf("THREADS_FAIL: create\n");
        fflush(stdout);
        return 1;
    }
    printf("THREADS_CREATE_OK\n");
    fflush(stdout);

    if (pthread_join(t, &ret) != 0) {
        printf("THREADS_FAIL: join\n");
        fflush(stdout);
        return 1;
    }
    /* Through the thread-local, so this also says the child had its own TLS
     * rather than writing into its creator's. */
    if ((long)ret != 42L) {
        printf("THREADS_FAIL: thread returned %ld, expected 42\n", (long)ret);
        fflush(stdout);
        return 1;
    }
    printf("THREADS_OK: created and joined a thread, tls returned %ld\n",
           (long)ret);
    fflush(stdout);
    return 0;
}
