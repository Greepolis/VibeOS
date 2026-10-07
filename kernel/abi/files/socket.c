/* A socket as a file, and the waits a socket call makes (docs/abi/ A3).
 *
 * The TCP/IP stack is kernel/net/inet.c; this is what a descriptor naming one of
 * its sockets does. Moved from the Linux socket handlers, which kept only the
 * ABI's half - reading a sockaddr, writing one back, installing a descriptor - so
 * another personality's socket calls can wait the same way. Since docs/abi/ L5
 * the calls are a table (vibeos/sockops.h) that the local sockets (unixsock.c)
 * fill in too, and the Linux handlers do not know which family they hold.
 *
 * Every wait below holds a reference to the description, so it cannot be closed
 * and reused under the caller: that half of M-020 (a sibling thread closing the
 * descriptor and opening another socket into the slot while a call slept) is
 * structural now. The other half is not: the socket itself can still go - a
 * process exiting releases the sockets it owns - and its slot in the stack be
 * given to a new socket. So every pass re-checks the socket's tenancy, and
 * check-net-stable.py holds every waiting function here to it. */

#include "files_internal.h"

/* How long connect waits for the handshake when nothing else says: the stack
 * gives up retransmitting a SYN well before this. */
#define SOCKET_CONNECT_SECONDS 30u

/* The largest datagram the stack sends whole: an Ethernet frame, less the
 * Ethernet, IP and UDP headers. It does not fragment. */
#define SOCKET_UDP_MAX 1472u

/* Where socket data waits between the stack and user memory (H-010).
 *
 * The portable stack copies straight into whatever pointer it is given, and it
 * cannot use the fault-tolerant copy - that is assembly in the arch layer. So
 * receives go into this buffer and are then copied out, and sends are copied in
 * first. Both halves happen under the network lock, which serialises every user
 * of it. One receive buffer's worth: a socket never holds more than that. */
static uint8_t g_bounce[VIBEOS_INET_RXBUF];

/* Give up the CPU until the next tick; the network is pumped from there. */
static void socket_wait_tick(void) {
    ks_idle();
}

/* One more look later, or a reason not to wait: a socket that may not block -
 * O_NONBLOCK, or MSG_DONTWAIT for this call - says EAGAIN, as does a wait past
 * the program's SO_RCVTIMEO or SO_SNDTIMEO (`deadline`, 0 for none), and a
 * signal that needs acting on ends the wait with `cut` - the call restarted
 * under SA_RESTART for accept and the reads, EINTR for connect, as Linux has
 * them. Until docs/abi/ L4 step 7 a wait here heard no signal: an httpd parked
 * in accept could not be killed, and the boot that started it never left
 * userland. 0 when it has waited. */
static long socket_wait(const vibeos_file_t *f, uint32_t msg_flags, long cut, uint64_t deadline) {
    if ((f->flags & VIBEOS_O_NONBLOCK) || (msg_flags & VIBEOS_MSG_DONTWAIT)) {
        return -VIBEOS_EAGAIN;
    }
    if (deadline != 0u && ks_ticks() >= deadline) {
        return -VIBEOS_EAGAIN;
    }
    if (ks_current() >= 0 && ks_signal_interrupts(ks_current())) {
        return cut;
    }
    socket_wait_tick();
    return 0;
}

static uint64_t socket_deadline(uint64_t timeo) {
    return timeo != 0u ? ks_ticks() + timeo : 0u;
}

int vibeos_sockfile_stable(const vibeos_file_t *f) {
    vibeos_inet_t *net = ks_net();
    int ok;

    if (!f || f->sock < 0 || !net) {
        return -1;
    }
    ks_lock(ks_net_lock(), __func__);
    ok = net->sockets[f->sock].used && net->sockets[f->sock].gen == f->sock_gen;
    if (!ok) {
        net->sock_fd_aba++;
    }
    ks_unlock(ks_net_lock());
    return ok ? f->sock : -1;
}

