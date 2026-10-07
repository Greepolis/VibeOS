/* Sockets (docs/abi/ L5), from ring 3, through a real C library.
 *
 * Every check prints SOCK_FAIL with its reason on a line of its own, and the
 * program prints SOCKETS_OK only if all of them held; the boot gate reads that
 * line (sockets_l5_failed). Written and run on Linux first: each expectation
 * is what Linux answered, not what this kernel was meant to.
 *
 * Step 1: the rest of the BSD calls - names, options, accept4, readv and
 * writev on a socket, sendmmsg and recvmmsg.
 * Steps 2 and 3: local sockets, stream and datagram, by path and in the
 * abstract namespace, between two processes, with a descriptor passed.
 * Step 4: a name looked up through the C library (/etc/hosts).
 *
 * With an argument, `resolve NAME`, it looks NAME up with getaddrinfo and
 * prints the first IPv4 address - the DNS check, which the boot's script runs
 * only when the host can resolve the name itself. */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

static int fail(const char *what, long a, long b) {
    printf("SOCK_FAIL: %s (%ld %ld)\n", what, a, b);
    fflush(stdout);
    return 0;
}

/* Step 1: the BSD calls on IP sockets, with nobody at the other end. */
static int check_bsd(void) {
    int ok = 1, u, l, v, a;
    struct sockaddr_in sin;
    socklen_t len;

    u = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (u < 0) {
        return fail("socket(AF_INET, SOCK_DGRAM)", u, errno);
    }
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    if (bind(u, (struct sockaddr *)&sin, sizeof(sin)) != 0) {
        ok = fail("bind to port 0", -1, errno);
    }
    len = sizeof(sin);
    if (getsockname(u, (struct sockaddr *)&sin, &len) != 0 || len != sizeof(sin) || ntohs(sin.sin_port) == 0) {
        ok = fail("getsockname names the port bind chose", ntohs(sin.sin_port), errno);
    }
    len = sizeof(sin);
    if (getpeername(u, (struct sockaddr *)&sin, &len) == 0 || errno != ENOTCONN) {
        ok = fail("getpeername of an unconnected socket is ENOTCONN", 0, errno);
    }
    v = 5000;
    len = sizeof(v);
    if (setsockopt(u, SOL_SOCKET, SO_SNDBUF, &v, sizeof(v)) != 0 ||
        getsockopt(u, SOL_SOCKET, SO_SNDBUF, &v, &len) != 0 || v != 10000) {
        ok = fail("SO_SNDBUF reads back doubled", v, errno);
    }
    len = sizeof(v);
    if (getsockopt(u, SOL_SOCKET, SO_TYPE, &v, &len) != 0 || v != SOCK_DGRAM) {
        ok = fail("SO_TYPE", v, errno);
    }
    len = sizeof(v);
    if (getsockopt(u, SOL_SOCKET, SO_ERROR, &v, &len) != 0 || v != 0) {
        ok = fail("SO_ERROR of a quiet socket is 0", v, errno);
    }
    /* A datagram socket with no peer and no address has nowhere to send. */
    if (send(u, "x", 1, MSG_NOSIGNAL) != -1 || errno != EDESTADDRREQ) {
        ok = fail("send with no peer is EDESTADDRREQ", 0, errno);
    }
    close(u);

    l = socket(AF_INET, SOCK_STREAM, 0);
    v = 1;
    if (l < 0 || setsockopt(l, IPPROTO_TCP, TCP_NODELAY, &v, sizeof(v)) != 0) {
        ok = fail("TCP_NODELAY", l, errno);
    }
    if (listen(l, 4) != 0) {
        ok = fail("listen binds a port of its own", -1, errno);
    }
    len = sizeof(v);
    if (getsockopt(l, SOL_SOCKET, SO_ACCEPTCONN, &v, &len) != 0 || v != 1) {
        ok = fail("SO_ACCEPTCONN of a listener", v, errno);
    }
    /* accept4's SOCK_NONBLOCK is the new socket's; the listener's own flag
     * decides whether accept4 waits. */
    fcntl(l, F_SETFL, O_NONBLOCK);
    a = accept4(l, 0, 0, SOCK_CLOEXEC);
    if (a != -1 || errno != EAGAIN) {
        ok = fail("accept4 with nobody waiting on a non-blocking listener is EAGAIN", a, errno);
    }
    close(l);
    return ok;
}

