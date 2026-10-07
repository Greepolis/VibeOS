/* The Linux socket syscalls.
 *
 * Lifted out of arch_hw.c (C4), portable since A2, and since docs/abi/ L5 a
 * translation and nothing else: struct sockaddr_in and sockaddr_un with their
 * lengths, struct msghdr and its control messages, MSG_* and SO_* by Linux's
 * numbers - in, into the kernel's own forms (vibeos/sockops.h), and back out.
 * What a call does is the socket type's - the stack's IP sockets
 * (kernel/abi/files/socket.c) or the local ones (unixsock.c) - reached through
 * the type's table, so nothing here asks which family it holds except where
 * Linux's answers differ by family.
 *
 * User memory a row can declare is declared there; what a row cannot - an
 * address whose length is an argument, the iovecs and control messages a
 * msghdr points at, an option's value - is judged here, by the two helpers
 * below, before anything is copied: vibeos_uaccess_copy on the machine is
 * fault-tolerant and not permission-checked (M-082). */

#include <stddef.h>

#include "linux_internal.h"

/* ---- user memory ------------------------------------------------------------------- */

static long linux_net_in(void *k, uint64_t uptr, uint64_t len) {
    if (len == 0u) {
        return 0;
    }
    if (!linux_user_ok(uptr, len, 0) || vibeos_uaccess_copy(k, (const void *)(uintptr_t)uptr, len) != 0) {
        return -VIBEOS_EFAULT;
    }
    return 0;
}

static long linux_net_out(uint64_t uptr, const void *k, uint64_t len) {
    if (len == 0u) {
        return 0;
    }
    if (!linux_user_ok(uptr, len, 1) || vibeos_uaccess_copy((void *)(uintptr_t)uptr, k, len) != 0) {
        return -VIBEOS_EFAULT;
    }
    return 0;
}

/* Network order is big-endian and this machine is not; bytes, so it does not
 * matter which the host is. */
static uint16_t linux_be16(uint16_t wire) {
    const uint8_t *b = (const uint8_t *)&wire;
    return (uint16_t)(((uint16_t)b[0] << 8) | b[1]);
}

static uint32_t linux_be32(uint32_t wire) {
    const uint8_t *b = (const uint8_t *)&wire;
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | (uint32_t)b[3];
}

/* ---- addresses ----------------------------------------------------------------------- */

#define LINUX_SA_MAX 128u   /* sizeof(struct sockaddr_storage) */

/* A sockaddr of `len` bytes out of user memory. *linux_family is what it said;
 * the kernel's family is set for the two this kernel has, and VIBEOS_SA_NONE
 * for AF_UNSPEC and for any other - the caller decides which error that is. */
static long linux_sa_in(uint64_t uptr, uint64_t len64, vibeos_sockaddr_t *a, uint32_t *linux_family) {
    uint8_t raw[sizeof(linux_sockaddr_un_t)];
    int32_t len = VIBEOS_ARG_INT(len64);
    uint32_t n, k;
    long r;

    a->family = VIBEOS_SA_NONE;
    a->ip = 0;
    a->port = 0;
    a->path_len = 0;
    if (len < 0 || (uint32_t)len > LINUX_SA_MAX) {
        return -VIBEOS_EINVAL;
    }
    if ((uint32_t)len < sizeof(uint16_t)) {
        return -VIBEOS_EINVAL;
    }
    n = (uint32_t)len < sizeof(raw) ? (uint32_t)len : (uint32_t)sizeof(raw);
    if ((r = linux_net_in(raw, uptr, n)) != 0) {
        return r;
    }
    *linux_family = (uint32_t)raw[0] | ((uint32_t)raw[1] << 8);
    if (*linux_family == LINUX_AF_INET) {
        const linux_sockaddr_in_t *in = (const linux_sockaddr_in_t *)(const void *)raw;

        if ((uint32_t)len < sizeof(linux_sockaddr_in_t)) {
            return -VIBEOS_EINVAL;
        }
        a->family = VIBEOS_SA_INET;
        a->port = linux_be16(in->sin_port);
        a->ip = linux_be32(in->sin_addr);
    } else if (*linux_family == LINUX_AF_UNIX) {
        n -= 2u;
        a->family = VIBEOS_SA_UNIX;
        if (n != 0u && raw[2] == 0) {
            a->path_len = (uint16_t)n;   /* the abstract namespace: every byte counts */
        } else {
            for (k = 0; k < n && raw[2u + k] != 0; k++) {
            }
            a->path_len = (uint16_t)k;
        }
        for (k = 0; k < a->path_len; k++) {
            a->path[k] = (char)raw[2u + k];
        }
    }
    return 0;
}

/* A kernel address back out as a sockaddr: as many bytes as the caller's
 * length says, and the real length in its place - Linux's value-result
 * argument. A null address is no address asked for. */
static long linux_sa_out(uint64_t addr, uint64_t lenp, const vibeos_sockaddr_t *a) {
    uint8_t raw[sizeof(linux_sockaddr_un_t)];
    int32_t len;
    uint32_t real, k;
    long r;

    if (addr == 0u || lenp == 0u) {
        return 0;
    }
    if ((r = linux_net_in(&len, lenp, sizeof(len))) != 0) {
        return r;
    }
    if (len < 0) {
        return -VIBEOS_EINVAL;
    }
    for (k = 0; k < sizeof(raw); k++) {
        raw[k] = 0;
    }
    if (a->family == VIBEOS_SA_INET) {
        linux_sockaddr_in_t *in = (linux_sockaddr_in_t *)(void *)raw;

        in->sin_family = LINUX_AF_INET;
        in->sin_port = linux_be16(a->port);   /* the swap is its own inverse */
        in->sin_addr = linux_be32(a->ip);
        real = sizeof(linux_sockaddr_in_t);
    } else {
        raw[0] = (uint8_t)LINUX_AF_UNIX;
        for (k = 0; k < a->path_len && k < LINUX_UNIX_PATH_MAX; k++) {
            raw[2u + k] = (uint8_t)a->path[k];
        }
        /* A path's length counts its terminating zero; an abstract name's does
         * not have one; an unnamed socket is the family alone. */
        real = 2u + a->path_len + ((a->path_len != 0u && a->path[0] != 0 && a->path_len < LINUX_UNIX_PATH_MAX) ? 1u : 0u);
    }
    if ((r = linux_net_out(addr, raw, (uint32_t)len < real ? (uint32_t)len : real)) != 0) {
        return r;
    }
    len = (int32_t)real;
    return linux_net_out(lenp, &len, sizeof(len));
}

/* A local socket's path as the kernel's absolute one, against the caller's
 * working directory and root, walked: `create` for a bind, which then makes the
 * node. -ENAMETOOLONG for a path that does not fit a sockaddr_un. */