/* The stack's refusals as the kernel's errno. */
static long inet_errno(long r) {
    switch (-r) {
        case VIBEOS_INET_EAGAIN:     return -VIBEOS_EAGAIN;
        case VIBEOS_INET_ECONNRESET: return -VIBEOS_ECONNRESET;
        case VIBEOS_INET_ENOTCONN:   return -VIBEOS_ENOTCONN;
        case VIBEOS_INET_ETIMEDOUT:  return -VIBEOS_ETIMEDOUT;
        case VIBEOS_INET_ENOBUFS:    return -VIBEOS_ENOBUFS;
        case VIBEOS_INET_EPIPE:      return -VIBEOS_EPIPE;
        case VIBEOS_INET_EADDRINUSE: return -VIBEOS_EADDRINUSE;
        case VIBEOS_INET_EISCONN:    return -VIBEOS_EISCONN;
        default:                     return -VIBEOS_EINVAL;
    }
}

/* A broken connection: EPIPE, and SIGPIPE unless the call asked for none, as a
 * pipe with no reader raises it (pipefile.c). */
static long socket_epipe(uint32_t msg_flags) {
    if (!(msg_flags & VIBEOS_MSG_NOSIGNAL) && ks_current() >= 0) {
        (void)ks_signal_raise(ks_current(), VIBEOS_SIGPIPE);
    }
    return -VIBEOS_EPIPE;
}

static uint64_t iov_total(const vibeos_msg_t *m) {
    uint64_t t = 0;
    uint32_t i;

    for (i = 0; i < m->iovcnt; i++) {
        t += m->iov[i].len;
    }
    return t;
}

/* Copy `n` bytes of the iovec, from byte `skip` on, into or out of `buf`.
 * 0, or -1 if user memory refused. */
static int iov_copy(const vibeos_msg_t *m, uint64_t skip, uint8_t *buf, uint64_t n, int to_user) {
    uint32_t i;
    uint64_t done = 0;

    for (i = 0; i < m->iovcnt && done < n; i++) {
        uint64_t len = m->iov[i].len, take;

        if (skip >= len) {
            skip -= len;
            continue;
        }
        take = len - skip;
        if (take > n - done) {
            take = n - done;
        }
        if (to_user ? vibeos_uaccess_copy((void *)(uintptr_t)(m->iov[i].base + skip), buf + done, take)
                    : vibeos_uaccess_copy(buf + done, (const void *)(uintptr_t)(m->iov[i].base + skip), take)) {
            return -1;
        }
        done += take;
        skip = 0;
    }
    return 0;
}

/* ---- the calls ------------------------------------------------------------------ */

/* An address this machine has: any, its own, or the loopback block. */
static int inet_local_ok(uint32_t ip) {
    vibeos_inet_t *net = ks_net();

    return ip == 0u || (net && ip == net->ip) || (ip >> 24) == 127u;
}

static long in_bind(vibeos_file_t *f, const vibeos_sockaddr_t *a) {
    int sock;
    long r;

    if (a->family != VIBEOS_SA_INET) {
        return -VIBEOS_EAFNOSUPPORT;
    }
    if (!inet_local_ok(a->ip)) {
        return -VIBEOS_EADDRNOTAVAIL;
    }
    if ((sock = vibeos_sockfile_stable(f)) < 0) {
        return -VIBEOS_EBADF;
    }
    ks_lock(ks_net_lock(), __func__);
    (void)vibeos_inet_set_reuse(ks_net(), sock, (f->sk_flags & VIBEOS_SKF_REUSEADDR) != 0u);
    r = vibeos_inet_bind_addr(ks_net(), sock, a->ip, a->port);
    ks_unlock(ks_net_lock());
    return r == 0 ? 0 : inet_errno(r);
}

static long in_listen(vibeos_file_t *f, int backlog) {
    int sock;
    long r;

    (void)backlog;   /* the stack's backlog is its own (VIBEOS_INET_BACKLOG) */
    if (f->sk_type != VIBEOS_SOCK_STREAM) {
        return -VIBEOS_EOPNOTSUPP;
    }
    if ((sock = vibeos_sockfile_stable(f)) < 0) {
        return -VIBEOS_EBADF;
    }
    ks_lock(ks_net_lock(), __func__);
    (void)vibeos_inet_set_reuse(ks_net(), sock, (f->sk_flags & VIBEOS_SKF_REUSEADDR) != 0u);
    r = vibeos_inet_listen(ks_net(), sock);
    ks_unlock(ks_net_lock());
    return r == 0 ? 0 : inet_errno(r);
}

/* A connection is made by the stack; this waits for it, or - for a socket that
 * may not block - says it is under way, and SO_ERROR and poll tell the rest. */
