/* The event loops (docs/abi/ L4), from ring 3, through a real C library.
 *
 * One program for the phase, grown a step at a time as SIGNAL.ELF was for L2.
 * Every check prints SIG_FAIL-style reasons on its own line and the program
 * prints EVENTS_OK only if all of them held; the boot gate reads that line
 * (events_l4_failed).
 *
 * Step 1: poll, ppoll, select and pselect6 on one engine, and sockets that say
 * what they can do - a socket used to be "always ready", so nc, which waits in
 * poll on its socket, never waited.
 *
 * Steps 2 to 4: eventfd, timerfd and signalfd, each woken by another process or
 * by the clock.
 *
 * Step 5: epoll, woken by another process, edge- and level-triggered.
 *
 * Step 6: inotify, woken by a file another process makes. */

#define _GNU_SOURCE
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_usr1;

static void on_usr1(int s) {
    (void)s;
    g_usr1++;
}

static double now_ms(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static int fail(const char *what, long a, long b) {
    printf("EVENTS_FAIL: %s (%ld %ld)\n", what, a, b);
    fflush(stdout);
    return 0;
}

/* Sockets: what poll says of each, with nothing having happened to any. */
static int socket_checks(void) {
    struct sockaddr_in sa;
    struct pollfd p[3];
    int udp = socket(AF_INET, SOCK_DGRAM, 0);
    int tcp = socket(AF_INET, SOCK_STREAM, 0);
    int lis = socket(AF_INET, SOCK_STREAM, 0);
    int ok = 1, n;

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(7071);
    if (udp < 0 || tcp < 0 || lis < 0 || bind(lis, (struct sockaddr *)&sa, sizeof(sa)) != 0 || listen(lis, 4) != 0) {
        return fail("three sockets, one listening", (long)lis, errno);
    }
    p[0].fd = udp; p[0].events = POLLIN | POLLOUT;
    p[1].fd = tcp; p[1].events = POLLIN | POLLOUT;
    p[2].fd = lis; p[2].events = POLLIN;
    n = poll(p, 3, 0);
    if (n != 2 || p[0].revents != POLLOUT || p[1].revents != (POLLOUT | POLLHUP) || p[2].revents != 0) {
        ok = fail("poll: a datagram socket can send, an unconnected stream socket is writable and hung up, "
                  "a listener with nobody waiting is nothing", n, (long)p[2].revents);
    }
    close(udp);
    close(tcp);
    close(lis);
    return ok;
}

/* select: never shorter than the time asked, and the sets say which. */
static int select_checks(void) {
    int f[2], ok = 1, n;
    fd_set rs;
    struct timeval tv;
    double t0, waited;

    if (pipe(f) != 0) {
        return fail("pipe", errno, 0);
    }
    FD_ZERO(&rs);
    FD_SET(f[0], &rs);
    tv.tv_sec = 0;
    tv.tv_usec = 50000;
    t0 = now_ms();
    n = select(f[0] + 1, &rs, 0, 0, &tv);
    waited = now_ms() - t0;
    if (n != 0 || FD_ISSET(f[0], &rs) || waited < 50.0) {
        ok = fail("select on an empty pipe returns 0, the set cleared, after at least the time asked",
                  n, (long)waited);
    }
    (void)write(f[1], "x", 1);
    FD_ZERO(&rs);
    FD_SET(f[0], &rs);
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    n = select(f[0] + 1, &rs, 0, 0, &tv);
    if (n != 1 || !FD_ISSET(f[0], &rs) || tv.tv_sec < 4) {
        ok = fail("a byte makes it readable at once, and most of the time is left", n, (long)tv.tv_sec);
    }
    close(f[0]);
    close(f[1]);
    return ok;
}

/* ppoll and pselect: a mask for the wait. SIGUSR1 is blocked and pending; a
 * wait whose mask lets it through ends in EINTR, its handler runs once, and the
 * program's own mask - SIGUSR1 blocked - is back afterwards. */
static int mask_checks(void) {
    sigset_t block, wait_mask, now;
    struct sigaction sa;
    struct timespec zero = {0, 0};
    fd_set rs;
    int ok = 1, r, f[2];

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_usr1;
    sigaction(SIGUSR1, &sa, 0);
    sigemptyset(&block);
    sigaddset(&block, SIGUSR1);
    sigprocmask(SIG_BLOCK, &block, 0);
    sigemptyset(&wait_mask);

    g_usr1 = 0;
    raise(SIGUSR1);
    r = ppoll(0, 0, 0, &wait_mask);
    sigprocmask(SIG_BLOCK, 0, &now);
    if (r != -1 || errno != EINTR || g_usr1 != 1 || !sigismember(&now, SIGUSR1)) {
        ok = fail("ppoll waits under its own mask, and the program's is back after the handler", r, (long)g_usr1);
    }
    /* Under a mask that keeps it out, the signal stays pending and the call
     * times out. */
    g_usr1 = 0;
    raise(SIGUSR1);
    r = ppoll(0, 0, &zero, &block);
    if (r != 0 || g_usr1 != 0) {
        ok = fail("ppoll's mask keeps out what it keeps out", r, (long)g_usr1);
    }
    r = pselect(0, 0, 0, 0, 0, &wait_mask);
    sigprocmask(SIG_BLOCK, 0, &now);
    if (r != -1 || errno != EINTR || g_usr1 != 1 || !sigismember(&now, SIGUSR1)) {
        ok = fail("so does pselect", r, (long)g_usr1);
    }
    if (pipe(f) == 0) {
        (void)write(f[1], "y", 1);
        FD_ZERO(&rs);
        FD_SET(f[0], &rs);
        r = pselect(f[0] + 1, &rs, 0, 0, &zero, &block);
        if (r != 1 || !FD_ISSET(f[0], &rs)) {
            ok = fail("pselect sees a readable pipe", r, 0);
        }
        close(f[0]);
        close(f[1]);
    }
    sigprocmask(SIG_UNBLOCK, &block, 0);
    signal(SIGUSR1, SIG_DFL);
    return ok;
}

/* Steps 2 to 4: eventfd, timerfd and signalfd, each waited for in a way only
 * another process or the clock can end - the wakes are what the host tests
 * cannot see. */
static int event_fd_checks(void) {
    struct pollfd p;
    uint64_t v = 0;
    struct itimerspec its;
    struct signalfd_siginfo rec[2];
    sigset_t m;
    double t0, dt;
    int ok = 1, st = 0;
    int efd = eventfd(0, EFD_CLOEXEC);
    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    int sfd, n;
    pid_t c;

    if (efd < 0 || tfd < 0) {
        return fail("eventfd and timerfd made", efd, tfd);
    }
    /* A child writes the counter after 30 ms; the parent's read waits for it. */
    c = fork();
    if (c == 0) {
        uint64_t seven = 7;
        usleep(30000);
        _exit(write(efd, &seven, 8) == 8 ? 0 : 1);
    }
    t0 = now_ms();
    n = (int)read(efd, &v, 8);
    dt = now_ms() - t0;
    if (n != 8 || v != 7 || dt < 20) {
        ok = fail("an eventfd read waits for another process's write", (long)v, (long)dt);
    }
    waitpid(c, &st, 0);
    /* A periodic timer, waited for in poll: the first expiry after 40 ms. */
    memset(&its, 0, sizeof(its));
    its.it_value.tv_nsec = 40000000;
    its.it_interval.tv_nsec = 20000000;
    p.fd = tfd; p.events = POLLIN; p.revents = 0;
    t0 = now_ms();
    n = timerfd_settime(tfd, 0, &its, 0) == 0 ? poll(&p, 1, 1000) : -1;
    dt = now_ms() - t0;
    if (n != 1 || dt < 35 || dt > 500) {
        ok = fail("poll waits for a timerfd's first expiry", n, (long)dt);
    }
    usleep(100000);
    if (read(tfd, &v, 8) != 8 || v < 3 || v > 12) {
        ok = fail("a periodic timerfd counts every period since", (long)v, 0);
    }
    /* A signal from another process, blocked, read off a signalfd that a
     * blocking read was already waiting on. */
    sigemptyset(&m);
    sigaddset(&m, SIGUSR2);
    sigprocmask(SIG_BLOCK, &m, 0);
    sfd = signalfd(-1, &m, SFD_CLOEXEC);
    c = fork();
    if (c == 0) {
        usleep(30000);
        _exit(kill(getppid(), SIGUSR2) == 0 ? 0 : 1);
    }
    memset(rec, 0, sizeof(rec));
    n = sfd < 0 ? -1 : (int)read(sfd, rec, sizeof(rec));
    if (n != (int)sizeof(rec[0]) || rec[0].ssi_signo != SIGUSR2 || rec[0].ssi_pid != (uint32_t)c ||
        rec[0].ssi_code != SI_USER) {
        ok = fail("a signalfd read waits for a signal another process sends", (long)rec[0].ssi_signo,
                  (long)rec[0].ssi_pid);
    }
    waitpid(c, &st, 0);
    /* And the same through poll: readable once the other process has sent it. */
    c = fork();
    if (c == 0) {
        usleep(30000);
        _exit(kill(getppid(), SIGUSR2) == 0 ? 0 : 1);
    }
    p.fd = sfd; p.events = POLLIN; p.revents = 0;
    n = poll(&p, 1, 2000);
    if (n != 1 || p.revents != POLLIN || read(sfd, rec, sizeof(rec)) != (ssize_t)sizeof(rec[0])) {
        ok = fail("poll on a signalfd wakes for a signal another process sends", n, p.revents);
    }
    waitpid(c, &st, 0);
    /* Taken, not delivered: nothing is pending now. */
    sigpending(&m);
    if (sigismember(&m, SIGUSR2)) {
        ok = fail("a signal read from a signalfd is no longer pending", 1, 0);
    }
    close(efd);
    close(tfd);
    close(sfd);
    return ok;
}

/* Step 5: epoll, woken by another process, edge-triggered against level, and
 * an epoll inside poll. */
static int epoll_checks(void) {
    struct epoll_event ev, out[4];
    struct pollfd p;
    int pf[2], ok = 1, n, st = 0;
    int ep = epoll_create1(EPOLL_CLOEXEC);
    char b[8];
    double t0, dt;
    pid_t c;

    if (ep < 0 || pipe(pf) != 0) {
        return fail("an epoll and a pipe", ep, errno);
    }
    ev.events = EPOLLIN | EPOLLET;
    ev.data.u64 = 0x1234;
    if (epoll_ctl(ep, EPOLL_CTL_ADD, pf[0], &ev) != 0) {
        return fail("epoll_ctl ADD", errno, 0);
    }
    c = fork();
    if (c == 0) {
        usleep(30000);
        _exit(write(pf[1], "ab", 2) == 2 ? 0 : 1);
    }
    t0 = now_ms();
    n = epoll_wait(ep, out, 4, 2000);
    dt = now_ms() - t0;
    if (n != 1 || out[0].data.u64 != 0x1234 || !(out[0].events & EPOLLIN) || dt < 20) {
        ok = fail("epoll_wait wakes for another process's write", n, (long)dt);
    }
    waitpid(c, &st, 0);
    if (epoll_wait(ep, out, 4, 0) != 0) {
        ok = fail("edge-triggered: not again while nothing changes", 1, 0);
    }
    ev.events = EPOLLIN;
    epoll_ctl(ep, EPOLL_CTL_MOD, pf[0], &ev);
    p.fd = ep; p.events = POLLIN; p.revents = 0;
    if (epoll_wait(ep, out, 4, 0) != 1 || epoll_wait(ep, out, 4, 0) != 1 || poll(&p, 1, 0) != 1) {
        ok = fail("level-triggered: every look, and poll sees the epoll readable", p.revents, 0);
    }
    if (read(pf[0], b, sizeof(b)) != 2 || epoll_wait(ep, out, 4, 0) != 0 || poll(&p, 1, 0) != 0) {
        ok = fail("emptied: neither says so", 0, 0);
    }
    close(pf[0]);
    close(pf[1]);
    close(ep);
    return ok;
}

/* Step 6: inotify - a watched directory, a file made in it by another process,
 * a blocking read that wakes with the record, and a rename's pair. */
static int inotify_checks(void) {
    char buf[1024], *p;
    struct pollfd pf;
    int ok = 1, st = 0, wd, in, n;
    uint32_t cookie = 0, seen = 0;
    pid_t c;

    mkdir("/tmp/evn", 0755);
    in = inotify_init1(IN_CLOEXEC);
    wd = in < 0 ? -1 : inotify_add_watch(in, "/tmp/evn", IN_CREATE | IN_MOVED_FROM | IN_MOVED_TO | IN_DELETE);
    if (wd < 1) {
        return fail("inotify_init1 and a watch on /tmp/evn", in, wd);
    }
    c = fork();
    if (c == 0) {
        int fd;

        usleep(30000);
        fd = open("/tmp/evn/a", O_CREAT | O_WRONLY, 0644);
        _exit(fd >= 0 && close(fd) == 0 ? 0 : 1);
    }
    /* poll first, with an end: a kernel that never says would otherwise hold
     * the boot here until the gate gave up. */
    pf.fd = in; pf.events = POLLIN; pf.revents = 0;
    n = poll(&pf, 1, 2000) == 1 ? (int)read(in, buf, sizeof(buf)) : -1;
    p = buf;
    if (n < (int)sizeof(struct inotify_event) || ((struct inotify_event *)p)->wd != wd ||
        ((struct inotify_event *)p)->mask != IN_CREATE || strcmp(((struct inotify_event *)p)->name, "a") != 0) {
        ok = fail("an inotify read wakes for a file another process makes", n, n > 0 ? (long)((struct inotify_event *)p)->mask : 0);
    }
    waitpid(c, &st, 0);
    if (rename("/tmp/evn/a", "/tmp/evn/b") != 0) {
        return fail("rename in the watched directory", errno, 0);
    }
    pf.fd = in; pf.events = POLLIN; pf.revents = 0;
    n = poll(&pf, 1, 1000) == 1 ? (int)read(in, buf, sizeof(buf)) : -1;
    for (p = buf; n > 0 && p < buf + n; p += sizeof(struct inotify_event) + ((struct inotify_event *)p)->len) {
        struct inotify_event *e = (struct inotify_event *)p;

        if (e->mask == IN_MOVED_FROM && strcmp(e->name, "a") == 0) {
            cookie = e->cookie;
            seen |= 1;
        } else if (e->mask == IN_MOVED_TO && strcmp(e->name, "b") == 0 && e->cookie == cookie && cookie != 0) {
            seen |= 2;
        }
    }
    if (seen != 3) {
        ok = fail("poll sees inotify readable; a rename is MOVED_FROM and MOVED_TO with one cookie", n, seen);
    }
    unlink("/tmp/evn/b");
    rmdir("/tmp/evn");
    close(in);
    return ok;
}

int main(void) {
    int ok = 1;

    printf("EVENTS_PHASE: steps 1 to 6\n");
    fflush(stdout);
    ok &= socket_checks();
    ok &= select_checks();
    ok &= mask_checks();
    ok &= event_fd_checks();
    ok &= epoll_checks();
    ok &= inotify_checks();
    if (ok) {
        printf("EVENTS_OK: poll, ppoll, select and pselect6; sockets say what they can do;"
               " eventfd, timerfd and signalfd wait and wake; epoll; inotify\n");
    }
    fflush(stdout);
    return ok ? 0 : 1;
}
