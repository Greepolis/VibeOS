/* SPDX-License-Identifier: GPL-3.0-or-later */
/* vsh's job control, played by a person at a pseudo-terminal
 * (docs/shell/s4.md step 3): Ctrl+Z, jobs, bg, fg, Ctrl+C, kill %n, a pipeline
 * stopped and resumed, a loop interrupted, a background reader stopped by the
 * terminal. The same program on Linux and in the guest.
 *
 *     VSHJOBS.ELF /usr/bin/vsh
 *
 * Prints VSHJOBS_OK if every one held, VSHJOBS_FAIL: and which if not. What it
 * expects is never spelled the way it is typed, because the terminal echoes
 * what is typed: `echo rc=$?` cannot satisfy a wait for rc=130. */

#define _GNU_SOURCE 1

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define PROMPT "vsh$ "

static int g_m = -1;
static pid_t g_shell = -1;
static char g_buf[65536];
static size_t g_len, g_at;

static void fail(const char *what) {
    char line[160];
    int n = snprintf(line, sizeof(line), "VSHJOBS_FAIL: %s\n", what);

    (void)!write(1, line, (size_t)n);
    if (g_shell > 0) {
        kill(g_shell, SIGKILL);
    }
    exit(1);
}

static long now_ms(void) {
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long)t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

static void send_text(const char *s) {
    size_t n = strlen(s);

    while (n > 0u) {
        ssize_t w = write(g_m, s, n);

        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            fail("the terminal refused a keystroke");
        }
        s += w;
        n -= (size_t)w;
    }
}

/* Past the next `word` in what the terminal printed; 0 if it did not come. */
static int until(const char *word) {
    long end = now_ms() + 40000L;

    for (;;) {
        struct pollfd p;
        const char *hit;
        ssize_t n;

        g_buf[g_len] = 0;
        hit = strstr(g_buf + g_at, word);
        if (hit) {
            g_at = (size_t)(hit - g_buf) + strlen(word);
            return 1;
        }
        if (g_len + 4096u >= sizeof(g_buf)) {
            memmove(g_buf, g_buf + g_at, g_len - g_at);
            g_len -= g_at;
            g_at = 0;
        }
        if (now_ms() > end) {
            return 0;
        }
        p.fd = g_m;
        p.events = POLLIN;
        p.revents = 0;
        if (poll(&p, 1, 200) <= 0) {
            continue;
        }
        n = read(g_m, g_buf + g_len, 4095);
        if (n > 0) {
            /* A NUL from the terminal must not end the search early. */
            ssize_t i;

            for (i = 0; i < n; i++) {
                if (!g_buf[g_len + (size_t)i]) {
                    g_buf[g_len + (size_t)i] = ' ';
                }
            }
            g_len += (size_t)n;
        } else if (n == 0 || errno != EINTR) {
            return 0;
        }
    }
}

static void expect(const char *word, const char *what) {
    if (!until(word)) {
        fail(what);
    }
}

static void prompt(const char *what) {
    expect(PROMPT, what);
}

/* A keystroke meant for a program goes after the program has the terminal;
 * sent earlier it goes to the shell, which ignores it. */
static void program_has_terminal(const char *what) {
    long end = now_ms() + 30000L;

    for (;;) {
        int fg = 0;

        if (ioctl(g_m, TIOCGPGRP, &fg) == 0 && fg > 0 && fg != g_shell) {
            usleep(300000);
            return;
        }
        if (now_ms() > end) {
            fail(what);
        }
        usleep(10000);
    }
}

static void shell_has_terminal(const char *what) {
    int fg = 0;

    if (ioctl(g_m, TIOCGPGRP, &fg) != 0 || fg != g_shell) {
        fail(what);
    }
}

static void stop_and_resume(void) {
    send_text("sleep 30\n");
    program_has_terminal("sleep never got the terminal");
    send_text("\x1a");
    expect("Stopped", "Ctrl+Z did not stop the program");
    prompt("no prompt after Ctrl+Z");
    shell_has_terminal("the shell did not take the terminal back after a stop");

    send_text("jobs\n");
    expect("[1]+ Stopped", "jobs does not list the stopped job");
    prompt("no prompt after jobs");

    send_text("bg\n");
    expect("[1] sleep 30 &", "bg did not say what it resumed");
    prompt("no prompt after bg");
    send_text("jobs\n");
    expect("Running", "bg did not leave the job running");
    prompt("no prompt after the second jobs");

    send_text("fg\n");
    expect("sleep 30", "fg did not say what it resumed");
    program_has_terminal("fg did not give the job the terminal");
    send_text("\x03");
    prompt("Ctrl+C did not end the foreground job");
    shell_has_terminal("the shell did not take the terminal back after Ctrl+C");
    send_text("echo rc=$?\n");
    expect("rc=130", "a job ended by Ctrl+C is not 130");
    prompt("no prompt after the status");
}

/* bg has to have resumed it: a stopped job would never end, and the wait with
 * it. It sleeps the rest of its time, not none of it. */
static void bg_finishes(void) {
    long began;

    began = now_ms();
    send_text("sleep 3\n");
    program_has_terminal("the second sleep never got the terminal");
    send_text("\x1a");
    expect("Stopped", "Ctrl+Z did not stop the second sleep");
    prompt("no prompt after stopping the second sleep");
    send_text("bg\n");
    prompt("no prompt after the second bg");
    send_text("wait %1; echo bw=$?\n");
    expect("bw=0", "a job resumed by bg did not finish");
    prompt("no prompt after waiting for it");
    if (now_ms() - began < 2500L) {
        fail("a sleep that was stopped and continued ended early");
    }
}