static long in_connect(vibeos_file_t *f, const vibeos_sockaddr_t *a) {
    uint64_t deadline;
    int r, sock, st;

    if ((sock = vibeos_sockfile_stable(f)) < 0) {
        return -VIBEOS_EBADF;
    }
    if (a->family == VIBEOS_SA_NONE && f->sk_type == VIBEOS_SOCK_DGRAM) {
        ks_lock(ks_net_lock(), __func__);
        ks_net()->sockets[sock].udp_peer = 0;   /* AF_UNSPEC: forget the peer */
        ks_unlock(ks_net_lock());
        return 0;
    }
    if (a->family != VIBEOS_SA_INET) {
        return -VIBEOS_EAFNOSUPPORT;
    }
    ks_lock(ks_net_lock(), __func__);
    st = vibeos_inet_socket_state(ks_net(), sock);
    r = (f->sk_type == VIBEOS_SOCK_STREAM && st == VIBEOS_TCP_SYN_SENT)
            ? -VIBEOS_INET_EAGAIN   /* one under way already */
            : vibeos_inet_connect(ks_net(), sock, a->ip, a->port);
    ks_unlock(ks_net_lock());
    if (r == -VIBEOS_INET_EAGAIN) {
        return -VIBEOS_EALREADY;
    }
    if (r != 0) {
        return inet_errno(r);
    }
    if (f->sk_type == VIBEOS_SOCK_DGRAM) {
        return 0;
    }
    if (f->flags & VIBEOS_O_NONBLOCK) {
        return -VIBEOS_EINPROGRESS;
    }
    deadline = f->sk_sndtimeo != 0u ? ks_ticks() + f->sk_sndtimeo
                                    : ks_ticks() + SOCKET_CONNECT_SECONDS * ks_hz();
    for (;;) {
        if ((sock = vibeos_sockfile_stable(f)) < 0) {
            return -VIBEOS_EBADF;
        }
        ks_lock(ks_net_lock(), __func__);
        st = vibeos_inet_socket_state(ks_net(), sock);
        ks_unlock(ks_net_lock());
        if (st == VIBEOS_TCP_ESTABLISHED || st == VIBEOS_TCP_CLOSE_WAIT) {
            return 0;
        }
        if (st == VIBEOS_TCP_CLOSED || st < 0) {
            return -VIBEOS_ECONNREFUSED;   /* refused, reset, or gave up retransmitting */
        }
        if (ks_ticks() >= deadline) {
            return f->sk_sndtimeo != 0u ? -VIBEOS_EINPROGRESS : -VIBEOS_ETIMEDOUT;
        }
        if (ks_current() >= 0 && ks_signal_interrupts(ks_current())) {
            return -VIBEOS_EINTR;
        }
        socket_wait_tick();
    }
}

static long in_accept(vibeos_file_t *f, uint32_t owner, vibeos_file_t **child_out, vibeos_sockaddr_t *peer) {
    uint64_t deadline = socket_deadline(f->sk_rcvtimeo);
    int child, sock;
    vibeos_file_t *c;

    *child_out = 0;
    if (f->sk_type != VIBEOS_SOCK_STREAM) {
        return -VIBEOS_EOPNOTSUPP;
    }
    for (;;) {
        if ((sock = vibeos_sockfile_stable(f)) < 0) {
            return -VIBEOS_EBADF;
        }
        ks_lock(ks_net_lock(), __func__);
        child = vibeos_inet_accept(ks_net(), sock);
        ks_unlock(ks_net_lock());
        if (child >= 0) {
            break;
        }
        if (child != -VIBEOS_INET_EAGAIN) {
            return -VIBEOS_EINVAL;   /* not listening */
        }
        {
            long w = socket_wait(f, 0, -VIBEOS_RESTART_CALL, deadline);

            if (w != 0) {
                return w;
            }
        }
    }
    ks_lock(ks_net_lock(), __func__);
    peer->family = VIBEOS_SA_INET;
    peer->ip = ks_net()->sockets[child].remote_ip;
    peer->port = ks_net()->sockets[child].remote_port;
    peer->path_len = 0;
    /* The child was made by the stack and is owned by nobody; without an owner,
     * process exit never releases it (M-031). */
    (void)vibeos_inet_socket_set_owner(ks_net(), child, owner);
    ks_unlock(ks_net_lock());
    c = vibeos_open_socket(child);
    if (!c) {
        ks_lock(ks_net_lock(), __func__);
        (void)vibeos_inet_close(ks_net(), child);
        ks_unlock(ks_net_lock());
        return -VIBEOS_ENFILE;
    }
    vibeos_sockopt_init(c, VIBEOS_SOCK_STREAM, VIBEOS_SA_INET);
    *child_out = c;
    return 0;
}