static long linux_unix_walk(vibeos_sockaddr_t *a, int create, vibeos_path_t *w) {
    char path[VIBEOS_SA_PATH_MAX + 1u], abs[VIBEOS_PATH_MAX], root[VIBEOS_PATH_MAX], cwd[VIBEOS_PATH_MAX];
    vibeos_procstate_t *ps = ks_ps(ks_current());
    uint32_t k;
    int r;

    if (!ps) {
        return -VIBEOS_EINVAL;
    }
    for (k = 0; k < a->path_len; k++) {
        path[k] = a->path[k];
    }
    path[k] = 0;
    ks_lock(&ps->files_lock, __func__);
    for (k = 0; k < sizeof(root); k++) {
        root[k] = ps->root[k];
        cwd[k] = ps->cwd[k];
    }
    ks_unlock(&ps->files_lock);
    if ((r = vibeos_path_normalize(root, cwd, path, abs, sizeof(abs))) != 0) {
        return r;
    }
    for (k = 0; abs[k]; k++) {
    }
    if (k > VIBEOS_SA_PATH_MAX) {
        return -VIBEOS_ENAMETOOLONG;
    }
    if ((r = vibeos_path_walk("/", "/", abs, create ? (VIBEOS_PATH_CREATE | VIBEOS_PATH_NOFOLLOW) : 0u, w)) != 0) {
        return r;
    }
    for (k = 0; abs[k]; k++) {
        a->path[k] = abs[k];
    }
    a->path_len = (uint16_t)k;
    return 0;
}

/* A path a connect or a send names: the socket node must be there and the
 * caller may write to it, as Linux asks. */
static long linux_unix_target(vibeos_sockaddr_t *a) {
    vibeos_path_t w;
    long r;

    if (a->family != VIBEOS_SA_UNIX || a->path_len == 0u || a->path[0] == 0) {
        return 0;
    }
    if ((r = linux_unix_walk(a, 0, &w)) != 0) {
        return r;
    }
    if ((w.node.mode & VIBEOS_S_IFMT) != VIBEOS_S_IFSOCK) {
        return -VIBEOS_ECONNREFUSED;
    }
    return linux_may(&w.node, VIBEOS_MAY_WRITE);
}

/* ---- descriptors ------------------------------------------------------------------- */

/* The socket description a descriptor names, with a reference the caller puts;
 * 0 and *err set when the descriptor is not open or not a socket. */
static vibeos_file_t *linux_socket_get(uint64_t fd, long *err) {
    vibeos_file_t *f = linux_file_get(fd);

    if (!f) {
        *err = -VIBEOS_EBADF;
        return 0;
    }
    if (!f->ops->sockops) {
        vibeos_file_put(f);
        *err = -VIBEOS_ENOTSOCK;
        return 0;
    }
    return f;
}

static uint32_t linux_fd_flags_of(uint64_t type) {
    return (type & LINUX_SOCK_CLOEXEC) ? VIBEOS_FD_CLOEXEC : 0u;
}

/* What socket and socketpair are asked for, as the kernel's kind. */
static long linux_sock_kind(uint64_t domain, uint64_t type64, uint64_t protocol, int *kind) {
    uint32_t type = (uint32_t)type64, base = type & 0xFu;

    if (type & ~(uint32_t)(0xFu | LINUX_SOCK_NONBLOCK | LINUX_SOCK_CLOEXEC)) {
        return -VIBEOS_EINVAL;
    }
    if (domain != LINUX_AF_INET && domain != LINUX_AF_UNIX) {
        /* A family this kernel has no sockets of. Not EINVAL: a C library
         * asks for a local socket to reach a name-service daemon before it
         * gives up on a user or group name, and takes "no such family" as "no
         * daemon" and anything else as a failure (seven LTP tests stopped
         * there before AF_UNIX existed). */
        return -VIBEOS_EAFNOSUPPORT;
    }
    if (base == 0u || base > 10u) {
        return -VIBEOS_EINVAL;
    }
    if (base != LINUX_SOCK_STREAM && base != LINUX_SOCK_DGRAM) {
        return base == LINUX_SOCK_RAW && domain == LINUX_AF_INET ? -VIBEOS_EPROTONOSUPPORT
                                                                : -VIBEOS_ESOCKTNOSUPPORT;
    }
    *kind = base == LINUX_SOCK_STREAM ? VIBEOS_SOCK_STREAM : VIBEOS_SOCK_DGRAM;
    if (domain == LINUX_AF_UNIX) {
        return (protocol == 0u || protocol == LINUX_AF_UNIX) ? 0 : -VIBEOS_EPROTONOSUPPORT;
    }
    if (protocol == 0u || (base == LINUX_SOCK_STREAM && protocol == LINUX_IPPROTO_TCP) ||
        (base == LINUX_SOCK_DGRAM && protocol == LINUX_IPPROTO_UDP)) {
        return 0;
    }
    return -VIBEOS_EPROTONOSUPPORT;
}

static long linux_sys_socket(uint64_t domain, uint64_t type, uint64_t protocol) {
    vibeos_file_t *f;
    long err;
    int me, kind;

    if ((err = linux_sock_kind(domain, type, protocol, &kind)) != 0) {
        return err;
    }
    if ((me = ks_current()) < 0 || !ks_id(me)->is_user) {
        return -VIBEOS_EINVAL;
    }
    if (domain == LINUX_AF_UNIX) {
        f = vibeos_unix_create(kind, (type & LINUX_SOCK_NONBLOCK) ? VIBEOS_O_NONBLOCK : 0u, &err);
    } else {
        f = vibeos_sockfile_create(kind == VIBEOS_SOCK_STREAM ? VIBEOS_INET_SOCK_TCP : VIBEOS_INET_SOCK_UDP,
                                   ks_id(me)->tgid, &err);
        if (f && (type & LINUX_SOCK_NONBLOCK)) {
            f->flags |= VIBEOS_O_NONBLOCK;
        }
    }
    if (!f) {
        return err;
    }
    return linux_fd_install(f, linux_fd_flags_of(type), 0);
}

static long linux_sys_socketpair(uint64_t domain, uint64_t type, uint64_t protocol, uint64_t sv_uptr) {
    vibeos_file_t *a, *b;
    int32_t sv[2];
    long r;
    int kind;

    if ((r = linux_sock_kind(domain, type, protocol, &kind)) != 0) {
        return r;
    }
    if (domain != LINUX_AF_UNIX) {
        return -VIBEOS_EOPNOTSUPP;   /* an IP socket has no pair */
    }
    if ((r = vibeos_unix_pair(kind, (type & LINUX_SOCK_NONBLOCK) ? VIBEOS_O_NONBLOCK : 0u, &a, &b)) != 0) {
        return r;
    }
    if ((r = linux_fd_install(a, linux_fd_flags_of(type), 0)) < 0) {
        vibeos_file_put(b);
        return r;
    }
    sv[0] = (int32_t)r;
    if ((r = linux_fd_install(b, linux_fd_flags_of(type), 0)) < 0) {
        (void)linux_fd_close((uint64_t)sv[0]);
        return r;
    }
    sv[1] = (int32_t)r;
    if (vibeos_uaccess_copy((void *)(uintptr_t)sv_uptr, sv, sizeof(sv)) != 0) {
        (void)linux_fd_close((uint64_t)sv[0]);
        (void)linux_fd_close((uint64_t)sv[1]);
        return -VIBEOS_EFAULT;
    }
    return 0;
}

