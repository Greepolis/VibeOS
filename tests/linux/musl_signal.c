/* Signals, from a real Linux program built against a real C library.
 *
 * Written against Linux, not against VibeOS: it uses sigaction and raise the
 * way any program would, and the C library supplies the return trampoline the
 * kernel has to jump back through. That is the point - a signal implementation
 * that only satisfies a hand-written test proves very little, because the hard
 * parts are the ones libc assumes rather than the ones a test remembers to
 * check.
 *
 * What it actually checks, in order:
 *
 *   the handler runs at all, with the right signal number
 *   execution resumes where it was interrupted, with locals intact - which is
 *     what the saved register frame is for
 *   a blocked signal stays pending instead of being lost, and arrives when it
 *     is unblocked
 *   an ignored signal really is discarded
 *   the default action still kills, and the parent sees 128 + the signal
 */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile sig_atomic_t g_got;
static volatile sig_atomic_t g_count;

static void on_signal(int sig) {
    g_got = sig;
    g_count++;
}

int main(void) {
    struct sigaction sa;
    sigset_t block, old;
    volatile long witness = 0x5A5A5A5A;
    int ok = 1;

    printf("SIG_PHASE: sigaction\n");
    fflush(stdout);
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    if (sigaction(SIGUSR1, &sa, NULL) != 0) {
        printf("SIG_FAIL: sigaction\n");
        return 1;
    }

    raise(SIGUSR1);
    printf("SIG_PHASE: handler\n");
    fflush(stdout);
    if (g_got != SIGUSR1 || g_count != 1) {
        printf("SIG_FAIL: handler did not run\n");
        ok = 0;
    }
    /* The value below has to survive the trip through the handler. If the
     * kernel restored the wrong frame, this is where it shows. */
    if (witness != 0x5A5A5A5A) {
        printf("SIG_FAIL: registers not restored\n");
        ok = 0;
    }

    /* Blocked means deferred, not dropped. */
    sigemptyset(&block);
    sigaddset(&block, SIGUSR2);
    printf("SIG_PHASE: masking\n");
    fflush(stdout);
    if (sigprocmask(SIG_BLOCK, &block, &old) != 0) {
        printf("SIG_FAIL: sigprocmask\n");
        ok = 0;
    }
    if (sigaction(SIGUSR2, &sa, NULL) != 0) {
        printf("SIG_FAIL: sigaction 2\n");
        ok = 0;
    }
    g_got = 0;
    raise(SIGUSR2);
    if (g_got != 0) {
        printf("SIG_FAIL: blocked signal was delivered\n");
        ok = 0;
    }
    if (sigprocmask(SIG_SETMASK, &old, NULL) != 0) {
        printf("SIG_FAIL: sigprocmask restore\n");
        ok = 0;
    }
    if (g_got != SIGUSR2) {
        printf("SIG_FAIL: pending signal lost while blocked\n");
        ok = 0;
    }

    /* Ignored means gone. */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_IGN;
    printf("SIG_PHASE: ignoring\n");
    fflush(stdout);
    sigaction(SIGUSR1, &sa, NULL);
    g_got = 0;
    raise(SIGUSR1);
    if (g_got != 0) {
        printf("SIG_FAIL: ignored signal was delivered\n");
        ok = 0;
    }

    /* The default action for SIGTERM is still to die, and the parent should
     * see that rather than a normal exit. */
    {
        pid_t child = fork();
        printf("SIG_PHASE: fork child=%d\n", (int)child);
        fflush(stdout);
        if (child == 0) {
            printf("SIG_PHASE: child terminate\n");
            fflush(stdout);
            raise(SIGTERM);
            _exit(0);          /* only reached if the signal did nothing */
        } else if (child > 0) {
            int status = 0;
            waitpid(child, &status, 0);
            /* A signal death is not an exit with a large code: WIFSIGNALED is
             * the question, and WTERMSIG is the answer. */
            if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGTERM) {
                printf("SIG_FAIL: default action, status=%d\n", status);
                ok = 0;
            }
        }
    }

    printf(ok ? "SIG_OK: handlers, masking, ignoring and default actions\n"
              : "SIG_FAIL: see above\n");
    fflush(stdout);
    return ok ? 0 : 1;
}