/* A stream sends what fits, waiting only for the first byte to fit; a datagram
 * goes whole or not at all. */
static long in_sendmsg(vibeos_file_t *f, vibeos_msg_t *m) {
    uint64_t total = iov_total(m), sent = 0;
    uint64_t deadline = socket_deadline(f->sk_sndtimeo);
    int sock;
    long n;

    if (m->nrights != 0u) {
        return -VIBEOS_EOPNOTSUPP;   /* rights travel over local sockets only */
    }
    if (f->sk_type == VIBEOS_SOCK_DGRAM) {
        uint32_t ip = 0;
        uint16_t port = 0;

        if (total > SOCKET_UDP_MAX) {
            return -VIBEOS_EMSGSIZE;
        }
        if (m->addr && m->addr->family == VIBEOS_SA_INET) {
            ip = m->addr->ip;
            port = m->addr->port;
            if (ip == 0xFFFFFFFFu && !(f->sk_flags & VIBEOS_SKF_BROADCAST)) {
                return -VIBEOS_EACCES;   /* Linux's: broadcast is asked for */
            }
        } else if (m->addr && m->addr->family != VIBEOS_SA_NONE) {
            return -VIBEOS_EAFNOSUPPORT;
        }
        if ((sock = vibeos_sockfile_stable(f)) < 0) {
            return -VIBEOS_EBADF;
        }
        ks_lock(ks_net_lock(), __func__);
        if ((ip == 0u && port == 0u) && !ks_net()->sockets[sock].udp_peer) {
            ks_unlock(ks_net_lock());
            return -VIBEOS_EDESTADDRREQ;
        }
        if (iov_copy(m, 0, g_bounce, total, 0) != 0) {
            ks_unlock(ks_net_lock());
            return -VIBEOS_EFAULT;
        }
        n = vibeos_inet_sendto(ks_net(), sock, g_bounce, (uint32_t)total, ip, port);
        ks_unlock(ks_net_lock());
        return n < 0 ? inet_errno(n) : n;
    }
    while (sent < total || total == 0u) {
        uint64_t chunk = total - sent;

        if ((sock = vibeos_sockfile_stable(f)) < 0) {
            return sent ? (long)sent : -VIBEOS_EBADF;
        }
        if (chunk > sizeof(g_bounce)) {
            chunk = sizeof(g_bounce);
        }
        ks_lock(ks_net_lock(), __func__);
        {
            int st = vibeos_inet_socket_state(ks_net(), sock);

            if (st != VIBEOS_TCP_ESTABLISHED && st != VIBEOS_TCP_CLOSE_WAIT &&
                !ks_net()->sockets[sock].fin_queued && !ks_net()->sockets[sock].reset) {
                ks_unlock(ks_net_lock());
                /* Never connected, or not yet: Linux's tcp_sendmsg says EPIPE. */
                return sent ? (long)sent : socket_epipe(m->flags);
            }
        }
        if (iov_copy(m, sent, g_bounce, chunk, 0) != 0) {
            ks_unlock(ks_net_lock());
            return sent ? (long)sent : -VIBEOS_EFAULT;
        }
        n = total == 0u ? 0 : vibeos_inet_send(ks_net(), sock, g_bounce, (uint32_t)chunk);
        ks_unlock(ks_net_lock());
        if (n == -VIBEOS_INET_EPIPE || n == -VIBEOS_INET_ECONNRESET) {
            return sent ? (long)sent : (n == -VIBEOS_INET_EPIPE ? socket_epipe(m->flags) : -VIBEOS_ECONNRESET);
        }
        if (n > 0) {
            sent += (uint64_t)n;
            continue;
        }
        if (total == 0u) {
            return 0;
        }
        if (n < 0 && n != -VIBEOS_INET_EAGAIN) {
            return sent ? (long)sent : inet_errno(n);
        }
        if (sent != 0u) {
            return (long)sent;   /* a short send, which a stream allows */
        }
        if ((n = socket_wait(f, m->flags, -VIBEOS_RESTART_CALL, deadline)) != 0) {
            return n;
        }
    }
    return (long)sent;
}