static long linux_sys_bind(uint64_t fd, uint64_t addr, uint64_t len) {
    vibeos_sockaddr_t a;
    uint32_t fam = 0;
    long r;
    vibeos_file_t *f = linux_socket_get(fd, &r);

    if (!f) {
        return r;
    }
    if ((r = linux_sa_in(addr, len, &a, &fam)) != 0) {
        goto out;
    }
    if (f->sk_family == VIBEOS_SA_INET) {
        vibeos_cred_t me;

        if (fam != LINUX_AF_INET) {
            /* AF_UNSPEC with any address is accepted, for old programs. */
            if (fam == LINUX_AF_UNSPEC && (uint32_t)VIBEOS_ARG_INT(len) >= sizeof(linux_sockaddr_in_t) &&
                a.ip == 0u) {
                a.family = VIBEOS_SA_INET;
            } else {
                r = -VIBEOS_EAFNOSUPPORT;
                goto out;
            }
        }
        linux_cred(&me);
        if (a.port != 0u && a.port < 1024u && me.euid != 0u) {
            r = -VIBEOS_EACCES;   /* a privileged port */
            goto out;
        }
    } else {
        vibeos_sockaddr_t now;
        vibeos_path_t w;
        vibeos_fs_node_t node;

        if (fam != LINUX_AF_UNIX) {
            r = -VIBEOS_EINVAL;
            goto out;
        }
        if (f->ops->sockops->name(f, 0, &now) == 0 && now.path_len != 0u) {
            r = -VIBEOS_EINVAL;   /* bound already: asked before a node is made */
            goto out;
        }
        if (a.path_len != 0u && a.path[0] != 0) {
            /* A path is a socket node, made here as any file is: the walk, the
             * permission to add a name, the umask, the owner. */
            if ((r = linux_unix_walk(&a, 1, &w)) != 0) {
                goto out;
            }
            if (w.exists) {
                r = -VIBEOS_EADDRINUSE;
                goto out;
            }
            if ((r = linux_may_add(&w)) != 0) {
                goto out;
            }
            if ((r = vibeos_fs_mknod(w.mnt, w.tail, VIBEOS_S_IFSOCK | (0777u & ~linux_umask()), &node)) != 0) {
                goto out;
            }
            linux_own_new(&w);
        }
    }
    r = f->ops->sockops->bind(f, &a);
out:
    vibeos_file_put(f);
    return r;
}

static long linux_sys_listen(uint64_t fd, uint64_t backlog) {
    long r;
    vibeos_file_t *f = linux_socket_get(fd, &r);

    if (!f) {
        return r;
    }
    r = f->ops->sockops->listen(f, VIBEOS_ARG_INT(backlog));
    vibeos_file_put(f);
    return r;
}

static long linux_sys_connect(uint64_t fd, uint64_t addr, uint64_t len) {
    vibeos_sockaddr_t a;
    uint32_t fam = 0;
    long r;
    vibeos_file_t *f = linux_socket_get(fd, &r);

    if (!f) {
        return r;
    }
    if ((r = linux_sa_in(addr, len, &a, &fam)) != 0) {
        goto out;
    }
    if (fam != LINUX_AF_UNSPEC && a.family != f->sk_family) {
        r = f->sk_family == VIBEOS_SA_UNIX ? -VIBEOS_EINVAL : -VIBEOS_EAFNOSUPPORT;
        goto out;
    }
    if ((r = linux_unix_target(&a)) != 0) {
        goto out;
    }
    r = f->ops->sockops->connect(f, &a);
out:
    vibeos_file_put(f);
    return r;
}

static long linux_sys_accept4(uint64_t fd, uint64_t addr, uint64_t lenp, uint64_t flags) {
    vibeos_file_t *child = 0;
    vibeos_sockaddr_t peer;
    long r, nfd;
    vibeos_file_t *f;

    if (flags & ~(uint64_t)(LINUX_SOCK_NONBLOCK | LINUX_SOCK_CLOEXEC)) {
        return -VIBEOS_EINVAL;
    }
    if (!(f = linux_socket_get(fd, &r))) {
        return r;
    }
    /* A socket that does not listen is EINVAL before anything is read: Linux
     * asks the socket first (LTP's accept01, a bad buffer on a fresh socket). */
    {
        int32_t on = 0;
        uint32_t olen = sizeof(on);

        if (f->sk_type == VIBEOS_SOCK_STREAM &&
            f->ops->sockops->getopt(f, VIBEOS_SO_ACCEPTCONN, &on, &olen) == 0 && on == 0) {
            vibeos_file_put(f);
            return -VIBEOS_EINVAL;
        }
    }
    /* A bad peer-address pointer is refused before a connection is consumed
     * (M-032): the length is read and the address judged first. */
    if (addr != 0u) {
        int32_t len;

        if ((r = linux_net_in(&len, lenp, sizeof(len))) != 0 || (len > 0 && !linux_user_ok(addr, (uint64_t)len, 1))) {
            vibeos_file_put(f);
            return r ? r : -VIBEOS_EFAULT;
        }
        if (len < 0) {
            vibeos_file_put(f);
            return -VIBEOS_EINVAL;
        }
    }
    r = f->ops->sockops->accept(f, ks_id(ks_current())->tgid, &child, &peer);
    vibeos_file_put(f);
    if (r != 0) {
        return r;
    }
    if (flags & LINUX_SOCK_NONBLOCK) {
        child->flags |= VIBEOS_O_NONBLOCK;
    }
    nfd = linux_fd_install(child, linux_fd_flags_of(flags), 0);
    if (nfd < 0) {
        return nfd;   /* the install released the child, which closed it */
    }
    if (linux_sa_out(addr, lenp, &peer) != 0) {
        /* The pointer went bad after the pre-check: undo the accept rather than
         * hand back a connection with no way to learn of it. */
        (void)linux_fd_close((uint64_t)nfd);
        return -VIBEOS_EFAULT;
    }
    return nfd;
}

static long linux_sys_name(uint64_t fd, uint64_t addr, uint64_t lenp, int peer) {
    vibeos_sockaddr_t a;
    long r;
    vibeos_file_t *f = linux_socket_get(fd, &r);

    if (!f) {
        return r;
    }
    r = f->ops->sockops->name(f, peer, &a);
    vibeos_file_put(f);
    if (r != 0) {
        return r;
    }
    if (addr == 0u || lenp == 0u) {
        /* Linux writes the name and its length; a null pointer takes neither
         * (LTP's getsockname01 and getpeername01: a null length is EFAULT). */
        return -VIBEOS_EFAULT;
    }
    return linux_sa_out(addr, lenp, &a);
}

static long linux_sys_shutdown(uint64_t fd, uint64_t how) {
    long r;
    vibeos_file_t *f = linux_socket_get(fd, &r);
    int bits;

    if (!f) {
        return r;
    }
    switch (VIBEOS_ARG_INT(how)) {
        case LINUX_SHUT_RD:   bits = VIBEOS_SHUT_RD; break;
        case LINUX_SHUT_WR:   bits = VIBEOS_SHUT_WR; break;
        case LINUX_SHUT_RDWR: bits = VIBEOS_SHUT_RD | VIBEOS_SHUT_WR; break;
        default:              bits = 0; break;
    }
    r = bits ? f->ops->sockops->shutdown(f, bits) : -VIBEOS_EINVAL;
    vibeos_file_put(f);
    return r;
}

/* ---- sending and receiving -------------------------------------------------------- */

