/* Threads created and joined while the process execs.
 *
 * The other half of the combination that produced the execve defect, and the
 * half nothing exercised. `musl_tfork.c` covers forking with a thread running;
 * this covers a process that is making and reaping threads while children of
 * it are replacing their images.
 *
 * Why that shape is dangerous here, specifically: execve destroys the outgoing
 * address space, and for a long time it did so unconditionally. A threaded
 * process that execs was therefore freeing page tables its siblings were still
 * running on - the fix asks `hw_aspace_shared_by_other` first, and that
 * question has exactly one caller's worth of testing behind it.
 *
 * The test does not assert anything about scheduling order, because there is
 * nothing to assert: what it checks is that the thread's own memory survives
 * unchanged across execs happening beside it, and that every child actually
 * reached its new image. A machine that gets this wrong does not fail here - it
 * fails somewhere else, later, in whichever program is handed the page.
 */

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>

/* Where this program lives on the boot media. It re-execs itself, and it
 * cannot ask: /proc/self/exe does not exist on this filesystem, which the boot
 * log shows a C runtime discovering on every start. */
#define SELF_PATH "EFI/BOOT/TEXEC.ELF"

#define ROUNDS 8
#define PATTERN_BYTES 4096

/* Each worker owns its own page and fills it with a byte only it uses, so a
 * page that comes back wrong names the thread it was stolen from. */
struct work {
    unsigned char *page;
    unsigned char byte;
    int ok;
};

static void *worker(void *arg)
{
    struct work *w = arg;
    int i;

    memset(w->page, w->byte, PATTERN_BYTES);
    /* Touched twice with a check in between: once to establish the mapping and
     * cache a translation, once after other work has had a chance to disturb
     * it. A single write and check would pass on a machine that only corrupts
     * pages it has had time to hand elsewhere. */
    for (i = 0; i < PATTERN_BYTES; i += 256) {
        if (w->page[i] != w->byte) {
            w->ok = 0;
            return NULL;
        }
    }
    memset(w->page, w->byte, PATTERN_BYTES);
    w->ok = 1;
    return NULL;
}

int main(int argc, char **argv)
{
    int round;
    int execs = 0;

    /* Re-exec'd children land here and leave immediately. Their job is to have
     * replaced an image while the parent had threads, not to do anything. */
    if (argc > 1 && strcmp(argv[1], "child") == 0) {
        return 0;
    }

    for (round = 0; round < ROUNDS; round++) {
        pthread_t t;
        struct work w;
        pid_t kid;
        int status = 0;

        w.page = malloc(PATTERN_BYTES);
        if (!w.page) {
            printf("TEXEC_FAIL: out of memory\n");
            return 1;
        }
        w.byte = (unsigned char)(0x40 + round);
        w.ok = -1;

        if (pthread_create(&t, NULL, worker, &w) != 0) {
            printf("TEXEC_FAIL: pthread_create\n");
            return 1;
        }

        /* The exec happens *while* that thread is alive and working. */
        kid = fork();
        if (kid == 0) {
            /* The path, not argv[0].
             *
             * The first version execv'd argv[0], which failed on the first run
             * - and the reason is a rule this project has written down: argv[0]
             * is a name the caller chose, not a path. BusyBox becomes twenty
             * commands by looking at it. A test that re-execs itself has to
             * know where it lives, and here that is a fixed location on the
             * boot media. */
            char *args[] = { (char *)"texec", (char *)"child", NULL };
            execv(SELF_PATH, args);
            _exit(127);            /* execv only returns on failure */
        }

        if (pthread_join(t, NULL) != 0) {
            printf("TEXEC_FAIL: pthread_join\n");
            return 1;
        }
        if (w.ok != 1) {
            printf("TEXEC_FAIL: a thread's own page changed under it, round %d\n",
                   round);
            return 1;
        }

        if (kid < 0) {
            printf("TEXEC_FAIL: fork\n");
            return 1;
        }
        if (waitpid(kid, &status, 0) != kid) {
            printf("TEXEC_FAIL: waitpid\n");
            return 1;
        }
        if (!WIFEXITED(status)) {
            printf("TEXEC_FAIL: the exec'd child died on a signal, round %d\n",
                   round);
            return 1;
        }
        if (WEXITSTATUS(status) == 127) {
            printf("TEXEC_FAIL: the child never reached its new image\n");
            return 1;
        }
        if (WEXITSTATUS(status) != 0) {
            printf("TEXEC_FAIL: the exec'd child exited %d\n",
                   WEXITSTATUS(status));
            return 1;
        }
        execs++;

        /* Checked after the join and the reap, so a page freed by either shows
         * up here rather than in whatever gets it next. */
        {
            int i;
            for (i = 0; i < PATTERN_BYTES; i += 256) {
                if (w.page[i] != w.byte) {
                    printf("TEXEC_FAIL: the page changed after the child exec'd,"
                           " round %d\n", round);
                    return 1;
                }
            }
        }
        free(w.page);
    }

    printf("TEXEC_OK rounds=%d execs=%d\n", ROUNDS, execs);
    return 0;
}
