/* The Linux socket syscalls, lifted out of arch_hw.c.
 *
 * First cut of the extraction: 395 lines, which is four per cent of that file
 * and not the point. The point is the seam. Everything in arch_hw.c was
 * `static` over shared globals, so nothing could move until somebody decided
 * what the rest of the file is allowed to see; that decision now lives in
 * arch_hw_internal.h and this file is the proof it works.
 *
 * These belong here because none of it is architecture. Reading a sockaddr out
 * of user memory, allocating a descriptor, blocking until a connection
 * arrives - that is Linux ABI translation over a portable TCP/IP stack
 * (kernel/net/inet.c), and it sat in the same file as the GDT because that is
 * where the file started.
 *
 * What it reached back for was six globals and six functions, listed in the
 * header - the honest cost of the cut. Since A2 (docs/abi/) that is the stack,
 * its lock, the clock and a wait, all through vibeos/ksvc.h, and this file runs
 * in the host tests like the rest of the personality.
 */

#include "linux_internal.h"

/* Read a struct sockaddr_in out of user memory: family (host order), port and
 * address (both network order on the wire). */
/*
 * Both helpers go through vibeos_uaccess_copy, into and out of a local copy
 * (M-050). They dereferenced the user pointer directly: the dispatcher checks the
 * range before the handler runs, and that check and these accesses are two
 * instants - in accept() and recvfrom() separated by a blocking wait of any
 * length - so a sibling thread's munmap in between made the kernel take the
 * fault in ring 0, outside the one instruction that can recover, and panic.
 * H-010's family again; M-040 closed the same shape in write(). */
static int linux_read_sockaddr(uint64_t uptr, uint32_t *out_ip, uint16_t *out_port) {
    uint8_t p[8];

    if (vibeos_uaccess_copy(p, (const void *)(uintptr_t)uptr, sizeof(p)) != 0) {
        return -1;
    }
    if (((uint16_t)p[0] | ((uint16_t)p[1] << 8)) != 2u) {   /* AF_INET */
        return -1;
    }
    *out_port = (uint16_t)(((uint16_t)p[2] << 8) | p[3]);
    *out_ip = ((uint32_t)p[4] << 24) | ((uint32_t)p[5] << 16) |
              ((uint32_t)p[6] << 8) | (uint32_t)p[7];
    return 0;
}

static int linux_write_sockaddr(uint64_t uptr, uint32_t ip, uint16_t port) {
    uint8_t p[16];
    int k;

    if (uptr == 0u) {
        return 0;
    }
    p[0] = 2; p[1] = 0;
    p[2] = (uint8_t)(port >> 8);
    p[3] = (uint8_t)(port & 0xFFu);
    p[4] = (uint8_t)(ip >> 24);
    p[5] = (uint8_t)((ip >> 16) & 0xFFu);
    p[6] = (uint8_t)((ip >> 8) & 0xFFu);
    p[7] = (uint8_t)(ip & 0xFFu);
    for (k = 8; k < 16; k++) {
        p[k] = 0;
    }
    /* The one fallible step, and the callers' undo paths were written for it:
     * until now it could not fail, so they had never run. */
    return vibeos_uaccess_copy((void *)(uintptr_t)uptr, p, sizeof(p)) == 0 ? 0 : -1;
}

/* Give up the CPU until the next tick; the network is pumped from there. */
static void linux_net_wait_tick(void) {
    ks_idle();
}