static uint32_t linux_msg_flags_in(uint64_t flags) {
    uint32_t m = 0;

    m |= (flags & LINUX_MSG_PEEK) ? VIBEOS_MSG_PEEK : 0u;
    m |= (flags & LINUX_MSG_DONTWAIT) ? VIBEOS_MSG_DONTWAIT : 0u;
    m |= (flags & LINUX_MSG_NOSIGNAL) ? VIBEOS_MSG_NOSIGNAL : 0u;
    m |= (flags & LINUX_MSG_WAITALL) ? VIBEOS_MSG_WAITALL : 0u;
    m |= (flags & LINUX_MSG_TRUNC) ? VIBEOS_MSG_TRUNC : 0u;
    return m;
}

static uint32_t linux_msg_flags_out(uint32_t m) {
    return ((m & VIBEOS_MSG_TRUNC) ? LINUX_MSG_TRUNC : 0u) | ((m & VIBEOS_MSG_CTRUNC) ? LINUX_MSG_CTRUNC : 0u);
}

static void linux_msg_init(vibeos_msg_t *m, vibeos_sockaddr_t *addr, const vibeos_uiov_t *iov, uint32_t n,
                           uint32_t flags) {
    m->addr = addr;
    m->iov = iov;
    m->iovcnt = n;
    m->flags = flags;
    m->nrights = 0;
    m->max_rights = VIBEOS_MSG_RIGHTS_MAX;
    m->full_len = 0;
}

static long linux_sys_sendto(uint64_t fd, uint64_t buf, uint64_t len, uint64_t flags, uint64_t addr,
                             uint64_t alen) {
    vibeos_sockaddr_t a;
    vibeos_uiov_t iov;
    vibeos_msg_t m;
    uint32_t fam = 0;
    long r;
    vibeos_file_t *f = linux_socket_get(fd, &r);

    if (!f) {
        return r;
    }
    if (flags & LINUX_MSG_OOB) {
        r = -VIBEOS_EOPNOTSUPP;
        goto out;
    }
    if (addr != 0u) {
        if ((r = linux_sa_in(addr, alen, &a, &fam)) != 0 || (r = linux_unix_target(&a)) != 0) {
            goto out;
        }
        if (fam != LINUX_AF_UNSPEC && a.family != f->sk_family && f->sk_type == VIBEOS_SOCK_DGRAM) {
            r = f->sk_family == VIBEOS_SA_UNIX ? -VIBEOS_EINVAL : -VIBEOS_EAFNOSUPPORT;
            goto out;
        }
    }
    iov.base = buf;
    iov.len = len;
    linux_msg_init(&m, addr != 0u ? &a : 0, &iov, 1, linux_msg_flags_in(flags));
    r = f->ops->sockops->sendmsg(f, &m);
out:
    vibeos_file_put(f);
    return r;
}

static long linux_sys_recvfrom(uint64_t fd, uint64_t buf, uint64_t len, uint64_t flags, uint64_t addr,
                               uint64_t lenp) {
    vibeos_sockaddr_t a;
    vibeos_uiov_t iov;
    vibeos_msg_t m;
    long r;
    vibeos_file_t *f = linux_socket_get(fd, &r);

    if (!f) {
        return r;
    }
    if (flags & LINUX_MSG_OOB) {
        vibeos_file_put(f);
        return -VIBEOS_EINVAL;   /* no urgent data is ever there */
    }
    if (flags & LINUX_MSG_ERRQUEUE) {
        vibeos_file_put(f);
        return -VIBEOS_EAGAIN;   /* nor anything in an error queue (LTP's recv01) */
    }
    /* The source-address pointer is refused before the datagram is dequeued, or a
     * bad pointer loses it with no error (M-032). A stream names no source, so
     * Linux never writes there and a bad pointer is no error (recvfrom01). */
    if (addr != 0u) {
        int32_t alen;

        if ((r = linux_net_in(&alen, lenp, sizeof(alen))) != 0 || alen < 0) {
            vibeos_file_put(f);
            return r ? r : -VIBEOS_EINVAL;   /* a negative length, even a stream's (recvfrom01) */
        }
        if (f->sk_type == VIBEOS_SOCK_DGRAM && alen > 0 && !linux_user_ok(addr, (uint64_t)alen, 1)) {
            vibeos_file_put(f);
            return -VIBEOS_EFAULT;
        }
    }
    iov.base = buf;
    iov.len = len;
    a.family = VIBEOS_SA_NONE;
    a.path_len = 0;
    linux_msg_init(&m, &a, &iov, 1, linux_msg_flags_in(flags));
    r = f->ops->sockops->recvmsg(f, &m);
    vibeos_file_put(f);
    /* No name - a stream's bytes, an unnamed sender's datagram - is a length
     * of 0, as Linux writes it. */
    if (r >= 0 && addr != 0u) {
        int32_t zero = 0;

        if (a.family != VIBEOS_SA_NONE ? linux_sa_out(addr, lenp, &a) != 0
                                       : linux_net_out(lenp, &zero, sizeof(zero)) != 0) {
            return -VIBEOS_EFAULT;
        }
    }
    return r;
}

/* The iovecs a msghdr names, judged one by one: more than fit in a page is
 * EMSGSIZE where Linux's limit is 1024 - a page of iovecs is 256. *pg is the
 * page when one was taken, which the caller gives back. */
#define LINUX_MSG_IOV_SMALL 8u
static long linux_msg_iov(const linux_msghdr_t *mh, vibeos_uiov_t *small, vibeos_uiov_t **out, void **pg,
                          int write) {
    uint64_t n = mh->msg_iovlen, i, total = 0;
    vibeos_uiov_t *iov = small;
    long r;

    *pg = 0;
    if (n > LINUX_UIO_MAXIOV) {
        return -VIBEOS_EMSGSIZE;
    }
    if (n * sizeof(vibeos_uiov_t) == 0u) {
        /* Nothing to read. Said here, on the byte count the copy below is
         * given, rather than left to that copy - which returns at once for 0:
         * clang's analyzer does not follow n to n * 16, so it took "n is not 0"
         * and "the copy read nothing" together and called iov[0] garbage. */
        *out = iov;
        return 0;
    }
    if (n > LINUX_MSG_IOV_SMALL) {
        if (n * sizeof(vibeos_uiov_t) > 4096u) {
            return -VIBEOS_EMSGSIZE;
        }
        if (!(*pg = ks_page_alloc())) {
            return -VIBEOS_ENOMEM;
        }
        iov = (vibeos_uiov_t *)*pg;
    }
    if ((r = linux_net_in(iov, mh->msg_iov, n * sizeof(vibeos_uiov_t))) != 0) {
        return r;
    }
    for (i = 0; i < n; i++) {
        if (iov[i].len > 0x7FFFFFFFFFFFFFFFull - total) {
            return -VIBEOS_EINVAL;
        }
        total += iov[i].len;
        if (iov[i].len != 0u && !linux_user_ok(iov[i].base, iov[i].len, write)) {
            return -VIBEOS_EFAULT;
        }
    }
    *out = iov;
    return 0;
}

/* SCM_RIGHTS out of a msghdr's control buffer: a reference to each description
 * named, taken now; anything else Linux accepts there and this kernel does not
 * carry (SCM_CREDENTIALS) is read past. */
