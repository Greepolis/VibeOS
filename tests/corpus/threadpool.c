/* A thread pool and the rest of what pthreads offers, for the corpus
 * (docs/abi/ L6 step 4). Built twice by scripts/dev/corpus-build.sh - against
 * musl statically and against glibc dynamically - and run by
 * tests/corpus/run-l1.sh in the guest and on Linux, whose two outputs the boot
 * gate compares line for line. So every line it prints is a fact both
 * kernels must agree on, and nothing it prints depends on timing or on how
 * many cores the machine has: the work is fixed, and so are the answers.
 *
 * What each part leans on in the kernel: the pool and its queue, futex WAIT
 * and WAKE through a condition variable, and REQUEUE where the C library
 * hands a broadcast's waiters to the mutex; the barrier, many waiters woken
 * at once; the semaphores, a futex word used as a counter; cancellation, a
 * signal to one thread that ends a blocking wait; thread-local storage, each
 * thread's own %fs; and four workers, so that on a machine with four cores
 * they run on several of them at once. */
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define WORKERS 4
#define JOBS 200

/* ---- the pool ------------------------------------------------------------- */

static pthread_mutex_t q_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t q_more = PTHREAD_COND_INITIALIZER;
static pthread_cond_t q_done = PTHREAD_COND_INITIALIZER;
static int q_next, q_finished, q_closed;
static unsigned long long results[JOBS];
static int done_by[WORKERS];

/* Enough work that a job is not nothing, and the same answer everywhere. */
static unsigned long long job(int i)
{
    unsigned long long h = 1469598103934665603ull ^ (unsigned long long)i;
    int k;

    for (k = 0; k < 20000; k++) {
        h ^= (unsigned long long)k;
        h *= 1099511628211ull;
    }
    return h;
}

static void *worker(void *arg)
{
    int me = (int)(long)arg;

    for (;;) {
        int i;

        pthread_mutex_lock(&q_lock);
        while (q_next >= JOBS && !q_closed) {
            pthread_cond_wait(&q_more, &q_lock);
        }
        if (q_next >= JOBS) {
            pthread_mutex_unlock(&q_lock);
            return 0;
        }
        i = q_next++;
        pthread_mutex_unlock(&q_lock);

        results[i] = job(i);

        pthread_mutex_lock(&q_lock);
        done_by[me]++;
        if (++q_finished == JOBS) {
            pthread_cond_signal(&q_done);
        }
        pthread_mutex_unlock(&q_lock);
    }
}

static void pool(void)
{
    pthread_t t[WORKERS];
    unsigned long long sum = 0;
    int i, total = 0;

    for (i = 0; i < WORKERS; i++) {
        pthread_create(&t[i], 0, worker, (void *)(long)i);
    }
    pthread_mutex_lock(&q_lock);
    pthread_cond_broadcast(&q_more);
    while (q_finished < JOBS) {
        pthread_cond_wait(&q_done, &q_lock);
    }
    q_closed = 1;
    pthread_cond_broadcast(&q_more);
    pthread_mutex_unlock(&q_lock);
    for (i = 0; i < WORKERS; i++) {
        pthread_join(t[i], 0);
        total += done_by[i];
    }
    for (i = 0; i < JOBS; i++) {
        sum ^= results[i] + (unsigned long long)i;
    }
    printf("pool: %d jobs by %d workers, checksum %016llx\n", total, WORKERS, sum);
}

/* ---- a barrier, three rounds ----------------------------------------------- */

static pthread_barrier_t bar;
static int round_seen[3][WORKERS];

static void *at_barrier(void *arg)
{
    int me = (int)(long)arg, r;

    for (r = 0; r < 3; r++) {
        round_seen[r][me] = 1;
        pthread_barrier_wait(&bar);
    }
    return 0;
}