static long linux_sys_socket(uint64_t domain, uint64_t type) {
    vibeos_procstate_t *ps;
    int me, fd, s;
    int kind;

    if (!ks_net() || (me = ks_current()) < 0 || !ks_id(me)->is_user) {
        return -VIBEOS_EINVAL;
    }
    if (domain != 2u) {                       /* AF_INET only */
        return -VIBEOS_EINVAL;
    }
    if ((type & 0xFFu) == 1u) {
        kind = VIBEOS_INET_SOCK_TCP;          /* SOCK_STREAM */
    } else if ((type & 0xFFu) == 2u) {
        kind = VIBEOS_INET_SOCK_UDP;          /* SOCK_DGRAM  */
    } else {
        return -VIBEOS_EINVAL;
    }

    fd = linux_fd_alloc(me);
    ps = ks_ps(me);
    if (fd < 0) {
        return -VIBEOS_EMFILE;
    }
    ks_lock(ks_net_lock(), __func__);
    s = vibeos_inet_socket(ks_net(), kind);
    if (s >= 0 && vibeos_inet_socket_set_owner(ks_net(), s, ks_id(me)->tgid) != 0) {
        (void)vibeos_inet_close(ks_net(), s);
        s = -1;
    }
    ks_unlock(ks_net_lock());
    if (s < 0) {
        ps->files.fds[fd].used = 0;
        return -VIBEOS_ENOMEM;
    }
    ps->files.fds[fd].net_sock = s;
    ps->files.fds[fd].pipe = -1;
    return 3 + fd;
}

static long linux_sys_bind(uint64_t fd, uint64_t addr_uptr) {
    vibeos_fd_t *f = linux_fd_get(fd);
    uint32_t ip;
    uint16_t port;
    int r;

    if (!f || f->net_sock < 0) {
        return -VIBEOS_EBADF;
    }
    if (linux_read_sockaddr(addr_uptr, &ip, &port) != 0) {
        return -VIBEOS_EFAULT;
    }
    ks_lock(ks_net_lock(), __func__);
    r = vibeos_inet_bind(ks_net(), f->net_sock, port);
    ks_unlock(ks_net_lock());
    return (r == 0) ? 0 : -VIBEOS_EINVAL;
}

static long linux_sys_listen(uint64_t fd) {
    vibeos_fd_t *f = linux_fd_get(fd);
    int r;

    if (!f || f->net_sock < 0) {
        return -VIBEOS_EBADF;
    }
    ks_lock(ks_net_lock(), __func__);
    r = vibeos_inet_listen(ks_net(), f->net_sock);
    ks_unlock(ks_net_lock());
    return (r == 0) ? 0 : -VIBEOS_EINVAL;
}

/* A blocking socket call re-reads its descriptor on every wake, and a sibling
 * thread can close it and reuse the slot for a new socket while it sleeps -
 * M-020's slot-index ABA in the FD table, the same shape as H-007 and H-028.
 * Capture the socket index and the socket's generation before blocking, and on
 * every wake confirm the descriptor still names that socket and it is still the
 * same tenant. Returns the stable socket index, or -1 (and counts the ABA) when
 * the descriptor was closed (f->used clear), pointed at a different socket
 * (net_sock changed), or that socket slot was reused (generation changed). */
static int linux_sock_stable(const vibeos_fd_t *f, int sock, uint32_t gen) {
    uint32_t cur;
    if (!f->used || f->net_sock != sock) {
        ks_net()->sock_fd_aba++;
        return -1;
    }
    ks_lock(ks_net_lock(), __func__);
    cur = ks_net()->sockets[sock].gen;
    ks_unlock(ks_net_lock());
    if (cur != gen) {
        ks_net()->sock_fd_aba++;
        return -1;
    }
    return sock;
}

