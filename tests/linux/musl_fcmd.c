/* SPDX-License-Identifier: GPL-3.0-or-later */
/* FinalCommander, a full-screen terminal program, played by a person at a
 * pseudo-terminal (docs/shell/s7.md). The same program on Linux and in the guest:
 *
 *     FCMDT.ELF /path/to/fcmd [transcript]
 *
 * It opens a directory holding a file with a known name, waits for the panel to
 * show it, makes a directory with F7, and quits with F10. Prints FCMD_OK, or
 * FCMD_FAIL: and which. A transcript path keeps every byte the program wrote,
 * to look at the screen afterwards. */

#define _GNU_SOURCE 1

#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int g_m = -1;
static pid_t g_child = -1;
static char g_buf[1 << 20];
static size_t g_len;
static FILE *g_keep;

static void fail(const char *what) {
    /* What the program had done by then, in one line: whether it was still
     * running, how much it had drawn, and the end of it with the escapes made
     * visible. "Never showed" and "did not end" alone were dead ends. */
    if (g_child > 0) {
        char tail[200];
        size_t from = g_len > 120u ? g_len - 120u : 0u, k = 0;
        int st = 0;
        pid_t r = waitpid(g_child, &st, WNOHANG);

        for (size_t i = from; i < g_len && k + 2u < sizeof(tail); i++) {
            unsigned char c = (unsigned char)g_buf[i];
            tail[k++] = (c >= 32u && c < 127u) ? (char)c : '.';
        }
        tail[k] = 0;
        printf("FCMD_STATE child=%s status=0x%x bytes=%zu tail=[%s]\n",
               r == g_child ? "ended" : (r == 0 ? "running" : "unknown"), st, g_len, tail);
        if (r == g_child) {
            g_child = -1;
        }
        /* And the text it drew, escapes dropped: the panel's file count and
         * its rows say whether the listing was empty or drawn in pieces. */
        {
            static char vis[sizeof(g_buf)];
            size_t n = 0;

            for (size_t i = 0; i < g_len; i++) {
                unsigned char c = (unsigned char)g_buf[i];

                if (c == 0x1b) {
                    i++;
                    if (i < g_len && g_buf[i] == '[') {
                        while (i + 1 < g_len && !(g_buf[i + 1] >= 0x40 && g_buf[i + 1] <= 0x7e)) {
                            i++;
                        }
                        i++;
                        if (n && vis[n - 1] != ' ') {
                            vis[n++] = ' ';
                        }
                    } else if (i < g_len && g_buf[i] == ']') {
                        while (i < g_len && g_buf[i] != 0x07) {
                            i++;
                        }
                    }
                } else if (c >= 32u && c < 127u && !(c == ' ' && n && vis[n - 1] == ' ')) {
                    vis[n++] = (char)c;
                }
            }
            vis[n] = 0;
            printf("FCMD_TEXT has_vibeos=%d text=[%s]\n", strstr(vis, "vibeos") != 0,
                   n > 600u ? vis + n - 600u : vis);
        }
        /* Each of its threads, as /proc has it: one parked for good beside
         * the others is a wake that never came. */
        if (g_child > 0) {
            char path[320], line[160];
            DIR *d;
            struct dirent *e;

            snprintf(path, sizeof(path), "/proc/%d/task", (int)g_child);
            d = opendir(path);
            printf("FCMD_THREADS dir=%s\n", d ? "yes" : "absent");
            while (d && (e = readdir(d)) != 0) {
                FILE *f;

                if (e->d_name[0] == '.') {
                    continue;
                }
                snprintf(path, sizeof(path), "/proc/%d/task/%s/stat", (int)g_child, e->d_name);
                f = fopen(path, "r");
                if (f && fgets(line, sizeof(line), f)) {
                    line[strcspn(line, "\n")] = 0;
                    printf("FCMD_THREAD %s\n", line);
                }
                if (f) {
                    fclose(f);
                }
            }
            if (d) {
                closedir(d);
            }
        }
    }
    printf("FCMD_FAIL: %s\n", what);
    fflush(stdout);
    if (g_child > 0) {
        kill(g_child, SIGKILL);
    }
    exit(1);
}

static long now_ms(void) {
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long)t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

/* Everything the program has written so far, without waiting. */
static void drain(int wait_ms) {
    struct pollfd p = {0, POLLIN, 0};
    static long said;
    ssize_t n;

    p.fd = g_m;
    if (!said) {
        said = now_ms();
    }
    while (poll(&p, 1, wait_ms) > 0) {
        /* A line now and then while output keeps coming too: this loop
         * reads for as long as the program writes, and the Windows build
         * redraws its "reading" panel for most of a minute under a loaded
         * host - silence the gate takes for a wedged boot (S7 step 3). */
        if (now_ms() - said >= 10000) {
            said = now_ms();
            printf("FCMD_READING %zu bytes\n", g_len);
            fflush(stdout);
        }
        if (g_len + 4096u >= sizeof(g_buf)) {
            memmove(g_buf, g_buf + sizeof(g_buf) / 2u, g_len - sizeof(g_buf) / 2u);
            g_len -= sizeof(g_buf) / 2u;
        }
        n = read(g_m, g_buf + g_len, 4096);
        if (n <= 0) {
            return;
        }
        for (ssize_t i = 0; i < n; i++) {
            if (!g_buf[g_len + (size_t)i]) {
                g_buf[g_len + (size_t)i] = ' ';
            }
        }
        if (g_keep) {
            fwrite(g_buf + g_len, 1, (size_t)n, g_keep);
            fflush(g_keep);
        }
        g_len += (size_t)n;
        g_buf[g_len] = 0;
        wait_ms = 0;
    }
}

