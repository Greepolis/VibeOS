/* SPDX-License-Identifier: GPL-3.0-or-later */
/* execve of a script (docs/shell/s5.md). The program is its own interpreter, so
 * what it was handed can be printed and compared; every expectation but the
 * last holds on Linux too, where this also runs:
 *
 *     SCRIPTT.ELF
 *
 * Prints SCRIPT_OK, or SCRIPT_FAIL: and which. */

#define _GNU_SOURCE 1

#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static char g_self[PATH_MAX];
static char g_dir[64];

static void fail(const char *what, const char *detail) {
    printf("SCRIPT_FAIL: %s [%s]\n", what, detail ? detail : "");
    fflush(stdout);
    exit(1);
}

static void put(const char *name, const char *text, mode_t mode) {
    char p[128];
    int fd;

    snprintf(p, sizeof(p), "%s/%s", g_dir, name);
    (void)unlink(p);
    fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0 || write(fd, text, strlen(text)) != (ssize_t)strlen(text) || close(fd) != 0 ||
        chmod(p, mode) != 0) {
        fail("could not write a script", name);
    }
}

/* What running it printed, or -errno if execve refused. */
static int run(const char *name, char *out, size_t cap) {
    char p[128];
    char *argv[3], *envp[] = {(char *)"SCRIPTT_INTERP=1", (char *)"PATH=/usr/bin:/bin", 0};
    int fds[2], report[2], st = 0, err = 0;
    size_t len = 0;
    ssize_t r;
    pid_t c;

    snprintf(p, sizeof(p), "%s/%s", g_dir, name);
    argv[0] = p;
    argv[1] = (char *)"x";
    argv[2] = 0;
    if (pipe(fds) != 0 || pipe2(report, O_CLOEXEC) != 0) {
        fail("pipe", name);
    }
    fflush(stdout);
    c = fork();
    if (c < 0) {
        fail("fork", name);
    }
    if (c == 0) {
        dup2(fds[1], 1);
        close(fds[0]);
        close(fds[1]);
        execve(p, argv, envp);
        err = errno;
        (void)!write(report[1], &err, sizeof(err));
        _exit(126);
    }
    close(fds[1]);
    close(report[1]);
    while (len + 1u < cap && (r = read(fds[0], out + len, cap - 1u - len)) > 0) {
        len += (size_t)r;
    }
    out[len] = 0;
    close(fds[0]);
    r = read(report[0], &err, sizeof(err));
    close(report[0]);
    (void)waitpid(c, &st, 0);
    return r == (ssize_t)sizeof(err) ? -err : 0;
}

static void prints(const char *name, const char *want) {
    char got[512];
    int r = run(name, got, sizeof(got));

    if (r != 0) {
        fail("execve refused it", name);
    }
    if (strcmp(got, want) != 0) {
        fail(name, got);
    }
}

static void refused(const char *name, int err) {
    char got[64];

    if (run(name, got, sizeof(got)) != -err) {
        fail("not the refusal Linux gives", name);
    }
}

/* Files in /tmp are memory, and the boot gate counts what a boot never gave
 * back: the first version left its scripts there and failed it. */
static void clean(void) {
    DIR *d = opendir(g_dir);
    struct dirent *e;

    while (d && (e = readdir(d)) != 0) {
        if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0) {
            (void)unlinkat(dirfd(d), e->d_name, 0);
        }
    }
    if (d) {
        closedir(d);
    }
    (void)rmdir(g_dir);
}

int main(int argc, char **argv) {
    char text[PATH_MAX + 64], want[PATH_MAX * 3];

    if (getenv("SCRIPTT_INTERP")) {
        int i;

        for (i = 0; i < argc; i++) {
            printf("%s%s", i ? "|" : "", argv[i]);
        }
        printf("\n");
        return 0;
    }
    if (argv[0][0] == '/') {
        snprintf(g_self, sizeof(g_self), "%s", argv[0]);
    } else if (readlink("/proc/self/exe", g_self, sizeof(g_self) - 1u) <= 0) {
        fail("who am I", argv[0]);
    }
    snprintf(g_dir, sizeof(g_dir), "/tmp/scriptt-%d", (int)getpid());
    if (mkdir(g_dir, 0755) != 0) {
        fail("mkdir", g_dir);
    }

    snprintf(text, sizeof(text), "#!%s\nbody\n", g_self);
    put("a", text, 0755);
    snprintf(want, sizeof(want), "%s|%s/a|x\n", g_self, g_dir);
    prints("a", want);

    snprintf(text, sizeof(text), "#!  %s   one  two  \n", g_self);
    put("b", text, 0755);
    snprintf(want, sizeof(want), "%s|one  two|%s/b|x\n", g_self, g_dir);
    prints("b", want);

    /* A script whose interpreter is a script. */
    snprintf(text, sizeof(text), "#!%s/a\n", g_dir);
    put("c", text, 0755);
    snprintf(want, sizeof(want), "%s|%s/a|%s/c|x\n", g_self, g_dir, g_dir);
    prints("c", want);

    put("gone", "#!/no/such/interpreter\n", 0755);
    refused("gone", ENOENT);
    put("plain", "just text\n", 0755);
    refused("plain", ENOEXEC);
    put("empty", "#!\n", 0755);
    refused("empty", ENOEXEC);
    snprintf(text, sizeof(text), "#!%s\n", g_self);
    put("noexec", text, 0644);
    refused("noexec", EACCES);
    snprintf(text, sizeof(text), "#!%s/loop\n", g_dir);
    put("loop", text, 0755);
    refused("loop", ELOOP);

    /* The table, where there is one: no `#!`, run by its extension. */
    if (access("/etc/vibeos/binfmt", R_OK) == 0) {
        put("t.vsh", "echo \"vsh-ran-${args[0]}\"\n", 0755);
        prints("t.vsh", "vsh-ran-x\n");
        put("U.VSH", "echo \"upper-${args[0]}\"\n", 0755);
        prints("U.VSH", "upper-x\n");
        snprintf(text, sizeof(text), "#!%s\necho no\n", g_self);
        put("w.vsh", text, 0755);
        snprintf(want, sizeof(want), "%s|%s/w.vsh|x\n", g_self, g_dir);
        prints("w.vsh", want);      /* a `#!` wins over the table */
        put("t.unknown", "text\n", 0755);
        refused("t.unknown", ENOEXEC);
    }
    clean();
    printf("SCRIPT_OK: interpreter, argument, nested, refusals, table\n");
    return 0;
}