static long linux_msg_rights_in(const linux_msghdr_t *mh, vibeos_msg_t *m) {
    uint8_t buf[256];
    uint64_t off = 0, len = mh->msg_controllen;
    long r;

    if (len == 0u || mh->msg_control == 0u) {
        return 0;
    }
    if (len > sizeof(buf)) {
        return -VIBEOS_ENOBUFS;
    }
    if ((r = linux_net_in(buf, mh->msg_control, len)) != 0) {
        return r;
    }
    while (off + sizeof(linux_cmsghdr_t) <= len) {
        const linux_cmsghdr_t *c = (const linux_cmsghdr_t *)(const void *)(buf + off);
        uint64_t k, n;

        if (c->cmsg_len < sizeof(linux_cmsghdr_t) || c->cmsg_len > len - off) {
            return -VIBEOS_EINVAL;
        }
        if (c->cmsg_level == (int32_t)LINUX_SOL_SOCKET && c->cmsg_type == LINUX_SCM_RIGHTS) {
            n = (c->cmsg_len - sizeof(linux_cmsghdr_t)) / sizeof(int32_t);
            for (k = 0; k < n; k++) {
                int32_t fd;
                vibeos_file_t *g;

                fd = *(const int32_t *)(const void *)(buf + off + sizeof(linux_cmsghdr_t) + k * sizeof(int32_t));
                if (m->nrights >= VIBEOS_MSG_RIGHTS_MAX) {
                    return -VIBEOS_EINVAL;   /* Linux's SCM_MAX_FD is 253; this kernel's is 16 */
                }
                if (!(g = linux_file_get((uint64_t)(uint32_t)fd))) {
                    return -VIBEOS_EBADF;
                }
                m->rights[m->nrights++] = g;
            }
        } else if (c->cmsg_level != (int32_t)LINUX_SOL_SOCKET || c->cmsg_type != LINUX_SCM_CREDENTIALS) {
            return -VIBEOS_EINVAL;
        }
        off += (c->cmsg_len + 7u) & ~(uint64_t)7u;
    }
    return 0;
}

static void linux_msg_rights_put(vibeos_msg_t *m) {
    uint32_t i;

    for (i = 0; i < m->nrights; i++) {
        vibeos_file_put(m->rights[i]);
    }
    m->nrights = 0;
}

/* Received rights into the caller's table and its control buffer, as many as
 * fit; the rest are given back and MSG_CTRUNC said. *used is the control
 * buffer's length used. */
static long linux_msg_rights_out(const linux_msghdr_t *mh, vibeos_msg_t *m, uint32_t cloexec, uint64_t *used) {
    uint8_t buf[sizeof(linux_cmsghdr_t) + VIBEOS_MSG_RIGHTS_MAX * sizeof(int32_t)];
    linux_cmsghdr_t *c = (linux_cmsghdr_t *)(void *)buf;
    uint64_t room = mh->msg_control ? mh->msg_controllen : 0u, fit;
    uint32_t i, n = 0;
    long r = 0;

    *used = 0;
    if (m->nrights == 0u) {
        return 0;
    }
    fit = room >= sizeof(linux_cmsghdr_t) ? (room - sizeof(linux_cmsghdr_t)) / sizeof(int32_t) : 0u;
    for (i = 0; i < m->nrights; i++) {
        long fd;

        if (i >= fit) {
            vibeos_file_put(m->rights[i]);
            m->flags |= VIBEOS_MSG_CTRUNC;
            continue;
        }
        fd = linux_fd_install(m->rights[i], cloexec, 0);   /* takes the reference over */
        if (fd < 0) {
            m->flags |= VIBEOS_MSG_CTRUNC;
            continue;
        }
        *(int32_t *)(void *)(buf + sizeof(linux_cmsghdr_t) + n * sizeof(int32_t)) = (int32_t)fd;
        n++;
    }
    m->nrights = 0;
    if (n == 0u) {
        return 0;
    }
    c->cmsg_len = sizeof(linux_cmsghdr_t) + n * sizeof(int32_t);
    c->cmsg_level = (int32_t)LINUX_SOL_SOCKET;
    c->cmsg_type = LINUX_SCM_RIGHTS;
    r = linux_net_out(mh->msg_control, buf, c->cmsg_len);
    *used = (c->cmsg_len + 7u) & ~(uint64_t)7u;
    if (*used > room) {
        *used = room;
    }
    return r;
}

static long linux_sendmsg_one(vibeos_file_t *f, linux_msghdr_t *mh, uint64_t flags) {
    vibeos_uiov_t small[LINUX_MSG_IOV_SMALL], *iov = small;
    vibeos_sockaddr_t a;
    vibeos_msg_t m;
    uint32_t fam = 0;
    void *pg = 0;
    long r;

    if (flags & LINUX_MSG_OOB) {
        return -VIBEOS_EOPNOTSUPP;
    }
    linux_msg_init(&m, 0, 0, 0, linux_msg_flags_in(flags));
    if ((r = linux_msg_iov(mh, small, &iov, &pg, 0)) != 0) {
        goto out;
    }
    if (mh->msg_name != 0u && f->sk_type == VIBEOS_SOCK_DGRAM) {
        if ((r = linux_sa_in(mh->msg_name, mh->msg_namelen, &a, &fam)) != 0 ||
            (r = linux_unix_target(&a)) != 0) {
            goto out;
        }
        m.addr = &a;
    }
    m.iov = iov;
    m.iovcnt = (uint32_t)mh->msg_iovlen;
    if ((r = linux_msg_rights_in(mh, &m)) != 0) {
        goto out;
    }
    r = f->ops->sockops->sendmsg(f, &m);
out:
    linux_msg_rights_put(&m);   /* what the type did not take over */
    if (pg) {
        ks_page_free(pg, "sendmsg iovecs");
    }
    return r;
}

static long linux_recvmsg_one(vibeos_file_t *f, uint64_t mh_uptr, linux_msghdr_t *mh, uint64_t flags) {
    vibeos_uiov_t small[LINUX_MSG_IOV_SMALL], *iov = small;
    vibeos_sockaddr_t a;
    vibeos_msg_t m;
    uint64_t used = 0;
    void *pg = 0;
    long r;

    if (flags & LINUX_MSG_OOB) {
        return -VIBEOS_EINVAL;
    }
    if (flags & LINUX_MSG_ERRQUEUE) {
        return -VIBEOS_EAGAIN;   /* no error queue ever has anything (recvmsg01) */
    }
    if ((r = linux_msg_iov(mh, small, &iov, &pg, 1)) != 0) {
        goto out;
    }
    a.family = VIBEOS_SA_NONE;
    a.path_len = 0;
    linux_msg_init(&m, &a, iov, (uint32_t)mh->msg_iovlen, linux_msg_flags_in(flags));
    r = f->ops->sockops->recvmsg(f, &m);
    if (r < 0) {
        linux_msg_rights_put(&m);
        goto out;
    }
    if (linux_msg_rights_out(mh, &m, (flags & LINUX_MSG_CMSG_CLOEXEC) ? VIBEOS_FD_CLOEXEC : 0u, &used) != 0) {
        r = -VIBEOS_EFAULT;
        goto out;
    }
    /* The name, the control length used and the flags go back into the
     * caller's msghdr, which Linux writes as a value-result. */
    if (mh->msg_name != 0u && a.family != VIBEOS_SA_NONE) {
        uint64_t lenp = mh_uptr + offsetof(linux_msghdr_t, msg_namelen);

        if (linux_sa_out(mh->msg_name, lenp, &a) != 0) {
            r = -VIBEOS_EFAULT;
            goto out;
        }
        if (linux_net_in(&mh->msg_namelen, lenp, sizeof(mh->msg_namelen)) != 0) {
            r = -VIBEOS_EFAULT;
            goto out;
        }
    } else {
        mh->msg_namelen = 0;
    }
    mh->msg_controllen = used;
    mh->msg_flags = (int32_t)linux_msg_flags_out(m.flags);
    if (linux_net_out(mh_uptr, mh, sizeof(*mh)) != 0) {
        r = -VIBEOS_EFAULT;
    }
out:
    if (pg) {
        ks_page_free(pg, "recvmsg iovecs");
    }
    return r;
}

