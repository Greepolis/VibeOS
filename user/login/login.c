/* SPDX-License-Identifier: GPL-3.0-or-later */
/* login (docs/shell/s2.md): who is at this terminal, and their session.
 *
 * Runs as root on a terminal, as init's service on the console. It asks for a
 * name and a password, checks them against /etc/shadow, and starts the user's
 * shell in a session of its own: the terminal made that session's, the user's
 * groups and ids, the home directory, a clean environment. It stays the
 * session's parent, and when the shell ends it asks again - `exit` is a logout,
 * not the end of the machine.
 *
 * An account whose hash is "!firstboot" has no password yet: a machine that
 * ships must not ship with one everybody knows. It is asked for here, before
 * anybody can log in, and written to /etc/shadow. */

#define _GNU_SOURCE 1

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define PASSWD_FILE "/etc/passwd"
#define SHADOW_FILE "/etc/shadow"
#define SHADOW_NEW "/etc/shadow.new"
#define FIRSTBOOT "!firstboot"
#define SHADOW_MAX 8192
#define MIN_PASSWORD 8u

/* Hashed in place of a real one for a name that does not exist or cannot log
 * in, so that how long the answer takes does not say which names do. */
#define DUMMY_HASH "$6$vibeosdummysalt$0US6Mv0G3alNupRNbIpFDp5qniJQRJgD/25LfUV4hNeiK70XRH/GfamO35Lg2EGF286hrC0S8NtsXThuO0GJq/"

typedef struct {
    char name[32];
    char home[128];
    char shell[128];
    uid_t uid;
    gid_t gid;
} account_t;

static struct termios g_sane;
static int g_have_sane;

static void say(const char *s) {
    size_t n = strlen(s);

    while (n > 0) {
        ssize_t w = write(1, s, n);
        if (w <= 0) {
            if (w < 0 && errno == EINTR) {
                continue;
            }
            return;
        }
        s += w;
        n -= (size_t)w;
    }
}

/* A line from the terminal, without its newline. 1, or 0 at end of file. */
static int read_line(char *buf, size_t cap) {
    size_t n = 0;

    for (;;) {
        char c;
        ssize_t r = read(0, &c, 1);

        if (r < 0 && errno == EINTR) {
            continue;
        }
        if (r <= 0) {
            return 0;
        }
        if (c == '\n' || c == '\r') {
            buf[n] = 0;
            return 1;
        }
        if (n + 1 < cap) {
            buf[n++] = c;
        }
    }
}

static int read_password(const char *prompt, char *buf, size_t cap) {
    struct termios off, was;
    int have = tcgetattr(0, &was) == 0, ok;

    /* Echo off first and the prompt after: flushing what was typed after the
     * prompt threw away a password typed the moment it appeared, and login
     * then waited for one that had already been given. */
    if (have) {
        off = was;
        off.c_lflag &= ~(tcflag_t)(ECHO | ECHOE | ECHOK);
        off.c_lflag |= ECHONL;
        (void)tcsetattr(0, TCSAFLUSH, &off);
    }
    say(prompt);
    ok = read_line(buf, cap);
    if (have) {
        (void)tcsetattr(0, TCSANOW, &was);
    }
    return ok;
}

/* The n-th ':' field of `line`, copied. */
static int field(const char *line, int n, char *out, size_t cap) {
    const char *p = line;
    size_t k = 0;

    while (n-- > 0) {
        p = strchr(p, ':');
        if (!p) {
            return 0;
        }
        p++;
    }
    while (*p && *p != ':' && *p != '\n' && k + 1 < cap) {
        out[k++] = *p++;
    }
    out[k] = 0;
    return 1;
}

static int same_name(const char *line, const char *name) {
    size_t n = strlen(name);

    return n > 0 && strncmp(line, name, n) == 0 && line[n] == ':';
}

static int find_account(const char *name, account_t *a) {
    char line[512], num[16];
    FILE *f;
    int found = 0;

    if (strchr(name, ':') || strlen(name) >= sizeof(a->name)) {
        return 0;
    }
    f = fopen(PASSWD_FILE, "r");
    if (!f) {
        return 0;
    }
    while (!found && fgets(line, sizeof(line), f)) {
        if (!same_name(line, name)) {
            continue;
        }
        strcpy(a->name, name);
        found = field(line, 2, num, sizeof(num));
        a->uid = (uid_t)strtoul(num, 0, 10);
        found = found && field(line, 3, num, sizeof(num));
        a->gid = (gid_t)strtoul(num, 0, 10);
        found = found && field(line, 5, a->home, sizeof(a->home)) && field(line, 6, a->shell, sizeof(a->shell)) &&
                a->shell[0] == '/';
    }
    fclose(f);
    return found;
}

static int shadow_hash(const char *name, char *hash, size_t cap) {
    char line[512];
    FILE *f = fopen(SHADOW_FILE, "r");
    int found = 0;

    if (!f) {
        return 0;
    }
    while (!found && fgets(line, sizeof(line), f)) {
        if (same_name(line, name)) {
            found = field(line, 1, hash, cap);
        }
    }
    explicit_bzero(line, sizeof(line));
    fclose(f);
    return found;
}