static long linux_sys_connect(uint64_t fd, uint64_t addr_uptr) {
    vibeos_fd_t *f = linux_fd_get(fd);
    uint32_t ip, gen;
    uint16_t port;
    uint64_t deadline;
    int r, sock;

    if (!f || f->net_sock < 0) {
        return -VIBEOS_EBADF;
    }
    if (linux_read_sockaddr(addr_uptr, &ip, &port) != 0) {
        return -VIBEOS_EFAULT;
    }
    sock = f->net_sock;
    ks_lock(ks_net_lock(), __func__);
    gen = ks_net()->sockets[sock].gen;
    r = vibeos_inet_connect(ks_net(), sock, ip, port);
    ks_unlock(ks_net_lock());
    if (r != 0) {
        return -VIBEOS_EINVAL;
    }

    deadline = ks_ticks() + LINUX_NET_TIMEOUT_SECONDS * ks_hz();
    for (;;) {
        int st;
        if (linux_sock_stable(f, sock, gen) < 0) {
            return -VIBEOS_EBADF;
        }
        ks_lock(ks_net_lock(), __func__);
        st = vibeos_inet_socket_state(ks_net(), sock);
        ks_unlock(ks_net_lock());
        if (st == VIBEOS_TCP_ESTABLISHED) {
            return 0;
        }
        if (st == VIBEOS_TCP_CLOSED || st < 0) {
            return -VIBEOS_EIO;   /* refused, reset, or gave up retransmitting */
        }
        if (ks_ticks() > deadline) {
            return -VIBEOS_EIO;
        }
        linux_net_wait_tick();
    }
}

static long linux_sys_accept(uint64_t fd, uint64_t addr_uptr) {
    vibeos_fd_t *f = linux_fd_get(fd);
    vibeos_procstate_t *ps;
    int me;
    int child = -1;
    int nfd, sock;
    uint32_t gen;

    if (!f || f->net_sock < 0 || ks_current() < 0) {
        return -VIBEOS_EBADF;
    }
    me = ks_current();
    /* A bad peer-address pointer is refused by the row, before a connection is
     * consumed (M-032). */
    sock = f->net_sock;
    ks_lock(ks_net_lock(), __func__);
    gen = ks_net()->sockets[sock].gen;
    ks_unlock(ks_net_lock());
    for (;;) {
        if (linux_sock_stable(f, sock, gen) < 0) {
            return -VIBEOS_EBADF;
        }
        ks_lock(ks_net_lock(), __func__);
        child = vibeos_inet_accept(ks_net(), sock);
        ks_unlock(ks_net_lock());
        if (child >= 0) {
            break;
        }
        if (child != -VIBEOS_INET_EAGAIN) {
            return -VIBEOS_EINVAL;
        }
        linux_net_wait_tick();
    }

    nfd = linux_fd_alloc(me);
    ps = ks_ps(me);
    if (nfd < 0) {
        ks_lock(ks_net_lock(), __func__);
        (void)vibeos_inet_close(ks_net(), child);
        ks_unlock(ks_net_lock());
        return -VIBEOS_EMFILE;
    }
    ps->files.fds[nfd].net_sock = child;
    ps->files.fds[nfd].pipe = -1;
    {
        uint32_t ip;
        uint16_t port;
        ks_lock(ks_net_lock(), __func__);
        ip = ks_net()->sockets[child].remote_ip;
        port = ks_net()->sockets[child].remote_port;
        /* The child was made by the stack and is owned by nobody; without an
         * owner, process exit never releases it (M-031). */
        (void)vibeos_inet_socket_set_owner(ks_net(), child, ks_id(me)->tgid);
        ks_unlock(ks_net_lock());
        if (linux_write_sockaddr(addr_uptr, ip, port) != 0) {
            /* The pointer went bad after the pre-check: undo the accept
             * rather than hand back a connection with no way to learn of it. */
            ps->files.fds[nfd].used = 0;
            ps->files.fds[nfd].net_sock = -1;
            ks_lock(ks_net_lock(), __func__);
            (void)vibeos_inet_close(ks_net(), child);
            ks_unlock(ks_net_lock());
            return -VIBEOS_EFAULT;
        }
    }
    return 3 + nfd;
}

/* Where socket data waits between the stack and user memory (H-010).
 *
 * The portable stack copies straight into whatever pointer it is given, and it
 * cannot use the fault-tolerant copy - that is assembly in the arch layer. So
 * receives go into this buffer and are then copied out, and sends are copied in
 * first. Both halves happen under the network lock, which serialises every user of it.
 * One receive buffer's worth: a socket never holds more than that. */
static uint8_t g_net_bounce[VIBEOS_INET_RXBUF];