static void barrier(void)
{
    pthread_t t[WORKERS];
    int i, r, all = 1;

    pthread_barrier_init(&bar, 0, WORKERS);
    for (i = 0; i < WORKERS; i++) {
        pthread_create(&t[i], 0, at_barrier, (void *)(long)i);
    }
    for (i = 0; i < WORKERS; i++) {
        pthread_join(t[i], 0);
    }
    for (r = 0; r < 3; r++) {
        for (i = 0; i < WORKERS; i++) {
            all &= round_seen[r][i];
        }
    }
    pthread_barrier_destroy(&bar);
    printf("barrier: 3 rounds of %d, all arrived: %s\n", WORKERS, all ? "yes" : "no");
}

/* ---- semaphores: a bounded producer and consumer ----------------------------- */

static sem_t slots, items;
static int ring[8], r_head, r_tail;
static long consumed;

static void *producer(void *arg)
{
    int i;

    (void)arg;
    for (i = 1; i <= 1000; i++) {
        sem_wait(&slots);
        ring[r_head++ % 8] = i;
        sem_post(&items);
    }
    return 0;
}

static void *consumer(void *arg)
{
    int i;

    (void)arg;
    for (i = 0; i < 1000; i++) {
        sem_wait(&items);
        consumed += ring[r_tail++ % 8];
        sem_post(&slots);
    }
    return 0;
}

static void semaphores(void)
{
    pthread_t p, c;

    sem_init(&slots, 0, 8);
    sem_init(&items, 0, 0);
    pthread_create(&p, 0, producer, 0);
    pthread_create(&c, 0, consumer, 0);
    pthread_join(p, 0);
    pthread_join(c, 0);
    printf("semaphores: 1000 items through 8 slots, sum %ld\n", consumed);
}

/* ---- thread-local storage and once ----------------------------------------- */

static __thread int mine = 100;
static pthread_once_t once = PTHREAD_ONCE_INIT;
static int once_ran;

static void init_once(void) { once_ran++; }

static void *tls_worker(void *arg)
{
    int i;

    pthread_once(&once, init_once);
    for (i = 0; i < (int)(long)arg; i++) {
        mine++;
    }
    return (void *)(long)mine;
}

static void tls(void)
{
    pthread_t t[WORKERS];
    long got[WORKERS];
    int i;

    for (i = 0; i < WORKERS; i++) {
        pthread_create(&t[i], 0, tls_worker, (void *)(long)(i + 1));
    }
    for (i = 0; i < WORKERS; i++) {
        void *v;
        pthread_join(t[i], &v);
        got[i] = (long)v;
    }
    printf("tls: %ld %ld %ld %ld, main's still %d, once ran %d time(s)\n",
           got[0], got[1], got[2], got[3], mine, once_ran);
}

/* ---- cancellation of a thread that waits ------------------------------------ */

static pthread_mutex_t c_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t c_never = PTHREAD_COND_INITIALIZER;
static int c_cleaned;

static void cleanup(void *arg)
{
    (void)arg;
    c_cleaned = 1;
    pthread_mutex_unlock(&c_lock);
}

static void *waits_forever(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&c_lock);
    pthread_cleanup_push(cleanup, 0);
    for (;;) {
        pthread_cond_wait(&c_never, &c_lock);
    }
    pthread_cleanup_pop(0);
    return 0;
}

static void cancellation(void)
{
    pthread_t t;
    void *v = 0;
    struct timespec ms = { 0, 20000000 };

    pthread_create(&t, 0, waits_forever, 0);
    nanosleep(&ms, 0);
    pthread_cancel(t);
    pthread_join(t, &v);
    printf("cancel: a thread in pthread_cond_wait ends %s, its cleanup ran: %s\n",
           v == PTHREAD_CANCELED ? "cancelled" : "otherwise", c_cleaned ? "yes" : "no");
}

int main(void)
{
    setvbuf(stdout, 0, _IONBF, 0);
    pool();
    barrier();
    semaphores();
    tls();
    cancellation();
    return 0;
}