/* readv and writev on a stream pair, then sendmmsg and recvmmsg on a datagram
 * pair. */
static int check_vectors(void) {
    int ok = 1, sv[2], i;
    char a[4], b[8], buf[32];
    struct iovec iov[2];
    struct mmsghdr mm[2];
    struct iovec mi[2];

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        return fail("socketpair(AF_UNIX, SOCK_STREAM)", -1, errno);
    }
    iov[0].iov_base = "abc";
    iov[0].iov_len = 3;
    iov[1].iov_base = "defgh";
    iov[1].iov_len = 5;
    if (writev(sv[0], iov, 2) != 8) {
        ok = fail("writev on a socket", -1, errno);
    }
    iov[0].iov_base = a;
    iov[0].iov_len = sizeof(a);
    iov[1].iov_base = b;
    iov[1].iov_len = sizeof(b);
    if (readv(sv[1], iov, 2) != 8 || memcmp(a, "abcd", 4) != 0 || memcmp(b, "efgh", 4) != 0) {
        ok = fail("readv on a socket scatters", 0, errno);
    }
    close(sv[0]);
    close(sv[1]);

    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) != 0) {
        return fail("socketpair(AF_UNIX, SOCK_DGRAM)", -1, errno);
    }
    memset(mm, 0, sizeof(mm));
    mi[0].iov_base = "one";
    mi[0].iov_len = 3;
    mi[1].iov_base = "three";
    mi[1].iov_len = 5;
    for (i = 0; i < 2; i++) {
        mm[i].msg_hdr.msg_iov = &mi[i];
        mm[i].msg_hdr.msg_iovlen = 1;
    }
    if (sendmmsg(sv[0], mm, 2, 0) != 2 || mm[0].msg_len != 3 || mm[1].msg_len != 5) {
        ok = fail("sendmmsg sends two and says each length", mm[0].msg_len, mm[1].msg_len);
    }
    memset(mm, 0, sizeof(mm));
    mi[0].iov_base = buf;
    mi[0].iov_len = 16;
    mi[1].iov_base = buf + 16;
    mi[1].iov_len = 16;
    for (i = 0; i < 2; i++) {
        mm[i].msg_hdr.msg_iov = &mi[i];
        mm[i].msg_hdr.msg_iovlen = 1;
    }
    if (recvmmsg(sv[1], mm, 2, MSG_DONTWAIT, 0) != 2 || mm[0].msg_len != 3 || mm[1].msg_len != 5 ||
        memcmp(buf, "one", 3) != 0 || memcmp(buf + 16, "three", 5) != 0) {
        ok = fail("recvmmsg receives both, whole", mm[0].msg_len, mm[1].msg_len);
    }
    if (recv(sv[1], buf, 2, MSG_DONTWAIT) != -1 || errno != EAGAIN) {
        ok = fail("and nothing is left", 0, errno);
    }
    if (send(sv[0], "message", 7, 0) != 7 || recv(sv[1], buf, 3, MSG_TRUNC) != 7) {
        ok = fail("MSG_TRUNC says a datagram's whole length", 0, errno);
    }
    close(sv[0]);
    close(sv[1]);
    return ok;
}

/* A descriptor passed over a local socket. */
static int send_fd(int s, int fd) {
    char c = 'F';
    struct iovec iov = { &c, 1 };
    union {
        struct cmsghdr h;
        char b[CMSG_SPACE(sizeof(int))];
    } u;
    struct msghdr m;

    memset(&m, 0, sizeof(m));
    memset(&u, 0, sizeof(u));
    m.msg_iov = &iov;
    m.msg_iovlen = 1;
    m.msg_control = u.b;
    m.msg_controllen = sizeof(u.b);
    CMSG_FIRSTHDR(&m)->cmsg_level = SOL_SOCKET;
    CMSG_FIRSTHDR(&m)->cmsg_type = SCM_RIGHTS;
    CMSG_FIRSTHDR(&m)->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(CMSG_FIRSTHDR(&m)), &fd, sizeof(int));
    return sendmsg(s, &m, 0) == 1 ? 0 : -1;
}