static long linux_sys_sendmsg(uint64_t fd, uint64_t mh_uptr, uint64_t flags) {
    linux_msghdr_t mh;
    long r;
    vibeos_file_t *f = linux_socket_get(fd, &r);

    if (!f) {
        return r;
    }
    r = linux_net_in(&mh, mh_uptr, sizeof(mh));
    if (r == 0) {
        r = linux_sendmsg_one(f, &mh, flags);
    }
    vibeos_file_put(f);
    return r;
}

static long linux_sys_recvmsg(uint64_t fd, uint64_t mh_uptr, uint64_t flags) {
    linux_msghdr_t mh;
    long r;
    vibeos_file_t *f = linux_socket_get(fd, &r);

    if (!f) {
        return r;
    }
    r = linux_net_in(&mh, mh_uptr, sizeof(mh));
    if (r == 0) {
        r = linux_recvmsg_one(f, mh_uptr, &mh, flags);
    }
    vibeos_file_put(f);
    return r;
}

/* sendmmsg and recvmmsg: the one-message call over a vector, each message's
 * length written beside it. An error on the first message is the call's; one
 * after that ends it with the count so far, as Linux does. recvmmsg's timeout is
 * looked at between messages, and MSG_WAITFORONE stops waiting after the first. */
static long linux_sys_mmsg(uint64_t fd, uint64_t vec, uint64_t vlen, uint64_t flags, uint64_t ts_uptr, int recv) {
    uint64_t n = vlen > LINUX_UIO_MAXIOV ? LINUX_UIO_MAXIOV : vlen, i;
    uint64_t deadline = 0;
    long r = 0;
    vibeos_file_t *f = linux_socket_get(fd, &r);

    if (!f) {
        return r;
    }
    if (recv && ts_uptr != 0u) {
        linux_timespec_t ts;
        int64_t t;

        if ((r = linux_net_in(&ts, ts_uptr, sizeof(ts))) != 0 || (t = linux_ticks_of(&ts)) < 0) {
            vibeos_file_put(f);
            return r ? r : -VIBEOS_EINVAL;
        }
        deadline = ks_ticks() + (uint64_t)t;
    }
    for (i = 0; i < n; i++) {
        linux_mmsghdr_t mm;
        uint64_t at = vec + i * sizeof(mm);
        uint64_t fl = flags;

        if ((r = linux_net_in(&mm, at, sizeof(mm))) != 0) {
            break;
        }
        if (recv && i != 0u && (flags & LINUX_MSG_WAITFORONE)) {
            fl |= LINUX_MSG_DONTWAIT;
        }
        r = recv ? linux_recvmsg_one(f, at, &mm.msg_hdr, fl & ~(uint64_t)LINUX_MSG_WAITFORONE)
                 : linux_sendmsg_one(f, &mm.msg_hdr, fl);
        if (r < 0) {
            break;
        }
        mm.msg_len = (uint32_t)r;
        if (linux_net_out(at + offsetof(linux_mmsghdr_t, msg_len), &mm.msg_len, sizeof(mm.msg_len)) != 0) {
            r = -VIBEOS_EFAULT;
            break;
        }
        if (deadline != 0u && ks_ticks() >= deadline) {
            i++;
            break;
        }
    }
    vibeos_file_put(f);
    if (i != 0u) {
        return (long)i;
    }
    return r;
}

/* ---- options --------------------------------------------------------------------- */

/* Linux's (level, name) as the kernel's option, and how its value is shaped:
 * 0 an int, 1 a struct timeval, 2 a struct linger, 3 a struct ucred. -1 for an
 * option this kernel does not have. */
static int linux_opt_of(int level, int name, int *shape) {
    *shape = 0;
    if (level == (int)LINUX_SOL_SOCKET) {
        switch (name) {
            case LINUX_SO_DEBUG:        return VIBEOS_SO_DEBUG;
            case (int)LINUX_SO_REUSEADDR: return VIBEOS_SO_REUSEADDR;
            case LINUX_SO_REUSEPORT:    return VIBEOS_SO_REUSEPORT;
            case LINUX_SO_TYPE:         return VIBEOS_SO_TYPE;
            case LINUX_SO_ERROR:        return VIBEOS_SO_ERROR;
            case LINUX_SO_DONTROUTE:    return VIBEOS_SO_DONTROUTE;
            case LINUX_SO_BROADCAST:    return VIBEOS_SO_BROADCAST;
            case LINUX_SO_SNDBUF:       return VIBEOS_SO_SNDBUF;
            case LINUX_SO_RCVBUF:       return VIBEOS_SO_RCVBUF;
            case (int)LINUX_SO_KEEPALIVE: return VIBEOS_SO_KEEPALIVE;
            case LINUX_SO_OOBINLINE:    return VIBEOS_SO_OOBINLINE;
            case LINUX_SO_PASSCRED:     return VIBEOS_SO_PASSCRED;
            case LINUX_SO_ACCEPTCONN:   return VIBEOS_SO_ACCEPTCONN;
            case LINUX_SO_DOMAIN:       return VIBEOS_SO_DOMAIN;
            case LINUX_SO_PROTOCOL:     return VIBEOS_SO_TYPE;   /* derived from the type, below */
            case LINUX_SO_LINGER:       *shape = 2; return VIBEOS_SO_LINGER;
            case LINUX_SO_PEERCRED:     *shape = 3; return VIBEOS_SO_PEERCRED;
            case LINUX_SO_RCVTIMEO_OLD: *shape = 1; return VIBEOS_SO_RCVTIMEO;
            case LINUX_SO_SNDTIMEO_OLD: *shape = 1; return VIBEOS_SO_SNDTIMEO;
            default:                    return -1;
        }
    }
    if (level == LINUX_IPPROTO_TCP && name == LINUX_TCP_NODELAY) {
        return VIBEOS_TCP_NODELAY;
    }
    return -1;
}

/* An option this kernel does not have, refused as Linux refuses it - which
 * depends on who would have been asked (LTP's getsockopt01). A name unknown at
 * a level the socket has is ENOPROTOOPT. A level it has not: IP's getsockopt
 * says EOPNOTSUPP and its setsockopt ENOPROTOOPT; a local socket has no levels
 * but SOL_SOCKET and says EOPNOTSUPP both ways. */
static long linux_opt_refused(const vibeos_file_t *f, int level, int get) {
    if (level == (int)LINUX_SOL_SOCKET) {
        return -VIBEOS_ENOPROTOOPT;
    }
    if (f->sk_family == VIBEOS_SA_UNIX) {
        return -VIBEOS_EOPNOTSUPP;
    }
    if (level == (int)LINUX_IPPROTO_IP ||
        level == (f->sk_type == VIBEOS_SOCK_STREAM ? (int)LINUX_IPPROTO_TCP : (int)LINUX_IPPROTO_UDP)) {
        return -VIBEOS_ENOPROTOOPT;
    }
    return get ? -VIBEOS_EOPNOTSUPP : -VIBEOS_ENOPROTOOPT;
}