static long in_recvmsg(vibeos_file_t *f, vibeos_msg_t *m) {
    uint64_t total = iov_total(m), got = 0;
    uint64_t deadline = socket_deadline(f->sk_rcvtimeo);
    uint32_t peek = (m->flags & VIBEOS_MSG_PEEK) ? VIBEOS_INET_PEEK : 0u;
    uint32_t asked = m->flags;
    int sock;
    long n;

    m->flags = 0;
    m->nrights = 0;
    for (;;) {
        uint64_t want = total - got;
        int faulted = 0;

        if ((sock = vibeos_sockfile_stable(f)) < 0) {
            return got ? (long)got : -VIBEOS_EBADF;
        }
        if (want > sizeof(g_bounce)) {
            want = sizeof(g_bounce);
        }
        ks_lock(ks_net_lock(), __func__);
        if (f->sk_type == VIBEOS_SOCK_DGRAM) {
            uint32_t ip = 0, full = 0;
            uint16_t port = 0;

            n = vibeos_inet_recvfrom_ex(ks_net(), sock, g_bounce, (uint32_t)want, peek, &ip, &port, &full);
            if (n >= 0) {
                if (iov_copy(m, 0, g_bounce, (uint64_t)n, 1) != 0) {
                    faulted = 1;
                }
                if (m->addr) {
                    m->addr->family = VIBEOS_SA_INET;
                    m->addr->ip = ip;
                    m->addr->port = port;
                    m->addr->path_len = 0;
                }
                m->full_len = full;
                if (full > (uint32_t)n) {
                    m->flags |= VIBEOS_MSG_TRUNC;
                }
            }
        } else {
            int st = vibeos_inet_socket_state(ks_net(), sock);

            if (st == VIBEOS_TCP_LISTEN || (st == VIBEOS_TCP_CLOSED && !ks_net()->sockets[sock].reset &&
                                            !ks_net()->sockets[sock].fin_received)) {
                ks_unlock(ks_net_lock());
                return got ? (long)got : -VIBEOS_ENOTCONN;
            }
            n = vibeos_inet_recv_ex(ks_net(), sock, g_bounce, (uint32_t)want, peek);
            if (n > 0 && iov_copy(m, got, g_bounce, (uint64_t)n, 1) != 0) {
                faulted = 1;
            }
        }
        ks_unlock(ks_net_lock());
        if (faulted) {
            return got ? (long)got : -VIBEOS_EFAULT;
        }
        if (n >= 0) {
            if (f->sk_type == VIBEOS_SOCK_DGRAM) {
                return (asked & VIBEOS_MSG_TRUNC) ? (long)m->full_len : n;
            }
            got += (uint64_t)n;
            /* MSG_WAITALL waits for the whole length, or the end of the stream. */
            if (n == 0 || !(asked & VIBEOS_MSG_WAITALL) || peek || got >= total) {
                return (long)got;
            }
            continue;
        }
        if (n == -VIBEOS_INET_ECONNRESET) {
            return got ? (long)got : -VIBEOS_ECONNRESET;
        }
        if (n != -VIBEOS_INET_EAGAIN) {
            return got ? (long)got : -VIBEOS_EINVAL;
        }
        if (total == 0u) {
            return 0;
        }
        if ((n = socket_wait(f, asked, -VIBEOS_RESTART_CALL, deadline)) != 0) {
            return got ? (long)got : n;
        }
    }
}

static long in_shutdown(vibeos_file_t *f, int how) {
    int r, sock, bits = 0;

    bits |= (how & VIBEOS_SHUT_RD) ? VIBEOS_INET_SHUT_RD : 0;
    bits |= (how & VIBEOS_SHUT_WR) ? VIBEOS_INET_SHUT_WR : 0;
    if ((sock = vibeos_sockfile_stable(f)) < 0) {
        return -VIBEOS_EBADF;
    }
    ks_lock(ks_net_lock(), __func__);
    r = vibeos_inet_shutdown(ks_net(), sock, bits);
    ks_unlock(ks_net_lock());
    return r == 0 ? 0 : inet_errno(r);
}