/* Blocking stream receive: returns 0 at end of stream, like Linux. */
long linux_net_recv(vibeos_fd_t *f, uint64_t buf, uint64_t len) {
    uint64_t deadline = ks_ticks() + LINUX_NET_TIMEOUT_SECONDS * ks_hz();
    int sock;
    uint32_t gen;

    if (!f || f->net_sock < 0) {
        return -VIBEOS_EBADF;
    }
    sock = f->net_sock;
    ks_lock(ks_net_lock(), __func__);
    gen = ks_net()->sockets[sock].gen;
    ks_unlock(ks_net_lock());
    for (;;) {
        long n;
        int faulted = 0;
        if (linux_sock_stable(f, sock, gen) < 0) {
            return -VIBEOS_EBADF;
        }
        ks_lock(ks_net_lock(), __func__);
        n = vibeos_inet_recv(ks_net(), sock, g_net_bounce,
                             (uint32_t)(len < sizeof(g_net_bounce) ? len : sizeof(g_net_bounce)));
        if (n > 0 && vibeos_uaccess_copy((void *)(uintptr_t)buf, g_net_bounce, (uint64_t)n) != 0) {
            faulted = 1;
        }
        ks_unlock(ks_net_lock());
        if (faulted) {
            return -VIBEOS_EFAULT;
        }
        if (n >= 0) {
            return n;
        }
        if (n == -VIBEOS_INET_ECONNRESET) {
            return -VIBEOS_EIO;
        }
        if (n != -VIBEOS_INET_EAGAIN) {
            return -VIBEOS_EINVAL;
        }
        if (ks_ticks() > deadline) {
            return -VIBEOS_EIO;
        }
        linux_net_wait_tick();
    }
}

long linux_net_send(vibeos_fd_t *f, uint64_t buf, uint64_t len) {
    long n;
    if (len > sizeof(g_net_bounce)) {
        len = sizeof(g_net_bounce);   /* a short send, which a stream allows */
    }
    ks_lock(ks_net_lock(), __func__);
    if (vibeos_uaccess_copy(g_net_bounce, (const void *)(uintptr_t)buf, len) != 0) {
        ks_unlock(ks_net_lock());
        return -VIBEOS_EFAULT;
    }
    n = vibeos_inet_send(ks_net(), f->net_sock, g_net_bounce, (uint32_t)len);
    ks_unlock(ks_net_lock());
    if (n < 0) {
        return (n == -VIBEOS_INET_EAGAIN) ? 0 : -VIBEOS_EIO;
    }
    return n;
}