/* Whether `word` has been printed since `from`, waiting up to `ms`. */
static int shows(size_t from, const char *word, long ms) {
    long start = now_ms(), end = start + ms, said = start;

    for (;;) {
        drain(100);
        /* A line now and then while the program loads: a boot that prints
         * nothing for 45 seconds is taken for a wedged one. */
        if (now_ms() - said >= 10000) {
            said = now_ms();
            printf("FCMD_WAITING %lds\n", (said - start) / 1000);
            fflush(stdout);
        }
        if (from <= g_len && strstr(g_buf + from, word)) {
            return 1;
        }
        if (now_ms() > end) {
            return 0;
        }
    }
}

static void keys(const char *s) {
    if (write(g_m, s, strlen(s)) != (ssize_t)strlen(s)) {
        fail("the terminal refused a key");
    }
}

static int exited(long ms, int *status) {
    long end = now_ms() + ms;

    for (;;) {
        pid_t r = waitpid(g_child, status, WNOHANG);

        if (r == g_child) {
            return 1;
        }
        if (now_ms() > end) {
            return 0;
        }
        drain(50);
    }
}

int main(int argc, char **argv) {
    char dir[64], marker[96], made[96];
    struct winsize ws = {30, 100, 0, 0};
    struct stat st;
    int status = 0, fd;

    if (argc < 2) {
        fail("usage: FCMDT.ELF path-of-fcmd [transcript]");
    }
    if (argc > 2) {
        g_keep = fopen(argv[2], "w");
    }
    signal(SIGPIPE, SIG_IGN);
    snprintf(dir, sizeof(dir), "/tmp/fcmdt-%d", (int)getpid());
    snprintf(marker, sizeof(marker), "%s/vibeos-was-here.txt", dir);
    snprintf(made, sizeof(made), "%s/madebyf7", dir);
    if (mkdir(dir, 0755) != 0 || (fd = open(marker, O_WRONLY | O_CREAT, 0644)) < 0) {
        fail("could not make the directory to show");
    }
    (void)!write(fd, "hello\n", 6);
    close(fd);

    g_m = posix_openpt(O_RDWR | O_NOCTTY);
    if (g_m < 0 || grantpt(g_m) != 0 || unlockpt(g_m) != 0 || ioctl(g_m, TIOCSWINSZ, &ws) != 0) {
        fail("no pseudo-terminal");
    }
    g_child = fork();
    if (g_child < 0) {
        fail("fork");
    }
    if (g_child == 0) {
        char *const env[] = {(char *)"TERM=xterm-256color", (char *)"HOME=/tmp", (char *)"LANG=C.UTF-8",
                             (char *)"PATH=/usr/bin:/bin", 0};
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
        if (chdir(dir) != 0) {
            _exit(121);
        }
        execle(argv[1], argv[1], "--lang", "en", dir, (char *)0, env);
        _exit(127);
    }

    /* Two minutes: a 25 MiB program is read from the disk first. */
    if (!shows(0, "vibeos-was-here", 120000)) {
        fail("the panel never showed the directory's file");
    }
    drain(800);

    /* F7: a new directory, named and confirmed. */
    keys("\x1b[18~");
    drain(1500);
    /* The name and Enter apart: in one write a terminal program can take the
     * whole as a paste, Enter included. */
    keys("madebyf7");
    drain(800);
    keys("\r");
    {
        long end = now_ms() + 15000;

        while (stat(made, &st) != 0 && now_ms() < end) {
            drain(100);
        }
        if (stat(made, &st) != 0 || !S_ISDIR(st.st_mode)) {
            fail("F7 made no directory");
        }
    }
    /* Done redrawing before F10: the directory exists on the disk a moment
     * before the program redraws, and with its pages read on first touch a
     * key sent into the redraw was lost once in six boots. Waiting for the
     * new name to be printed does not work: the program writes only the cells
     * that changed, so the name never arrives whole. Two quiet seconds do. */
    {
        long end = now_ms() + 30000;
        size_t before;

        do {
            before = g_len;
            drain(2000);
        } while (g_len != before && now_ms() < end);
    }

    /* F10, and a confirmation if one is asked. */
    keys("\x1b[21~");
    if (!exited(8000, &status)) {
        keys("\r");
        if (!exited(20000, &status)) {
            fail("F10 did not end it");
        }
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fail("it did not end cleanly");
    }
    g_child = -1;
    rmdir(made);
    unlink(marker);
    rmdir(dir);
    printf("FCMD_OK: panel, F7, F10\n");
    return 0;
}
