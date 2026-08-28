/* Threads, through a real C library.
 *
 * Written against POSIX, not against VibeOS: pthread_create becomes clone()
 * with CLONE_VM|CLONE_THREAD and the rest, and pthread_join sleeps on a futex
 * until the exiting thread's kernel-side clear_child_tid write wakes it.
 *
 * It passes: one thread created and joined, then four at once contending for a
 * mutex, with the counter landing exactly on 8000 and every thread seeing its
 * own thread-local.
 *
 * It is NOT on the boot media, and that is a measurement rather than caution.
 * With this program in the self-test the boot passes six times in eight; with
 * every other change from the same day and this program left out, seven in
 * eight - the same as the tree before any of it, whose one failure is the
 * known cold-start stall in the firmware. So the remaining defect is in
 * running several threads, not in the memory or exit changes that went in
 * alongside.
 *
 * The two failure signatures, for whoever picks this up:
 *
 *   - a #GP in ring 3 at a rip inside the low Linux window with an rsp in the
 *     high VibeOS window, which is two programs' state in one context;
 *   - a wedge at the busybox phase with no fault at all.
 *
 * Build it and run it by hand from the kernel CLI while working on that.
 */
#include <pthread.h>
#include <stdio.h>

static __thread int mine;   /* thread-local: the child must see its own */

#define WORKERS 4
#define BUMPS 2000

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static long counter;

/* Several of these run at once, so the lock really contends - which is the
 * only way FUTEX_WAIT is reached at all: an uncontended lock is taken with an
 * atomic instruction and never enters the kernel. */
static void *worker(void *arg)
{
    int i;

    mine = (int)(long)arg;
    for (i = 0; i < BUMPS; i++) {
        pthread_mutex_lock(&lock);
        counter++;
        pthread_mutex_unlock(&lock);
    }
    return (void *)(long)mine;
}

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
    printf("THREADS_STAGE1_OK: created and joined one thread, tls returned %ld\n",
           (long)ret);
    fflush(stdout);

    /* Now several at once, which is a different question: threads that
     * overlap, contend for a lock, and exit while others are still being
     * created. */
    {
        pthread_t many[WORKERS];
        long i, tls_ok = 1;

        for (i = 0; i < WORKERS; i++) {
            if (pthread_create(&many[i], 0, worker, (void *)i) != 0) {
                printf("THREADS_FAIL: create %ld\n", i);
                fflush(stdout);
                return 1;
            }
        }
        for (i = 0; i < WORKERS; i++) {
            void *r = 0;
            if (pthread_join(many[i], &r) != 0) {
                printf("THREADS_FAIL: join %ld\n", i);
                fflush(stdout);
                return 1;
            }
            if ((long)r != i) {
                tls_ok = 0;   /* a thread saw another thread's thread-local */
            }
        }
        printf("THREADS_OK: %d threads, counter=%ld expected=%d tls=%s\n",
               WORKERS, counter, WORKERS * BUMPS, tls_ok ? "ok" : "shared");
        fflush(stdout);
    }
    return 0;
}