static long in_name(vibeos_file_t *f, int peer, vibeos_sockaddr_t *out) {
    uint32_t lip = 0, rip = 0;
    uint16_t lport = 0, rport = 0;
    int sock, has;

    if ((sock = vibeos_sockfile_stable(f)) < 0) {
        return -VIBEOS_EBADF;
    }
    ks_lock(ks_net_lock(), __func__);
    has = vibeos_inet_names(ks_net(), sock, &lip, &lport, &rip, &rport);
    ks_unlock(ks_net_lock());
    if (has < 0) {
        return -VIBEOS_EBADF;
    }
    if (peer && !has) {
        return -VIBEOS_ENOTCONN;
    }
    out->family = VIBEOS_SA_INET;
    out->ip = peer ? rip : lip;
    out->port = peer ? rport : lport;
    out->path_len = 0;
    return 0;
}

static long in_setopt(vibeos_file_t *f, int opt, const void *val, uint32_t len) {
    long r = vibeos_sockopt_set(f, opt, val, len);
    int sock;

    if (r == 0 && opt == VIBEOS_SO_REUSEADDR && (sock = vibeos_sockfile_stable(f)) >= 0) {
        ks_lock(ks_net_lock(), __func__);
        (void)vibeos_inet_set_reuse(ks_net(), sock, (f->sk_flags & VIBEOS_SKF_REUSEADDR) != 0u);
        ks_unlock(ks_net_lock());
    }
    return r;
}

static long in_getopt(vibeos_file_t *f, int opt, void *val, uint32_t *len) {
    int sock;
    int32_t v;

    if (opt != VIBEOS_SO_ACCEPTCONN && opt != VIBEOS_SO_NREAD && opt != VIBEOS_SO_ERROR) {
        return vibeos_sockopt_get(f, opt, val, len);
    }
    if (*len < sizeof(v)) {
        return -VIBEOS_EINVAL;
    }
    if ((sock = vibeos_sockfile_stable(f)) < 0) {
        return -VIBEOS_EBADF;
    }
    ks_lock(ks_net_lock(), __func__);
    {
        const vibeos_inet_socket_t *s = &ks_net()->sockets[sock];

        if (opt == VIBEOS_SO_ACCEPTCONN) {
            v = s->type == VIBEOS_INET_SOCK_TCP && s->state == VIBEOS_TCP_LISTEN;
        } else if (opt == VIBEOS_SO_NREAD) {
            v = s->type == VIBEOS_INET_SOCK_UDP ? (s->rx_len >= 8u ? (int32_t)(((uint32_t)s->rx[0] << 8) | s->rx[1]) : 0)
                                                : (int32_t)s->rx_len;
        } else {
            /* A connection that failed is the error a non-blocking connect is
             * finished by reading, once. */
            v = f->sk_err;
            if (v == 0 && s->type == VIBEOS_INET_SOCK_TCP && s->reset) {
                v = s->state == VIBEOS_TCP_CLOSED ? VIBEOS_ECONNREFUSED : VIBEOS_ECONNRESET;
            }
            f->sk_err = 0;
        }
    }
    ks_unlock(ks_net_lock());
    *(int32_t *)val = v;
    *len = sizeof(v);
    return 0;
}

static const vibeos_sock_ops_t g_inet_sockops = {
    .bind = in_bind,
    .listen = in_listen,
    .connect = in_connect,
    .accept = in_accept,
    .sendmsg = in_sendmsg,
    .recvmsg = in_recvmsg,
    .shutdown = in_shutdown,
    .name = in_name,
    .setopt = in_setopt,
    .getopt = in_getopt,
};

/* ---- the file type ------------------------------------------------------------------ */

static long socket_recv(vibeos_file_t *f, uint64_t buf, uint64_t len) {
    vibeos_uiov_t iov;
    vibeos_msg_t m;

    iov.base = buf;
    iov.len = len;
    m.addr = 0;
    m.iov = &iov;
    m.iovcnt = 1;
    m.flags = 0;
    m.nrights = 0;
    m.max_rights = 0;
    m.full_len = 0;
    return in_recvmsg(f, &m);
}

static long socket_send(vibeos_file_t *f, uint64_t buf, uint64_t len) {
    vibeos_uiov_t iov;
    vibeos_msg_t m;

    iov.base = buf;
    iov.len = len;
    m.addr = 0;
    m.iov = &iov;
    m.iovcnt = 1;
    m.flags = 0;
    m.nrights = 0;
    m.max_rights = 0;
    m.full_len = 0;
    return in_sendmsg(f, &m);
}

