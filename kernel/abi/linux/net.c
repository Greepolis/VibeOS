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
 * What it still reaches back for is six globals and six functions, listed in
 * the header. That number is the honest cost of the cut, and it is the thing to
 * watch: a later cut that needs thirty is not a cut, it is a rename.
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
static int hw_read_sockaddr(uint64_t uptr, uint32_t *out_ip, uint16_t *out_port) {
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

static int hw_write_sockaddr(uint64_t uptr, uint32_t ip, uint16_t port) {
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
static void hw_net_wait_tick(void) {
    __asm__ __volatile__("sti; hlt" ::: "memory");
}

static long hw_sys_socket(uint64_t domain, uint64_t type) {
    hw_task_t *t;
    int fd, s;
    int kind;

    if (!g_net_up || hw_current_task() < 0 || !g_tasks[hw_current_task()].id.is_user) {
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

    t = &g_tasks[hw_current_task()];
    fd = hw_fd_alloc(t);
    if (fd < 0) {
        return -VIBEOS_EMFILE;
    }
    hw_spin_lock(&g_net_lock);
    s = vibeos_inet_socket(&g_net, kind);
    if (s >= 0 && vibeos_inet_socket_set_owner(&g_net, s, t->id.tgid) != 0) {
        (void)vibeos_inet_close(&g_net, s);
        s = -1;
    }
    hw_spin_unlock(&g_net_lock);
    if (s < 0) {
        t->files.fds[fd].used = 0;
        return -VIBEOS_ENOMEM;
    }
    t->files.fds[fd].net_sock = s;
    t->files.fds[fd].pipe = -1;
    return 3 + fd;
}

static long hw_sys_bind(uint64_t fd, uint64_t addr_uptr) {
    hw_fd_t *f = hw_fd_get(fd);
    uint32_t ip;
    uint16_t port;
    int r;

    if (!f || f->net_sock < 0) {
        return -VIBEOS_EBADF;
    }
    if (hw_read_sockaddr(addr_uptr, &ip, &port) != 0) {
        return -VIBEOS_EFAULT;
    }
    hw_spin_lock(&g_net_lock);
    r = vibeos_inet_bind(&g_net, f->net_sock, port);
    hw_spin_unlock(&g_net_lock);
    return (r == 0) ? 0 : -VIBEOS_EINVAL;
}

static long hw_sys_listen(uint64_t fd) {
    hw_fd_t *f = hw_fd_get(fd);
    int r;

    if (!f || f->net_sock < 0) {
        return -VIBEOS_EBADF;
    }
    hw_spin_lock(&g_net_lock);
    r = vibeos_inet_listen(&g_net, f->net_sock);
    hw_spin_unlock(&g_net_lock);
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
static int hw_sock_stable(const hw_fd_t *f, int sock, uint32_t gen) {
    uint32_t cur;
    if (!f->used || f->net_sock != sock) {
        g_net.sock_fd_aba++;
        return -1;
    }
    hw_spin_lock(&g_net_lock);
    cur = g_net.sockets[sock].gen;
    hw_spin_unlock(&g_net_lock);
    if (cur != gen) {
        g_net.sock_fd_aba++;
        return -1;
    }
    return sock;
}

static long hw_sys_connect(uint64_t fd, uint64_t addr_uptr) {
    hw_fd_t *f = hw_fd_get(fd);
    uint32_t ip, gen;
    uint16_t port;
    uint64_t deadline;
    int r, sock;

    if (!f || f->net_sock < 0) {
        return -VIBEOS_EBADF;
    }
    if (hw_read_sockaddr(addr_uptr, &ip, &port) != 0) {
        return -VIBEOS_EFAULT;
    }
    sock = f->net_sock;
    hw_spin_lock(&g_net_lock);
    gen = g_net.sockets[sock].gen;
    r = vibeos_inet_connect(&g_net, sock, ip, port);
    hw_spin_unlock(&g_net_lock);
    if (r != 0) {
        return -VIBEOS_EINVAL;
    }

    deadline = g_timer_ticks + VIBEOS_HW_NET_TIMEOUT_TICKS;
    for (;;) {
        int st;
        if (hw_sock_stable(f, sock, gen) < 0) {
            return -VIBEOS_EBADF;
        }
        hw_spin_lock(&g_net_lock);
        st = vibeos_inet_socket_state(&g_net, sock);
        hw_spin_unlock(&g_net_lock);
        if (st == VIBEOS_TCP_ESTABLISHED) {
            return 0;
        }
        if (st == VIBEOS_TCP_CLOSED || st < 0) {
            return -VIBEOS_EIO;   /* refused, reset, or gave up retransmitting */
        }
        if (g_timer_ticks > deadline) {
            return -VIBEOS_EIO;
        }
        hw_net_wait_tick();
    }
}

static long hw_sys_accept(uint64_t fd, uint64_t addr_uptr) {
    hw_fd_t *f = hw_fd_get(fd);
    hw_task_t *t;
    int child = -1;
    int nfd, sock;
    uint32_t gen;

    if (!f || f->net_sock < 0 || hw_current_task() < 0) {
        return -VIBEOS_EBADF;
    }
    t = &g_tasks[hw_current_task()];
    /* A bad peer-address pointer is refused by the row, before a connection is
     * consumed (M-032). */
    sock = f->net_sock;
    hw_spin_lock(&g_net_lock);
    gen = g_net.sockets[sock].gen;
    hw_spin_unlock(&g_net_lock);
    for (;;) {
        if (hw_sock_stable(f, sock, gen) < 0) {
            return -VIBEOS_EBADF;
        }
        hw_spin_lock(&g_net_lock);
        child = vibeos_inet_accept(&g_net, sock);
        hw_spin_unlock(&g_net_lock);
        if (child >= 0) {
            break;
        }
        if (child != -VIBEOS_INET_EAGAIN) {
            return -VIBEOS_EINVAL;
        }
        hw_net_wait_tick();
    }

    nfd = hw_fd_alloc(t);
    if (nfd < 0) {
        hw_spin_lock(&g_net_lock);
        (void)vibeos_inet_close(&g_net, child);
        hw_spin_unlock(&g_net_lock);
        return -VIBEOS_EMFILE;
    }
    t->files.fds[nfd].net_sock = child;
    t->files.fds[nfd].pipe = -1;
    {
        uint32_t ip;
        uint16_t port;
        hw_spin_lock(&g_net_lock);
        ip = g_net.sockets[child].remote_ip;
        port = g_net.sockets[child].remote_port;
        /* The child was made by the stack and is owned by nobody; without an
         * owner, process exit never releases it (M-031). */
        (void)vibeos_inet_socket_set_owner(&g_net, child, t->id.tgid);
        hw_spin_unlock(&g_net_lock);
        if (hw_write_sockaddr(addr_uptr, ip, port) != 0) {
            /* The pointer went bad after the pre-check: undo the accept
             * rather than hand back a connection with no way to learn of it. */
            t->files.fds[nfd].used = 0;
            t->files.fds[nfd].net_sock = -1;
            hw_spin_lock(&g_net_lock);
            (void)vibeos_inet_close(&g_net, child);
            hw_spin_unlock(&g_net_lock);
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
 * first. Both halves happen under g_net_lock, which serialises every user of it.
 * One receive buffer's worth: a socket never holds more than that. */
static uint8_t g_net_bounce[VIBEOS_INET_RXBUF];

/* Blocking stream receive: returns 0 at end of stream, like Linux. */
long hw_net_recv(hw_fd_t *f, uint64_t buf, uint64_t len) {
    uint64_t deadline = g_timer_ticks + VIBEOS_HW_NET_TIMEOUT_TICKS;
    int sock;
    uint32_t gen;

    if (!f || f->net_sock < 0) {
        return -VIBEOS_EBADF;
    }
    sock = f->net_sock;
    hw_spin_lock(&g_net_lock);
    gen = g_net.sockets[sock].gen;
    hw_spin_unlock(&g_net_lock);
    for (;;) {
        long n;
        int faulted = 0;
        if (hw_sock_stable(f, sock, gen) < 0) {
            return -VIBEOS_EBADF;
        }
        hw_spin_lock(&g_net_lock);
        n = vibeos_inet_recv(&g_net, sock, g_net_bounce,
                             (uint32_t)(len < sizeof(g_net_bounce) ? len : sizeof(g_net_bounce)));
        if (n > 0 && vibeos_uaccess_copy((void *)(uintptr_t)buf, g_net_bounce, (uint64_t)n) != 0) {
            faulted = 1;
        }
        hw_spin_unlock(&g_net_lock);
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
        if (g_timer_ticks > deadline) {
            return -VIBEOS_EIO;
        }
        hw_net_wait_tick();
    }
}

long hw_net_send(hw_fd_t *f, uint64_t buf, uint64_t len) {
    long n;
    if (len > sizeof(g_net_bounce)) {
        len = sizeof(g_net_bounce);   /* a short send, which a stream allows */
    }
    hw_spin_lock(&g_net_lock);
    if (vibeos_uaccess_copy(g_net_bounce, (const void *)(uintptr_t)buf, len) != 0) {
        hw_spin_unlock(&g_net_lock);
        return -VIBEOS_EFAULT;
    }
    n = vibeos_inet_send(&g_net, f->net_sock, g_net_bounce, (uint32_t)len);
    hw_spin_unlock(&g_net_lock);
    if (n < 0) {
        return (n == -VIBEOS_INET_EAGAIN) ? 0 : -VIBEOS_EIO;
    }
    return n;
}

static long hw_sys_sendto(uint64_t fd, uint64_t buf, uint64_t len, uint64_t addr_uptr) {
    hw_fd_t *f = hw_fd_get(fd);
    uint32_t ip;
    uint16_t port;
    long n;

    if (!f || f->net_sock < 0) {
        return -VIBEOS_EBADF;
    }
    if (addr_uptr == 0u) {
        return hw_net_send(f, buf, len);
    }
    if (hw_read_sockaddr(addr_uptr, &ip, &port) != 0) {
        return -VIBEOS_EFAULT;
    }
    if (len > sizeof(g_net_bounce)) {
        return -VIBEOS_EINVAL;   /* a datagram is not split */
    }
    hw_spin_lock(&g_net_lock);
    if (vibeos_uaccess_copy(g_net_bounce, (const void *)(uintptr_t)buf, len) != 0) {
        hw_spin_unlock(&g_net_lock);
        return -VIBEOS_EFAULT;
    }
    n = vibeos_inet_sendto(&g_net, f->net_sock, g_net_bounce, (uint32_t)len, ip, port);
    hw_spin_unlock(&g_net_lock);
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
static long hw_sys_netctl(uint64_t op, uint64_t arg) {
    uint64_t deadline;

    if (!g_net_up) {
        return -VIBEOS_EIO;
    }
    switch (op) {
        case 0: {
            /* Gathered under the lock, written after it (M-050): a user store
             * that faults under g_net_lock is a panic with the network lock
             * held, and the row's range check before the handler does not
             * survive a sibling's munmap. */
            uint32_t out[5];
            hw_spin_lock(&g_net_lock);
            out[0] = g_net.ip;
            out[1] = g_net.netmask;
            out[2] = g_net.gateway;
            out[3] = g_net.dns;
            out[4] = (uint32_t)vibeos_inet_dhcp_bound(&g_net);
            hw_spin_unlock(&g_net_lock);
            return vibeos_uaccess_copy((void *)(uintptr_t)arg, out, sizeof(out)) == 0
                       ? 0 : -VIBEOS_EFAULT;
        }
        case 1: {
            hw_spin_lock(&g_net_lock);
            (void)vibeos_inet_ping(&g_net, (uint32_t)arg);
            hw_spin_unlock(&g_net_lock);
            deadline = g_timer_ticks + (VIBEOS_HW_TIMER_HZ * 4u);
            for (;;) {
                uint64_t rtt = 0;
                int r;
                hw_spin_lock(&g_net_lock);
                r = vibeos_inet_ping_result(&g_net, &rtt);
                hw_spin_unlock(&g_net_lock);
                if (r == 0) {
                    return (long)rtt;
                }
                if (g_timer_ticks > deadline) {
                    return -VIBEOS_EIO;
                }
                hw_net_wait_tick();
            }
        }
        case 2: {
            char name[64];
            if (hw_copy_user_string(arg, name, sizeof(name)) != 0) {
                return -VIBEOS_EFAULT;
            }
            hw_spin_lock(&g_net_lock);
            (void)vibeos_inet_resolve(&g_net, name);
            hw_spin_unlock(&g_net_lock);
            deadline = g_timer_ticks + (VIBEOS_HW_TIMER_HZ * 5u);
            for (;;) {
                uint32_t ip = 0;
                int r;
                hw_spin_lock(&g_net_lock);
                r = vibeos_inet_resolve_result(&g_net, &ip);
                hw_spin_unlock(&g_net_lock);
                if (r == 0) {
                    return (long)ip;
                }
                if (r != -VIBEOS_INET_EAGAIN || g_timer_ticks > deadline) {
                    return -VIBEOS_ENOENT;
                }
                hw_net_wait_tick();
            }
        }
        case 3: {
            uint64_t out[4];   /* as op 0: gathered under the lock, written after */
            hw_spin_lock(&g_net_lock);
            out[0] = g_net.tx_frames;
            out[1] = g_net.rx_frames;
            out[2] = g_net.rx_dropped;
            out[3] = g_net.tcp_retransmits;
            hw_spin_unlock(&g_net_lock);
            return vibeos_uaccess_copy((void *)(uintptr_t)arg, out, sizeof(out)) == 0
                       ? 0 : -VIBEOS_EFAULT;
        }
        default:
            return -VIBEOS_EINVAL;
    }
}

static long hw_sys_recvfrom(uint64_t fd, uint64_t buf, uint64_t len, uint64_t addr_uptr) {
    hw_fd_t *f = hw_fd_get(fd);
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
    hw_spin_lock(&g_net_lock);
    gen = g_net.sockets[sock].gen;
    hw_spin_unlock(&g_net_lock);
    /* The buffer and the source-address pointer are refused by the row, before the
     * datagram is dequeued - or a bad pointer loses it with no error (M-032). */
    deadline = g_timer_ticks + VIBEOS_HW_NET_TIMEOUT_TICKS;
    for (;;) {
        long n;
        uint32_t ip = 0;
        uint16_t port = 0;
        int faulted = 0;
        if (hw_sock_stable(f, sock, gen) < 0) {
            return -VIBEOS_EBADF;
        }
        hw_spin_lock(&g_net_lock);
        n = vibeos_inet_recvfrom(&g_net, sock, g_net_bounce,
                                 (uint32_t)(len < sizeof(g_net_bounce) ? len : sizeof(g_net_bounce)),
                                 &ip, &port);
        if (n > 0 && vibeos_uaccess_copy((void *)(uintptr_t)buf, g_net_bounce, (uint64_t)n) != 0) {
            faulted = 1;
        }
        hw_spin_unlock(&g_net_lock);
        if (faulted) {
            return -VIBEOS_EFAULT;
        }
        if (n >= 0) {
            if (hw_write_sockaddr(addr_uptr, ip, port) != 0) {
                return -VIBEOS_EFAULT;
            }
            return n;
        }
        if (n != -VIBEOS_INET_EAGAIN) {
            return -VIBEOS_EINVAL;
        }
        if (g_timer_ticks > deadline) {
            return -VIBEOS_EIO;
        }
        hw_net_wait_tick();
    }
}

/* ---- the syscalls this file implements ---------------------------------------
 *
 * The Linux ABI passes the 4th, 5th and 6th arguments in r10, r8 and r9, which is
 * why sendto and recvfrom read ARG(4): the peer address is the fifth argument. */
#define LINUX_NET_SYSCALLS(X) \
    X(41,   socket,   SOCKET,   NOPTR, hw_sys_socket(ARG(0), ARG(1))) \
    X(42,   connect,  CONNECT,  PTRS(IN(1, 8)), hw_sys_connect(ARG(0), ARG(1))) \
    X(43,   accept,   ACCEPT,   PTRS(OUT_OPT(1, 16)), hw_sys_accept(ARG(0), ARG(1))) \
    X(44,   sendto,   SENDTO,   PTRS(IN_BUF(1, 2), IN_OPT(4, 8)), hw_sys_sendto(ARG(0), ARG(1), ARG(2), ARG(4))) \
    X(45,   recvfrom, RECVFROM, PTRS(OUT_BUF(1, 2), OUT_OPT(4, 16)), hw_sys_recvfrom(ARG(0), ARG(1), ARG(2), ARG(4))) \
    X(49,   bind,     BIND,     PTRS(IN(1, 8)), hw_sys_bind(ARG(0), ARG(1))) \
    X(50,   listen,   LISTEN,   NOPTR, hw_sys_listen(ARG(0))) \
    X(1000, netctl,   NETCTL,   PTRS(OUT_IF(0, 0, 1, 20), OUT_IF(0, 3, 1, 32)), hw_sys_netctl(ARG(0), ARG(1)))

LINUX_DEFINE_SYSCALLS(net, LINUX_NET_SYSCALLS)