static long linux_sys_setsockopt(uint64_t fd, uint64_t level, uint64_t name, uint64_t val_uptr, uint64_t len64) {
    int32_t len = VIBEOS_ARG_INT(len64);
    int shape, opt;
    long r;
    vibeos_file_t *f = linux_socket_get(fd, &r);

    if (!f) {
        return r;
    }
    /* The FORCE sizes are the privileged way past the system maximum (LTP's
     * setsockopt04); this kernel's buffers have no maximum to be past, so for
     * the superuser they are the plain ones, and nobody else may set them. */
    if (VIBEOS_ARG_INT(level) == (int)LINUX_SOL_SOCKET &&
        (VIBEOS_ARG_INT(name) == LINUX_SO_SNDBUFFORCE || VIBEOS_ARG_INT(name) == LINUX_SO_RCVBUFFORCE)) {
        vibeos_cred_t c;

        linux_cred(&c);
        if (c.euid != 0u) {
            vibeos_file_put(f);
            return -VIBEOS_EPERM;
        }
        name = VIBEOS_ARG_INT(name) == LINUX_SO_SNDBUFFORCE ? LINUX_SO_SNDBUF : LINUX_SO_RCVBUF;
    }
    opt = linux_opt_of(VIBEOS_ARG_INT(level), VIBEOS_ARG_INT(name), &shape);
    if (len < 0) {
        r = -VIBEOS_EINVAL;
    } else if (opt < 0 || (opt == VIBEOS_TCP_NODELAY && !(f->sk_family == VIBEOS_SA_INET &&
                                                          f->sk_type == VIBEOS_SOCK_STREAM))) {
        r = linux_opt_refused(f, VIBEOS_ARG_INT(level), 0);
    } else if (shape == 1) {
        linux_timeval_t tv;
        uint64_t ticks;

        if ((uint32_t)len < sizeof(tv)) {
            r = -VIBEOS_EINVAL;
        } else if ((r = linux_net_in(&tv, val_uptr, sizeof(tv))) == 0) {
            if (tv.tv_usec < 0 || tv.tv_usec >= 1000000) {
                r = -VIBEOS_EDOM;
            } else {
                /* 0 is for ever; a time that has passed waits not at all. */
                ticks = tv.tv_sec < 0 ? 1u
                        : (((uint64_t)tv.tv_sec * 1000000u + (uint64_t)tv.tv_usec) * ks_hz() + 999999u) / 1000000u;
                r = f->ops->sockops->setopt(f, opt, &ticks, sizeof(ticks));
            }
        }
    } else if (shape == 2) {
        linux_linger_t l;

        if ((uint32_t)len < sizeof(l)) {
            r = -VIBEOS_EINVAL;
        } else if ((r = linux_net_in(&l, val_uptr, sizeof(l))) == 0) {
            r = f->ops->sockops->setopt(f, opt, &l, sizeof(l));
        }
    } else {
        int32_t v;

        if ((uint32_t)len < sizeof(v)) {
            r = -VIBEOS_EINVAL;
        } else if ((r = linux_net_in(&v, val_uptr, sizeof(v))) == 0) {
            r = f->ops->sockops->setopt(f, opt, &v, sizeof(v));
        }
    }
    vibeos_file_put(f);
    return r;
}

static long linux_sys_getsockopt(uint64_t fd, uint64_t level, uint64_t name, uint64_t val_uptr, uint64_t lenp) {
    union {
        int32_t i;
        uint64_t ticks;
        linux_linger_t l;
        linux_ucred_t c;
        linux_timeval_t tv;
        uint32_t u3[3];
    } v;
    uint32_t got = sizeof(v);
    int32_t len;
    int shape, opt;
    long r;
    vibeos_file_t *f = linux_socket_get(fd, &r);

    if (!f) {
        return r;
    }
    if ((r = linux_net_in(&len, lenp, sizeof(len))) != 0) {
        goto out;
    }
    if (len < 0) {
        r = -VIBEOS_EINVAL;
        goto out;
    }
    opt = linux_opt_of(VIBEOS_ARG_INT(level), VIBEOS_ARG_INT(name), &shape);
    if (opt < 0 || (opt == VIBEOS_TCP_NODELAY && !(f->sk_family == VIBEOS_SA_INET && f->sk_type == VIBEOS_SOCK_STREAM))) {
        r = linux_opt_refused(f, VIBEOS_ARG_INT(level), 1);
        goto out;
    }
    if ((r = f->ops->sockops->getopt(f, opt, &v, &got)) != 0) {
        goto out;
    }
    if (shape == 1) {
        uint64_t t = v.ticks;

        v.tv.tv_sec = (int64_t)(t / ks_hz());
        v.tv.tv_usec = (int64_t)((t % ks_hz()) * (1000000u / ks_hz()));
        got = sizeof(v.tv);
    } else if (shape == 3) {
        linux_ucred_t c;

        c.pid = (int32_t)v.u3[0];
        c.uid = v.u3[1];
        c.gid = v.u3[2];
        v.c = c;
        got = sizeof(v.c);
    } else if (VIBEOS_ARG_INT(level) == (int)LINUX_SOL_SOCKET && VIBEOS_ARG_INT(name) == LINUX_SO_DOMAIN) {
        v.i = v.i == (int32_t)VIBEOS_SA_UNIX ? (int32_t)LINUX_AF_UNIX : (int32_t)LINUX_AF_INET;
    } else if (VIBEOS_ARG_INT(level) == (int)LINUX_SOL_SOCKET && VIBEOS_ARG_INT(name) == LINUX_SO_PROTOCOL) {
        v.i = f->sk_family == VIBEOS_SA_UNIX ? 0
              : (v.i == VIBEOS_SOCK_STREAM ? LINUX_IPPROTO_TCP : LINUX_IPPROTO_UDP);
    }
    /* As much of the value as the caller has room for, and how much that was. */
    if ((uint32_t)len > got) {
        len = (int32_t)got;
    }
    if ((r = linux_net_out(val_uptr, &v, (uint64_t)len)) == 0) {
        r = linux_net_out(lenp, &len, sizeof(len));
    }
out:
    vibeos_file_put(f);
    return r;
}

/* What FIONREAD says of a socket (fs.c): the bytes a read would return now. */
long linux_socket_nread(vibeos_file_t *f) {
    int32_t v = 0;
    uint32_t len = sizeof(v);

    if (!f->ops->sockops || f->ops->sockops->getopt(f, VIBEOS_SO_NREAD, &v, &len) != 0) {
        return -VIBEOS_ENOTTY;
    }
    return v;
}

/* readv and writev on a socket (fs.c): one receive or send over the vector, as
 * Linux makes them - element by element would wait in the second for what the
 * first was not given. */
long linux_socket_vec(vibeos_file_t *f, const vibeos_uiov_t *iov, uint32_t n, int write) {
    vibeos_msg_t m;
    long r;

    linux_msg_init(&m, 0, iov, n, 0);
    r = write ? f->ops->sockops->sendmsg(f, &m) : f->ops->sockops->recvmsg(f, &m);
    linux_msg_rights_put(&m);
    return r;
}