/* Every byte looked at, whatever the first difference: the time this takes
 * must not say how much of a guess was right. */
static int same_hash(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b), i;
    unsigned char diff = (unsigned char)(la != lb);

    for (i = 0; i < la && i < lb; i++) {
        diff |= (unsigned char)(a[i] ^ b[i]);
    }
    return diff == 0;
}

static int check_password(const char *name, const char *password, account_t *a) {
    char hash[160];
    const char *got;
    int usable = find_account(name, a) && shadow_hash(name, hash, sizeof(hash)) && hash[0] == '$';

    /* "!", "*", an empty field and !firstboot are no password at all. */
    got = crypt(password, usable ? hash : DUMMY_HASH);
    usable = usable && got && same_hash(got, hash);
    explicit_bzero(hash, sizeof(hash));
    return usable;
}

/* ---- the first boot ------------------------------------------------------- */

static int new_hash(const char *password, char *out, size_t cap) {
    static const char alphabet[] = "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    unsigned char r[16];
    char setting[32];
    const char *h;
    size_t i = 0;

    while (i < sizeof(r)) {
        ssize_t n = getrandom(r + i, sizeof(r) - i, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return 0;
        }
        i += (size_t)n;
    }
    memcpy(setting, "$6$", 3);
    for (i = 0; i < sizeof(r); i++) {
        setting[3 + i] = alphabet[r[i] & 63u];
    }
    setting[3 + sizeof(r)] = '$';
    setting[4 + sizeof(r)] = 0;
    h = crypt(password, setting);
    if (!h || h[0] != '$' || strlen(h) >= cap) {
        return 0;
    }
    strcpy(out, h);
    return 1;
}

/* The whole file written beside it and renamed over it: a power cut leaves the
 * old shadow or the new one, never half of one and nobody able to log in. */
static int shadow_store(const char *text, size_t n) {
    size_t done = 0;
    int fd = open(SHADOW_NEW, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600), dir;

    if (fd < 0) {
        return 0;
    }
    if (fchmod(fd, 0600) != 0) {
        close(fd);
        return 0;
    }
    while (done < n) {
        ssize_t w = write(fd, text + done, n - done);
        if (w <= 0) {
            close(fd);
            unlink(SHADOW_NEW);
            return 0;
        }
        done += (size_t)w;
    }
    if (fsync(fd) != 0 || close(fd) != 0 || rename(SHADOW_NEW, SHADOW_FILE) != 0) {
        unlink(SHADOW_NEW);
        return 0;
    }
    dir = open("/etc", O_RDONLY | O_DIRECTORY);
    if (dir >= 0) {
        (void)fsync(dir);
        close(dir);
    }
    return 1;
}

/* Sets the first account still waiting for a password. 1 if it did, 0 if none
 * waits, -1 at end of input or on a failure. */
static int first_boot_one(void) {
    static char text[SHADOW_MAX], out[SHADOW_MAX];
    char name[32], one[128], two[128], hash[160], prompt[80];
    const char *at;
    char *mark;
    size_t n, before;
    ssize_t r;
    int fd = open(SHADOW_FILE, O_RDONLY);

    if (fd < 0) {
        return 0;
    }
    r = read(fd, text, sizeof(text) - 1);
    close(fd);
    if (r <= 0) {
        return 0;
    }
    n = (size_t)r;
    text[n] = 0;
    mark = strstr(text, ":" FIRSTBOOT ":");
    if (!mark) {
        return 0;
    }
    at = mark;
    while (at > text && at[-1] != '\n') {
        at--;
    }
    if ((size_t)(mark - at) >= sizeof(name)) {
        return -1;
    }
    memcpy(name, at, (size_t)(mark - at));
    name[mark - at] = 0;
    for (;;) {
        snprintf(prompt, sizeof(prompt), "New password for %s: ", name);
        if (!read_password(prompt, one, sizeof(one))) {
            return -1;
        }
        if (strlen(one) < MIN_PASSWORD) {
            say("At least 8 characters.\n");
            continue;
        }
        if (!read_password("Again: ", two, sizeof(two))) {
            explicit_bzero(one, sizeof(one));
            return -1;
        }
        if (strcmp(one, two) == 0) {
            break;
        }
        say("They differ.\n");
    }
    r = new_hash(one, hash, sizeof(hash));
    explicit_bzero(one, sizeof(one));
    explicit_bzero(two, sizeof(two));
    if (!r) {
        say("login: could not make a password hash\n");
        return -1;
    }
    before = (size_t)(mark + 1 - text);
    if (before + strlen(hash) + (n - before - strlen(FIRSTBOOT)) >= sizeof(out)) {
        return -1;
    }
    memcpy(out, text, before);
    strcpy(out + before, hash);
    memcpy(out + before + strlen(hash), mark + 1 + strlen(FIRSTBOOT), n - before - strlen(FIRSTBOOT));
    n = before + strlen(hash) + (n - before - strlen(FIRSTBOOT));
    if (!shadow_store(out, n)) {
        say("login: could not write the password\n");
        return -1;
    }
    return 1;
}