static void jobs_is_empty(void) {
    size_t from;
    char saved;
    const char *found;

    send_text("jobs; echo end-$((1+1))\n");
    from = g_at;
    expect("end-2", "jobs did not finish");
    saved = g_buf[g_at];
    g_buf[g_at] = 0;
    found = strstr(g_buf + from, "[1]");
    g_buf[g_at] = saved;
    if (found) {
        fail("a finished job is still listed");
    }
    prompt("no prompt after the empty jobs");
}

static void kill_by_job(void) {
    send_text("sleep 30 &\n");
    expect("[1] ", "& did not announce the job");
    prompt("no prompt after &");
    shell_has_terminal("a background job took the terminal");
    send_text("kill %1; wait %1; echo w=$?\n");
    expect("w=143", "kill %1 and wait %1 did not report SIGTERM");
    prompt("no prompt after kill");
}

static void pipeline(void) {
    send_text("cat | cat\n");
    program_has_terminal("the pipeline never got the terminal");
    send_text("\x1a");
    expect("Stopped", "Ctrl+Z did not stop the pipeline");
    prompt("no prompt after stopping the pipeline");
    send_text("fg\n");
    expect("cat | cat", "fg did not name the pipeline");
    program_has_terminal("fg did not give the pipeline the terminal");
    send_text("through-it\n");
    expect("through-it", "the terminal did not echo the line");
    expect("through-it", "the resumed pipeline did not carry the line");
    send_text("\x04");
    prompt("end of input did not end the pipeline");
    send_text("echo back-$((2+2))\n");
    expect("back-4", "the shell lost its input after the pipeline");
    prompt("no prompt after the pipeline");
}

static void loop_interrupted(void) {
    size_t from;
    char saved;
    const char *found;

    send_text("for i in 1 2 { sleep 30; echo no-$i }\n");
    from = g_at;
    program_has_terminal("the loop's program never got the terminal");
    send_text("\x03");
    prompt("Ctrl+C did not end the loop");
    saved = g_buf[g_at];
    g_buf[g_at] = 0;
    found = strstr(g_buf + from, "no-1");
    g_buf[g_at] = saved;
    if (found) {
        fail("Ctrl+C ended the program and the loop went on");
    }
}

static void background_reader(void) {
    send_text("cat &\n");
    expect("[1] ", "& did not announce the reader");
    prompt("no prompt after the reader");
    send_text("sleep 1; jobs; echo e-$((3+3))\n");
    expect("Stopped", "a background reader was not stopped by the terminal");
    expect("e-6", "jobs did not finish after the reader");
    prompt("no prompt after the reader's jobs");
    send_text("kill %1; wait %1; echo k=$?\n");
    expect("k=143", "SIGTERM did not end a stopped job");
    prompt("no prompt after killing the reader");
}

static void survives_ctrl_c(void) {
    send_text("\x03");
    send_text("echo alive-$((4+4))\n");
    expect("alive-8", "Ctrl+C at the prompt ended the shell");
    prompt("no prompt after Ctrl+C at the prompt");
}

int main(int argc, char **argv) {
    char *const env[] = {(char *)"PATH=/usr/bin:/bin", (char *)"TERM=dumb", (char *)"HOME=/", 0};
    char *cargv[2];
    int st = 0;
    long end;
    pid_t got;

    if (argc != 2) {
        fail("usage: VSHJOBS.ELF path-of-vsh");
    }
    signal(SIGPIPE, SIG_IGN);
    g_m = posix_openpt(O_RDWR | O_NOCTTY);
    if (g_m < 0 || grantpt(g_m) != 0 || unlockpt(g_m) != 0) {
        fail("no pseudo-terminal");
    }
    g_shell = fork();
    if (g_shell < 0) {
        fail("fork");
    }
    if (g_shell == 0) {
        int s;

        setsid();
        s = open(ptsname(g_m), O_RDWR);
        if (s < 0 || ioctl(s, TIOCSCTTY, 0) != 0) {
            _exit(120);
        }
        dup2(s, 0);
        dup2(s, 1);
        dup2(s, 2);
        if (s > 2) {
            close(s);
        }
        close(g_m);
        signal(SIGPIPE, SIG_DFL);
        cargv[0] = argv[1];
        cargv[1] = 0;
        execve(argv[1], cargv, env);
        _exit(127);
    }
    prompt("vsh printed no prompt");
    shell_has_terminal("the shell does not own its terminal");

    stop_and_resume();
    bg_finishes();
    jobs_is_empty();
    kill_by_job();
    pipeline();
    loop_interrupted();
    background_reader();
    survives_ctrl_c();

    send_text("exit 7\n");
    end = now_ms() + 20000L;
    while ((got = waitpid(g_shell, &st, WNOHANG)) == 0 && now_ms() < end) {
        char drain[256];
        struct pollfd p = {g_m, POLLIN, 0};

        if (poll(&p, 1, 50) > 0) {
            (void)!read(g_m, drain, sizeof(drain));
        }
    }
    if (got != g_shell || !WIFEXITED(st) || WEXITSTATUS(st) != 7) {
        fail("exit 7 did not end the shell with 7");
    }
    g_shell = -1;
    (void)!write(1, "VSHJOBS_OK: stop, bg, fg, kill, pipeline, loop, reader\n", 55);
    return 0;
}