/* netctl: the small control surface a shell needs to inspect and exercise the
 * interface. Linux would spread this across ioctl and netlink; VibeOS keeps one
 * explicit call rather than pretending to implement either.
 *
 *   op 0  write {ip, netmask, gateway, dns, up} as five u32 to `arg`
 *   op 1  ping `arg` (an IPv4 address), returns the round trip in ms
 *   op 2  resolve the name at `arg`, returns the address
 *   op 3  write {tx_frames, rx_frames, rx_dropped, tcp_retransmits} as four u64
 */
static long linux_sys_netctl(uint64_t op, uint64_t arg) {
    uint64_t deadline;

    if (!ks_net()) {
        return -VIBEOS_EIO;
    }
    switch (op) {
        case 0: {
            /* Gathered under the lock, written after it (M-050): a user store
             * that faults under the network lock is a panic with the network lock
             * held, and the row's range check before the handler does not
             * survive a sibling's munmap. */
            uint32_t out[5];
            ks_lock(ks_net_lock(), __func__);
            out[0] = ks_net()->ip;
            out[1] = ks_net()->netmask;
            out[2] = ks_net()->gateway;
            out[3] = ks_net()->dns;
            out[4] = (uint32_t)vibeos_inet_dhcp_bound(ks_net());
            ks_unlock(ks_net_lock());
            return vibeos_uaccess_copy((void *)(uintptr_t)arg, out, sizeof(out)) == 0
                       ? 0 : -VIBEOS_EFAULT;
        }
        case 1: {
            ks_lock(ks_net_lock(), __func__);
            (void)vibeos_inet_ping(ks_net(), (uint32_t)arg);
            ks_unlock(ks_net_lock());
            deadline = ks_ticks() + (ks_hz() * 4u);
            for (;;) {
                uint64_t rtt = 0;
                int r;
                ks_lock(ks_net_lock(), __func__);
                r = vibeos_inet_ping_result(ks_net(), &rtt);
                ks_unlock(ks_net_lock());
                if (r == 0) {
                    return (long)rtt;
                }
                if (ks_ticks() > deadline) {
                    return -VIBEOS_EIO;
                }
                ks_idle();
            }
        }
        case 2: {
            char name[64];
            if (ks_copy_user_string(arg, name, sizeof(name)) != 0) {
                return -VIBEOS_EFAULT;
            }
            ks_lock(ks_net_lock(), __func__);
            (void)vibeos_inet_resolve(ks_net(), name);
            ks_unlock(ks_net_lock());
            deadline = ks_ticks() + (ks_hz() * 5u);
            for (;;) {
                uint32_t ip = 0;
                int r;
                ks_lock(ks_net_lock(), __func__);
                r = vibeos_inet_resolve_result(ks_net(), &ip);
                ks_unlock(ks_net_lock());
                if (r == 0) {
                    return (long)ip;
                }
                if (r != -VIBEOS_INET_EAGAIN || ks_ticks() > deadline) {
                    return -VIBEOS_ENOENT;
                }
                ks_idle();
            }
        }
        case 3: {
            uint64_t out[4];   /* as op 0: gathered under the lock, written after */
            ks_lock(ks_net_lock(), __func__);
            out[0] = ks_net()->tx_frames;
            out[1] = ks_net()->rx_frames;
            out[2] = ks_net()->rx_dropped;
            out[3] = ks_net()->tcp_retransmits;
            ks_unlock(ks_net_lock());
            return vibeos_uaccess_copy((void *)(uintptr_t)arg, out, sizeof(out)) == 0
                       ? 0 : -VIBEOS_EFAULT;
        }
        default:
            return -VIBEOS_EINVAL;
    }
}

/* ---- the syscalls this file implements ---------------------------------------
 *
 * The Linux ABI passes the 4th, 5th and 6th arguments in r10, r8 and r9. Rows
 * declare what has a fixed size or a length in another argument; addresses,
 * msghdrs and option values are judged above, by linux_net_in and _out. */
#define LINUX_NET_SYSCALLS(X) \
    X(41,   socket,      SOCKET,      NOPTR, linux_sys_socket(ARG(0), ARG(1), ARG(2))) \
    X(53,   socketpair,  SOCKETPAIR,  PTRS(OUT(3, 8)), linux_sys_socketpair(ARG(0), ARG(1), ARG(2), ARG(3))) \
    X(42,   connect,     CONNECT,     NOPTR, linux_sys_connect(ARG(0), ARG(1), ARG(2))) \
    X(43,   accept,      ACCEPT,      NOPTR, linux_sys_accept4(ARG(0), ARG(1), ARG(2), 0)) \
    X(288,  accept4,     ACCEPT,      NOPTR, linux_sys_accept4(ARG(0), ARG(1), ARG(2), ARG(3))) \
    X(44,   sendto,      SENDTO,      PTRS(IN_BUF(1, 2)), linux_sys_sendto(ARG(0), ARG(1), ARG(2), ARG(3), ARG(4), ARG(5))) \
    X(45,   recvfrom,    RECVFROM,    PTRS(OUT_BUF(1, 2)), linux_sys_recvfrom(ARG(0), ARG(1), ARG(2), ARG(3), ARG(4), ARG(5))) \
    X(46,   sendmsg,     SENDMSG,     NOPTR, linux_sys_sendmsg(ARG(0), ARG(1), ARG(2))) \
    X(47,   recvmsg,     RECVMSG,     NOPTR, linux_sys_recvmsg(ARG(0), ARG(1), ARG(2))) \
    X(307,  sendmmsg,    SENDMSG,     NOPTR, linux_sys_mmsg(ARG(0), ARG(1), ARG(2), ARG(3), 0, 0)) \
    X(299,  recvmmsg,    RECVMSG,     NOPTR, linux_sys_mmsg(ARG(0), ARG(1), ARG(2), ARG(3), ARG(4), 1)) \
    X(49,   bind,        BIND,        NOPTR, linux_sys_bind(ARG(0), ARG(1), ARG(2))) \
    X(50,   listen,      LISTEN,      NOPTR, linux_sys_listen(ARG(0), ARG(1))) \
    X(51,   getsockname, SOCKNAME,    NOPTR, linux_sys_name(ARG(0), ARG(1), ARG(2), 0)) \
    X(52,   getpeername, SOCKNAME,    NOPTR, linux_sys_name(ARG(0), ARG(1), ARG(2), 1)) \
    X(48,   shutdown,    SHUTDOWN,    NOPTR, linux_sys_shutdown(ARG(0), ARG(1))) \
    X(54,   setsockopt,  SETSOCKOPT,  NOPTR, linux_sys_setsockopt(ARG(0), ARG(1), ARG(2), ARG(3), ARG(4))) \
    X(55,   getsockopt,  SETSOCKOPT,  NOPTR, linux_sys_getsockopt(ARG(0), ARG(1), ARG(2), ARG(3), ARG(4))) \
    X(1000, netctl,      NETCTL,      PTRS(OUT_IF(0, 0, 1, 20), OUT_IF(0, 3, 1, 32)), linux_sys_netctl(ARG(0), ARG(1)))

LINUX_DEFINE_SYSCALLS(net, LINUX_NET_SYSCALLS)