static int recv_fd(int s) {
    char c;
    struct iovec iov = { &c, 1 };
    union {
        struct cmsghdr h;
        char b[CMSG_SPACE(sizeof(int))];
    } u;
    struct msghdr m;
    struct cmsghdr *h;
    int fd = -1;

    memset(&m, 0, sizeof(m));
    m.msg_iov = &iov;
    m.msg_iovlen = 1;
    m.msg_control = u.b;
    m.msg_controllen = sizeof(u.b);
    if (recvmsg(s, &m, MSG_CMSG_CLOEXEC) != 1 || c != 'F') {
        return -1;
    }
    h = CMSG_FIRSTHDR(&m);
    if (!h || h->cmsg_level != SOL_SOCKET || h->cmsg_type != SCM_RIGHTS || h->cmsg_len != CMSG_LEN(sizeof(int))) {
        return -2;
    }
    memcpy(&fd, CMSG_DATA(h), sizeof(int));
    return fd;
}

#define L5_PATH "/tmp/l5.sock"

/* A server and a client in two processes: a named stream socket, the client's
 * credentials as the server sees them, a pipe's read end passed back, and a
 * datagram in the abstract namespace. */
static int check_local(void) {
    int ok = 1, srv, c, p[2], st, fd;
    struct sockaddr_un sun;
    struct ucred cr;
    socklen_t len;
    struct stat sb;
    pid_t kid;
    char buf[64];

    unlink(L5_PATH);
    memset(&sun, 0, sizeof(sun));
    sun.sun_family = AF_UNIX;
    strcpy(sun.sun_path, L5_PATH);
    srv = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv < 0 || bind(srv, (struct sockaddr *)&sun, sizeof(sun)) != 0 || listen(srv, 2) != 0) {
        return fail("a named listener", srv, errno);
    }
    if (stat(L5_PATH, &sb) != 0 || !S_ISSOCK(sb.st_mode)) {
        ok = fail("bind made a socket node", (long)sb.st_mode, errno);
    }
    if (pipe(p) != 0) {
        return fail("pipe", -1, errno);
    }
    kid = fork();
    if (kid == 0) {
        int s = socket(AF_UNIX, SOCK_STREAM, 0), q[2], r = 0;

        close(srv);
        if (connect(s, (struct sockaddr *)&sun, sizeof(sun)) != 0) {
            _exit(10);
        }
        if (write(s, "hello", 5) != 5) {
            _exit(11);
        }
        if (read(s, buf, sizeof(buf)) != 5 || memcmp(buf, "world", 5) != 0) {
            _exit(12);
        }
        /* The other way: a pipe of the child's own, its read end passed to
         * the parent, which reads what the child writes after. */
        if (pipe(q) != 0 || send_fd(s, q[0]) != 0) {
            _exit(13);
        }
        close(q[0]);
        if (write(q[1], "through", 7) != 7) {
            _exit(14);
        }
        close(q[1]);
        /* The end of the stream is the parent's shutdown. */
        if (read(s, buf, sizeof(buf)) != 0) {
            r = 15;
        }
        close(s);
        _exit(r);
    }
    c = accept4(srv, 0, 0, SOCK_CLOEXEC);
    if (c < 0) {
        ok = fail("accept4 on a named listener", c, errno);
    } else {
        len = sizeof(cr);
        if (getsockopt(c, SOL_SOCKET, SO_PEERCRED, &cr, &len) != 0 || cr.pid != kid || cr.uid != getuid()) {
            ok = fail("SO_PEERCRED names the client", cr.pid, kid);
        }
        if (read(c, buf, sizeof(buf)) != 5 || memcmp(buf, "hello", 5) != 0 || write(c, "world", 5) != 5) {
            ok = fail("bytes cross a named connection", 0, errno);
        }
        fd = recv_fd(c);
        if (fd < 0) {
            ok = fail("SCM_RIGHTS arrives", fd, errno);
        } else {
            if (read(fd, buf, sizeof(buf)) != 7 || memcmp(buf, "through", 7) != 0) {
                ok = fail("the passed descriptor reads the child's pipe", 0, errno);
            }
            if (!(fcntl(fd, F_GETFD) & FD_CLOEXEC)) {
                ok = fail("MSG_CMSG_CLOEXEC", fcntl(fd, F_GETFD), 0);
            }
            close(fd);
        }
        shutdown(c, SHUT_WR);
    }
    if (waitpid(kid, &st, 0) != kid || !WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        ok = fail("the client said everything held", WIFEXITED(st) ? WEXITSTATUS(st) : -1, st);
    }
    if (c >= 0) {
        close(c);
    }
    close(srv);
    close(p[0]);
    close(p[1]);
    if (unlink(L5_PATH) != 0) {
        ok = fail("the node is unlinked like a file", -1, errno);
    }

    /* The abstract namespace: no node, a name that goes with its socket. */
    {
        int d1 = socket(AF_UNIX, SOCK_DGRAM, 0), d2 = socket(AF_UNIX, SOCK_DGRAM, 0);
        socklen_t al = offsetof(struct sockaddr_un, sun_path) + 1 + 9;
        struct sockaddr_un from;

        memset(&sun, 0, sizeof(sun));
        sun.sun_family = AF_UNIX;
        memcpy(sun.sun_path, "\0vibeos-l5", 10);
        if (bind(d1, (struct sockaddr *)&sun, al) != 0) {
            ok = fail("bind in the abstract namespace", -1, errno);
        }
        if (sendto(d2, "abstract", 8, 0, (struct sockaddr *)&sun, al) != 8) {
            ok = fail("sendto an abstract name", -1, errno);
        }
        len = sizeof(from);
        if (recvfrom(d1, buf, sizeof(buf), 0, (struct sockaddr *)&from, &len) != 8 ||
            memcmp(buf, "abstract", 8) != 0 || len != 0) {
            ok = fail("recvfrom: the datagram, from an unnamed sender", (long)len, errno);
        }
        close(d1);
        close(d2);
    }
    return ok;
}