/* ---- the session ----------------------------------------------------------- */

static void set_signals(void (*how)(int)) {
    static const int sigs[] = {SIGINT, SIGQUIT, SIGTSTP, SIGTTIN, SIGTTOU, SIGHUP};
    size_t i;

    for (i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++) {
        signal(sigs[i], how);
    }
}

/* Never returns. Any step that fails ends the attempt: a shell started with
 * the wrong ids would be worse than none. */
static void become(const account_t *a) {
    char arg0[sizeof(a->shell) + 1];
    const char *base = strrchr(a->shell, '/');
    char *argv[2], *none[1] = {0};

    set_signals(SIG_DFL);
    if (setsid() < 0) {
        say("login: cannot give the session its terminal\n");
        _exit(1);
    }
    /* The console's descriptions for the session, opened now that it is a
     * session: its end revokes exactly these (H-045), so nothing it leaves
     * behind can read the next login, and login's own stay usable. Only for
     * the console, which is what the revocation covers, and asked by the
     * device rather than by name: ttyname sent a pseudo-terminal's session to
     * the console in the boot's LOGINT. */
    {
        struct stat st;

        if (fstat(0, &st) == 0 && S_ISCHR(st.st_mode) && st.st_rdev == makedev(5, 1)) {
            int fd = open("/dev/console", O_RDWR | O_NOCTTY);

            if (fd < 0 || dup2(fd, 0) < 0 || dup2(fd, 1) < 0 || dup2(fd, 2) < 0) {
                say("login: cannot open the terminal for the session\n");
                _exit(1);
            }
            if (fd > 2) {
                close(fd);
            }
        }
    }
    if (ioctl(0, TIOCSCTTY, 1) != 0) {
        say("login: cannot give the session its terminal\n");
        _exit(1);
    }
    if (initgroups(a->name, a->gid) != 0 || setgid(a->gid) != 0 || setuid(a->uid) != 0) {
        say("login: cannot become that user\n");
        _exit(1);
    }
    /* Asked rather than assumed: a process that could get root back has not
     * become the user. */
    if (getuid() != a->uid || geteuid() != a->uid || getgid() != a->gid || getegid() != a->gid ||
        (a->uid != 0 && setuid(0) == 0)) {
        say("login: the ids did not change\n");
        _exit(1);
    }
    if (chdir(a->home) != 0) {
        say("login: no home directory, starting in /\n");
        if (chdir("/") != 0) {
            _exit(1);
        }
    }
    clearenv();
    setenv("HOME", a->home, 1);
    setenv("USER", a->name, 1);
    setenv("LOGNAME", a->name, 1);
    setenv("SHELL", a->shell, 1);
    setenv("PATH", a->uid == 0 ? "/usr/bin" : "/usr/bin", 1);
    setenv("TERM", "linux", 1);
    /* "-sh": what tells a shell it is a login shell. */
    snprintf(arg0, sizeof(arg0), "-%s", base ? base + 1 : a->shell);
    argv[0] = arg0;
    argv[1] = 0;
    (void)none;
    execv(a->shell, argv);
    say("login: cannot run the shell\n");
    _exit(127);
}

static void pause_after_failure(void) {
    struct timespec t = {.tv_sec = 2, .tv_nsec = 0};

    while (nanosleep(&t, &t) != 0 && errno == EINTR) {
    }
}

int main(void) {
    char name[64], password[128];

    if (getuid() != 0) {
        say("login: only root can start a session\n");
        return 1;
    }
    set_signals(SIG_IGN);
    (void)setsid();                 /* a service is not a group leader; if it is, it already leads */
    (void)ioctl(0, TIOCSCTTY, 1);
    g_have_sane = tcgetattr(0, &g_sane) == 0;

    for (;;) {
        int r = first_boot_one();

        if (r == 0) {
            break;
        }
        if (r < 0) {
            return 1;
        }
    }

    for (;;) {
        account_t a;
        int ok;
        pid_t child;

        /* Whatever the last session left the terminal in. */
        (void)ioctl(0, TIOCSCTTY, 1);
        if (g_have_sane) {
            (void)tcsetattr(0, TCSAFLUSH, &g_sane);
        }
        say("vibeos login: ");
        /* End of file ends login, as on Linux: a closed terminal is the end
         * of it. Ctrl+D at the prompt is end of file too, and init starts
         * login again (respawn, M-153) - before that it was the end of the
         * console's logins. */
        if (!read_line(name, sizeof(name))) {
            return 0;
        }
        if (!name[0]) {
            continue;
        }
        if (!read_password("Password: ", password, sizeof(password))) {
            return 0;
        }
        ok = check_password(name, password, &a);
        explicit_bzero(password, sizeof(password));
        if (!ok) {
            pause_after_failure();
            say("Login incorrect\n");
            continue;
        }
        child = fork();
        if (child == 0) {
            become(&a);
        }
        if (child < 0) {
            say("login: cannot start a session\n");
            continue;
        }
        while (waitpid(child, 0, 0) < 0 && errno == EINTR) {
        }
    }
}
