/* fork() from a process that still has a thread running.
 *
 * This is the one shape that needs a TLB shootdown, and nothing else in the
 * boot produces it: fork revokes write permission on every page of the calling
 * process so the child can share them copy-on-write, but `invlpg` reaches only
 * the core that ran fork. A thread of the same process on another core keeps
 * its cached writable entry and writes straight through into a page the child
 * now shares - no fault, no copy, and the damage appears later in whichever
 * program that page is eventually handed to.
 *
 * The test makes that visible instead of waiting for it to corrupt something:
 *
 *   - the worker touches a page, which is what caches the writable entry on
 *     its core. Without this step there is nothing stale to catch.
 *   - the main thread forks.
 *   - the worker then fills the page with a different byte.
 *   - the child, which forked before any of that, must still see the old byte.
 *
 * If the child ever sees the new byte, the worker wrote through a translation
 * that fork had already revoked.
 */

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define PAGE 4096u
#define BEFORE 0xAAu
#define AFTER  0xCCu

static volatile unsigned char *g_page;
static volatile int g_touched;
static volatile int g_go;

static void *worker(void *unused) {
    unsigned i;

    (void)unused;
    /* Write once, so this core caches a writable translation for the page. */
    g_page[0] = BEFORE;
    g_touched = 1;

    while (!g_go) {
        /* Spin rather than sleep: the point is to still be here, on this core,
         * holding that translation, when the other thread calls fork. */
    }
    for (i = 0; i < PAGE; i++) {
        g_page[i] = AFTER;
    }
    return 0;
}

int main(void) {
    pthread_t t;
    pid_t child;
    unsigned spin;

    g_page = mmap(0, PAGE, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (g_page == MAP_FAILED) {
        printf("TFORK_FAIL: mmap\n");
        fflush(stdout);
        return 1;
    }
    memset((void *)g_page, BEFORE, PAGE);

    if (pthread_create(&t, 0, worker, 0) != 0) {
        printf("TFORK_FAIL: pthread_create\n");
        fflush(stdout);
        return 1;
    }
    while (!g_touched) {
    }

    child = fork();
    if (child < 0) {
        printf("TFORK_FAIL: fork\n");
        fflush(stdout);
        return 1;
    }
    if (child == 0) {
        /* The child has no worker thread - fork carries only the caller - so
         * nothing here should ever change this page. Keep looking for a while:
         * the write that would break it happens on another core, after this
         * process already exists. */
        unsigned round;
        for (round = 0; round < 200000u; round++) {
            unsigned i;
            for (i = 0; i < PAGE; i += 512u) {
                if (g_page[i] != BEFORE) {
                    _exit(2);   /* the parent's thread wrote into our copy */
                }
            }
        }
        _exit(0);
    }

    /* Release the worker only now, so every one of its writes happens after
     * the child's address space was taken. */
    g_go = 1;
    pthread_join(t, 0);

    {
        int status = 0;
        waitpid(child, &status, 0);
        if (!WIFEXITED(status)) {
            printf("TFORK_FAIL: child died on a signal\n");
        } else if (WEXITSTATUS(status) == 2) {
            printf("TFORK_FAIL: child saw the parent thread's writes\n");
        } else if (WEXITSTATUS(status) != 0) {
            printf("TFORK_FAIL: child exited %d\n", WEXITSTATUS(status));
        } else {
            /* And the parent's own copy did get the writes: a test that passes
             * because nothing happened at all would be worthless. */
            for (spin = 0; spin < PAGE; spin += 512u) {
                if (g_page[spin] != AFTER) {
                    printf("TFORK_FAIL: the worker's writes went nowhere\n");
                    fflush(stdout);
                    return 1;
                }
            }
            printf("TFORK_OK: fork from a threaded process kept the child's pages\n");
        }
    }
    fflush(stdout);
    return 0;
}