static long linux_sys_sendto(uint64_t fd, uint64_t buf, uint64_t len, uint64_t addr_uptr) {
    vibeos_fd_t *f = linux_fd_get(fd);
    uint32_t ip;
    uint16_t port;
    long n;

    if (!f || f->net_sock < 0) {
        return -VIBEOS_EBADF;
    }
    if (addr_uptr == 0u) {
        return linux_net_send(f, buf, len);
    }
    if (linux_read_sockaddr(addr_uptr, &ip, &port) != 0) {
        return -VIBEOS_EFAULT;
    }
    if (len > sizeof(g_net_bounce)) {
        return -VIBEOS_EINVAL;   /* a datagram is not split */
    }
    ks_lock(ks_net_lock(), __func__);
    if (vibeos_uaccess_copy(g_net_bounce, (const void *)(uintptr_t)buf, len) != 0) {
        ks_unlock(ks_net_lock());
        return -VIBEOS_EFAULT;
    }
    n = vibeos_inet_sendto(ks_net(), f->net_sock, g_net_bounce, (uint32_t)len, ip, port);
    ks_unlock(ks_net_lock());
    return (n < 0) ? -VIBEOS_EIO : n;
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
                linux_net_wait_tick();
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
                linux_net_wait_tick();
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

static long linux_sys_recvfrom(uint64_t fd, uint64_t buf, uint64_t len, uint64_t addr_uptr) {
    vibeos_fd_t *f = linux_fd_get(fd);
    uint64_t deadline;
    uint32_t gen;
    int sock;

    if (!f || f->net_sock < 0) {
        return -VIBEOS_EBADF;
    }
    /* The socket and its generation, taken once and re-verified on every pass
     * (M-020). This loop waits, and it used to re-read f->net_sock each time
     * round: a sibling thread that closed the descriptor and opened another
     * socket into the same slot had this call receive on the new one. M-020's
     * fix reached connect, accept and the stream read and missed this one,
     * which check-net-stable.py now makes impossible to miss again. */
    sock = f->net_sock;
    ks_lock(ks_net_lock(), __func__);
    gen = ks_net()->sockets[sock].gen;
    ks_unlock(ks_net_lock());
    /* The buffer and the source-address pointer are refused by the row, before the
     * datagram is dequeued - or a bad pointer loses it with no error (M-032). */
    deadline = ks_ticks() + LINUX_NET_TIMEOUT_SECONDS * ks_hz();
    for (;;) {
        long n;
        uint32_t ip = 0;
        uint16_t port = 0;
        int faulted = 0;
        if (linux_sock_stable(f, sock, gen) < 0) {
            return -VIBEOS_EBADF;
        }
        ks_lock(ks_net_lock(), __func__);
        n = vibeos_inet_recvfrom(ks_net(), sock, g_net_bounce,
                                 (uint32_t)(len < sizeof(g_net_bounce) ? len : sizeof(g_net_bounce)),
                                 &ip, &port);
        if (n > 0 && vibeos_uaccess_copy((void *)(uintptr_t)buf, g_net_bounce, (uint64_t)n) != 0) {
            faulted = 1;
        }
        ks_unlock(ks_net_lock());
        if (faulted) {
            return -VIBEOS_EFAULT;
        }
        if (n >= 0) {
            if (linux_write_sockaddr(addr_uptr, ip, port) != 0) {
                return -VIBEOS_EFAULT;
            }
            return n;
        }
        if (n != -VIBEOS_INET_EAGAIN) {
            return -VIBEOS_EINVAL;
        }
        if (ks_ticks() > deadline) {
            return -VIBEOS_EIO;
        }
        linux_net_wait_tick();
    }
}

/* ---- the syscalls this file implements ---------------------------------------
 *
 * The Linux ABI passes the 4th, 5th and 6th arguments in r10, r8 and r9, which is
 * why sendto and recvfrom read ARG(4): the peer address is the fifth argument. */
#define LINUX_NET_SYSCALLS(X) \
    X(41,   socket,   SOCKET,   NOPTR, linux_sys_socket(ARG(0), ARG(1))) \
    X(42,   connect,  CONNECT,  PTRS(IN(1, 8)), linux_sys_connect(ARG(0), ARG(1))) \
    X(43,   accept,   ACCEPT,   PTRS(OUT_OPT(1, 16)), linux_sys_accept(ARG(0), ARG(1))) \
    X(44,   sendto,   SENDTO,   PTRS(IN_BUF(1, 2), IN_OPT(4, 8)), linux_sys_sendto(ARG(0), ARG(1), ARG(2), ARG(4))) \
    X(45,   recvfrom, RECVFROM, PTRS(OUT_BUF(1, 2), OUT_OPT(4, 16)), linux_sys_recvfrom(ARG(0), ARG(1), ARG(2), ARG(4))) \
    X(49,   bind,     BIND,     PTRS(IN(1, 8)), linux_sys_bind(ARG(0), ARG(1))) \
    X(50,   listen,   LISTEN,   NOPTR, linux_sys_listen(ARG(0))) \
    X(1000, netctl,   NETCTL,   PTRS(OUT_IF(0, 0, 1, 20), OUT_IF(0, 3, 1, 32)), linux_sys_netctl(ARG(0), ARG(1)))

LINUX_DEFINE_SYSCALLS(net, LINUX_NET_SYSCALLS)