static int socket_stat(vibeos_file_t *f, vibeos_file_stat_t *out) {
    out->mode = VIBEOS_S_IFSOCK | 0777u;   /* what Linux's sockfs says */
    out->size = 0;
    out->ino = 0x20000u + (uint64_t)(uint32_t)f->sock;
    return 0;
}

/* What poll may say of it (docs/abi/ L4): the stack's answer, by its own rules.
 * A socket used to have no `ready`, which reads as "always ready for both", so
 * poll returned at once on a socket with nothing to read and nc - which waits in
 * poll on its socket - spun. A socket that is no longer this description's
 * (closed, its slot given away) is hung up. */
static uint32_t socket_ready(vibeos_file_t *f) {
    uint32_t in, r = 0;
    int sock;

    if ((sock = vibeos_sockfile_stable(f)) < 0) {
        return VIBEOS_READY_HUP;
    }
    ks_lock(ks_net_lock(), __func__);
    in = vibeos_inet_ready(ks_net(), sock);
    ks_unlock(ks_net_lock());
    r |= (in & VIBEOS_INET_READY_IN) ? VIBEOS_READY_IN : 0u;
    r |= (in & VIBEOS_INET_READY_OUT) ? VIBEOS_READY_OUT : 0u;
    r |= (in & VIBEOS_INET_READY_HUP) ? VIBEOS_READY_HUP : 0u;
    r |= (in & VIBEOS_INET_READY_ERR) ? VIBEOS_READY_ERR : 0u;
    r |= (in & VIBEOS_INET_READY_RDHUP) ? VIBEOS_READY_RDHUP : 0u;
    return r;
}

/* The last descriptor has gone. Closed only if it is still the socket this
 * description was opened on: a process's exit releases the sockets it owns, and
 * a child that inherited the descriptor can outlive its parent - closing by
 * index then would close whoever holds the slot now. */
static void socket_release(vibeos_file_t *f) {
    if (vibeos_sockfile_stable(f) >= 0) {
        ks_lock(ks_net_lock(), __func__);
        (void)vibeos_inet_close(ks_net(), f->sock);
        ks_unlock(ks_net_lock());
    }
    f->sock = -1;
}

const vibeos_file_ops_t vibeos_fops_socket = {
    .name = "socket",
    .read = socket_recv,
    .write = socket_send,
    .stat = socket_stat,
    .release = socket_release,
    .ready = socket_ready,
    .sockops = &g_inet_sockops,
};

vibeos_file_t *vibeos_open_socket(int sock) {
    vibeos_file_t *f;

    if (sock < 0 || !ks_net()) {
        return 0;
    }
    f = vibeos_file_alloc(&vibeos_fops_socket, VIBEOS_O_RDWR);
    if (!f) {
        return 0;
    }
    ks_lock(ks_net_lock(), __func__);
    f->sock = sock;
    f->sock_gen = ks_net()->sockets[sock].gen;
    f->sk_type = ks_net()->sockets[sock].type == VIBEOS_INET_SOCK_UDP ? VIBEOS_SOCK_DGRAM : VIBEOS_SOCK_STREAM;
    ks_unlock(ks_net_lock());
    vibeos_sockopt_init(f, f->sk_type, VIBEOS_SA_INET);
    return f;
}

vibeos_file_t *vibeos_sockfile_create(int kind, uint32_t owner, long *err) {
    vibeos_file_t *f;
    int s;

    if (!ks_net()) {
        *err = -VIBEOS_EAFNOSUPPORT;
        return 0;
    }
    ks_lock(ks_net_lock(), __func__);
    s = vibeos_inet_socket(ks_net(), kind);
    if (s >= 0 && vibeos_inet_socket_set_owner(ks_net(), s, owner) != 0) {
        (void)vibeos_inet_close(ks_net(), s);
        s = -1;
    }
    ks_unlock(ks_net_lock());
    if (s < 0) {
        *err = -VIBEOS_ENOBUFS;
        return 0;
    }
    f = vibeos_open_socket(s);
    if (!f) {
        ks_lock(ks_net_lock(), __func__);
        (void)vibeos_inet_close(ks_net(), s);
        ks_unlock(ks_net_lock());
        *err = -VIBEOS_ENFILE;
        return 0;
    }
    *err = 0;
    return f;
}
