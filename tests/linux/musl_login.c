/* SPDX-License-Identifier: GPL-3.0-or-later */
/* login, driven as a person would (docs/shell/s2.md): LOGIN.ELF on a
 * pseudo-terminal, this program at the keyboard. A wrong password, a name
 * that does not exist, a right one and what the session then is - the user's
 * ids and home, and /etc/shadow out of its reach - a logout back to the prompt,
 * and root. Prints LOGIN_OK if every one held, LOGIN_FAIL: and which if not.
 *
 * The password is the development image's (scripts/make-root-tree.py); a
 * production image has none until its first boot. */

#define _GNU_SOURCE 1

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#define LOGIN_PATH "/usr/bin/LOGIN.ELF"

static char g_seen[8192], g_rest[8192];
static size_t g_n;
static const char *g_waiting = "";
static pid_t g_child;

/* A conversation that stalls says where, inside the boot gate's quiet budget
 * rather than as a silent machine. */
static void on_alarm(int sig) {
    (void)sig;
    dprintf(1, "LOGIN_FAIL: stalled waiting for [%s]\nLOGIN_SAW: %.300s\n", g_waiting, g_seen);
    if (g_child > 0) {
        kill(g_child, SIGKILL);   /* a session of its own: not in our group */
    }
    _exit(1);
}

/* Reads until `word` has appeared since the last call; what was read stays in
 * g_seen for the checks after it. */
static int until(int m, const char *word) {
    /* What came after the last word is the start of this one: "Login
     * incorrect" and the next prompt arrive in one read. */
    g_n = strlen(g_rest);
    memcpy(g_seen, g_rest, g_n + 1);
    g_rest[0] = 0;
    g_waiting = word;
    alarm(30);
    for (;;) {
        ssize_t r;
        char *hit = strstr(g_seen, word);

        if (hit) {
            hit += strlen(word);
            strcpy(g_rest, hit);
            *hit = 0;
            return 1;
        }
        if (g_n >= sizeof(g_seen) - 1) {
            return 0;
        }
        r = read(m, g_seen + g_n, sizeof(g_seen) - 1 - g_n);
        if (r < 0 && errno == EINTR) {
            continue;
        }
        if (r <= 0) {
            return 0;
        }
        g_n += (size_t)r;
        g_seen[g_n] = 0;
    }
}

static void type(int m, const char *s) {
    if (write(m, s, strlen(s)) != (ssize_t)strlen(s)) {
        printf("LOGIN_FAIL: could not type\n");
        fflush(stdout);
        _exit(1);
    }
}

#define NEED(cond, what)                                   \
    do {                                                   \
        if (!(cond)) {                                     \
            printf("LOGIN_FAIL: %s\n", what);              \
            printf("LOGIN_SAW: %.300s\n", g_seen);         \
            fflush(stdout);                                \
            if (child > 0) {                               \
                kill(child, SIGKILL);                      \
            }                                              \
            return 1;                                      \
        }                                                  \
    } while (0)

int main(void) {
    int m, s, status = 0;
    pid_t child = 0;

    m = posix_openpt(O_RDWR | O_NOCTTY);
    NEED(m >= 0 && unlockpt(m) == 0, "no pseudo-terminal");
    s = open(ptsname(m), O_RDWR | O_NOCTTY);
    NEED(s >= 0, "no slave");
    signal(SIGALRM, on_alarm);
    fflush(stdout);
    child = fork();
    if (child == 0) {
        char *argv[] = {"login", 0}, *envp[] = {"LEAK=from_the_caller", 0};

        close(m);
        dup2(s, 0);
        dup2(s, 1);
        dup2(s, 2);
        if (s > 2) {
            close(s);
        }
        execve(LOGIN_PATH, argv, envp);
        _exit(127);
    }
    g_child = child;
    close(s);

    NEED(until(m, "login: "), "no prompt");
    type(m, "vibe\n");
    NEED(until(m, "Password: "), "no password prompt");
    type(m, "not-the-password\n");
    NEED(until(m, "Login incorrect"), "a wrong password was not refused");
    NEED(!strstr(g_seen, "not-the-password"), "the password was echoed");
    NEED(until(m, "login: "), "no prompt after a refusal");

    type(m, "nobody-here\n");
    NEED(until(m, "Password: "), "a name that does not exist was told apart at the prompt");
    type(m, "whatever\n");
    NEED(until(m, "Login incorrect"), "a name that does not exist was not refused");
    NEED(until(m, "login: "), "no prompt after that");

    type(m, "vibe\n");
    NEED(until(m, "Password: "), "no password prompt for the real login");
    type(m, "vibeos\n");
    NEED(until(m, "$ "), "the user's shell did not start");
    type(m, "echo U_$(id -u)_G_$(id -g)_P_$(pwd)_H_${HOME}_L_${LEAK:-clean}; cat /etc/shadow 2>/dev/null; echo SHADOW_RC_$?; echo DONE_$((3*3))\n");
    NEED(until(m, "DONE_9"), "the session did not run the command");
    NEED(strstr(g_seen, "U_1000_G_1000_P_/home/vibe"), "the wrong ids or directory");
    NEED(strstr(g_seen, "_L_clean"), "the caller's environment reached the session");
    NEED(strstr(g_seen, "SHADOW_RC_1"), "a user could read /etc/shadow");
    NEED(!strstr(g_seen, "$6$"), "a password hash reached a user");
    type(m, "exit\n");
    NEED(until(m, "login: "), "logging out did not come back to the prompt");

    type(m, "root\n");
    NEED(until(m, "Password: "), "no password prompt for root");
    type(m, "vibeos\n");
    NEED(until(m, "# "), "root's shell did not start");
    type(m, "echo R_$(id -u)_P_$(pwd); echo DONE_$((2+2))\n");
    NEED(until(m, "DONE_4"), "root's session did not run the command");
    NEED(strstr(g_seen, "R_0_P_/root"), "root has the wrong ids or directory");
    type(m, "exit\n");
    NEED(until(m, "login: "), "root's logout did not come back");

    close(m);   /* the terminal goes: login reads end of file and ends */
    NEED(waitpid(child, &status, 0) == child, "login did not end");
    alarm(0);
    child = 0;
    NEED(WIFEXITED(status) && WEXITSTATUS(status) == 0, "login did not end cleanly");
    printf("LOGIN_OK: refused, session, ids, home, shadow, logout, root\n");
    fflush(stdout);
    return 0;
}