/* getaddrinfo through the C library, out of /etc/hosts. */
static int check_hosts(void) {
    struct addrinfo hint, *res = 0;
    int ok = 1, r;

    memset(&hint, 0, sizeof(hint));
    hint.ai_family = AF_INET;
    hint.ai_socktype = SOCK_STREAM;
    r = getaddrinfo("localhost", "80", &hint, &res);
    if (r != 0 || !res || ((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr != htonl(0x7F000001u)) {
        ok = fail("getaddrinfo(localhost)", r, errno);
    }
    if (res) {
        freeaddrinfo(res);
    }
    return ok;
}

static int resolve(const char *name) {
    struct addrinfo hint, *res = 0;
    char ip[INET_ADDRSTRLEN];
    int r;

    memset(&hint, 0, sizeof(hint));
    hint.ai_family = AF_INET;
    hint.ai_socktype = SOCK_STREAM;
    r = getaddrinfo(name, 0, &hint, &res);
    if (r != 0 || !res) {
        printf("SOCK_RESOLVE_FAILED %s %d %s\n", name, r, gai_strerror(r));
        return 1;
    }
    inet_ntop(AF_INET, &((struct sockaddr_in *)res->ai_addr)->sin_addr, ip, sizeof(ip));
    printf("SOCK_RESOLVED %s %s\n", name, ip);
    freeaddrinfo(res);
    return 0;
}

int main(int argc, char **argv) {
    int ok = 1;

    setvbuf(stdout, 0, _IONBF, 0);
    if (argc == 3 && strcmp(argv[1], "resolve") == 0) {
        return resolve(argv[2]);
    }
    ok &= check_bsd();
    ok &= check_vectors();
    ok &= check_local();
    ok &= check_hosts();
    if (ok) {
        printf("SOCKETS_OK: names, options, accept4, vectors, mmsg; local stream and datagram,"
               " by path and abstract, SO_PEERCRED, SCM_RIGHTS; getaddrinfo\n");
    }
    return ok ? 0 : 1;
}
