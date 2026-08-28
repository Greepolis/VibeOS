/* Several threads at once. Does not pass yet - see the note below.
 *
 * Deliberately not on the boot media: it hangs, and a hanging program in the
 * self-test is a boot that fails for five minutes instead of a test that says
 * no. Build and run it by hand when working on the defect.
 *
 * What happens: creating a second thread while the first is exiting leaves the
 * process stuck. The main thread sleeps on musl's __thread_list_lock, and that
 * word holds the tid of a thread that has already exited - so the lock was
 * released by nobody and every later pthread_create waits on a dead owner.
 *
 * What has been ruled out, so the next person does not repeat it:
 *  - thread-local storage: each thread gets a distinct fs_base, logged at
 *    creation;
 *  - the futex itself: wait and wake meet correctly for the single-thread
 *    join, which uses the same path;
 *  - unsupported futex operations: unknown ones are logged as warnings and
 *    none appears;
 *  - task slots: exhaustion is logged and does not happen.
 *
 * The remaining suspect is the exit path: a thread that dies between taking
 * that lock and releasing it. musl's __tl_unlock has a recursive count and an
 * owner field, so the next step is to find out whether the exiting thread ever
 * reaches its unlock - which means tracing the exit sequence rather than
 * reasoning about it.
 */
#include <pthread.h>
#include <stdio.h>

#define WORKERS 4
#define BUMPS 2000

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static long counter;
static __thread int mine;

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

int main(void)
{
    pthread_t t[WORKERS];
    long i, tls_ok = 1;

    for (i = 0; i < WORKERS; i++) {
        if (pthread_create(&t[i], 0, worker, (void *)i) != 0) {
            printf("THREADS_MANY_FAIL: create %ld\n", i);
            fflush(stdout);
            return 1;
        }
    }
    for (i = 0; i < WORKERS; i++) {
        void *ret = 0;
        if (pthread_join(t[i], &ret) != 0) {
            printf("THREADS_MANY_FAIL: join %ld\n", i);
            fflush(stdout);
            return 1;
        }
        if ((long)ret != i) {
            tls_ok = 0;
        }
    }
    printf("THREADS_MANY_OK: counter=%ld expected=%d tls=%s\n",
           counter, WORKERS * BUMPS, tls_ok ? "ok" : "shared");
    fflush(stdout);
    return 0;
}
