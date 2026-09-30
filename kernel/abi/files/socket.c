/* A socket as a file, and the waits a socket call makes (docs/abi/ A3).
 *
 * The TCP/IP stack is kernel/net/inet.c; this is what a descriptor naming one of
 * its sockets does. Moved from the Linux socket handlers, which kept only the
 * ABI's half - reading a sockaddr, writing one back, installing a descriptor - so
 * another personality's socket calls can wait the same way.
 *
 * Every wait below holds a reference to the description, so it cannot be closed
 * and reused under the caller: that half of M-020 (a sibling thread closing the
 * descriptor and opening another socket into the slot while a call slept) is
 * structural now. The other half is not: the socket itself can still go - a
 * process exiting releases the sockets it owns - and its slot in the stack be
 * given to a new socket. So every pass re-checks the socket's tenancy, and
 * check-net-stable.py holds every waiting function here to it. */

#include "files_internal.h"

#define SOCKET_TIMEOUT_SECONDS 10u

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

static long socket_recv(vibeos_file_t *f, uint64_t buf, uint64_t len) {
    uint64_t deadline = ks_ticks() + SOCKET_TIMEOUT_SECONDS * ks_hz();

    for (;;) {
        long n;
        int faulted = 0, sock;
        if ((sock = vibeos_sockfile_stable(f)) < 0) {
            return -VIBEOS_EBADF;
        }
        ks_lock(ks_net_lock(), __func__);
        n = vibeos_inet_recv(ks_net(), sock, g_bounce,
                             (uint32_t)(len < sizeof(g_bounce) ? len : sizeof(g_bounce)));
        if (n > 0 && vibeos_uaccess_copy((void *)(uintptr_t)buf, g_bounce, (uint64_t)n) != 0) {
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
        socket_wait_tick();
    }
}

static long socket_send(vibeos_file_t *f, uint64_t buf, uint64_t len) {
    long n;
    int sock;

    if ((sock = vibeos_sockfile_stable(f)) < 0) {
        return -VIBEOS_EBADF;
    }
    if (len > sizeof(g_bounce)) {
        len = sizeof(g_bounce);   /* a short send, which a stream allows */
    }
    ks_lock(ks_net_lock(), __func__);
    if (vibeos_uaccess_copy(g_bounce, (const void *)(uintptr_t)buf, len) != 0) {
        ks_unlock(ks_net_lock());
        return -VIBEOS_EFAULT;
    }
    n = vibeos_inet_send(ks_net(), sock, g_bounce, (uint32_t)len);
    ks_unlock(ks_net_lock());
    if (n < 0) {
        return (n == -VIBEOS_INET_EAGAIN) ? 0 : -VIBEOS_EIO;
    }
    return n;
}

static int socket_stat(vibeos_file_t *f, vibeos_file_stat_t *out) {
    out->mode = VIBEOS_S_IFSOCK | 0600u;
    out->size = 0;
    out->ino = 0x20000u + (uint64_t)(uint32_t)f->sock;
    return 0;
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
    "socket", socket_recv, socket_send, 0, socket_stat, 0, 0, socket_release
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
    ks_unlock(ks_net_lock());
    return f;
}

vibeos_file_t *vibeos_sockfile_create(int kind, uint32_t owner, long *err) {
    vibeos_file_t *f;
    int s;

    if (!ks_net()) {
        *err = -VIBEOS_EINVAL;
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
        *err = -VIBEOS_ENOMEM;
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

long vibeos_sockfile_bind(vibeos_file_t *f, uint16_t port) {
    int r, sock;

    if ((sock = vibeos_sockfile_stable(f)) < 0) {
        return -VIBEOS_EBADF;
    }
    ks_lock(ks_net_lock(), __func__);
    r = vibeos_inet_bind(ks_net(), sock, port);
    ks_unlock(ks_net_lock());
    return (r == 0) ? 0 : -VIBEOS_EINVAL;
}

long vibeos_sockfile_listen(vibeos_file_t *f) {
    int r, sock;

    if ((sock = vibeos_sockfile_stable(f)) < 0) {
        return -VIBEOS_EBADF;
    }
    ks_lock(ks_net_lock(), __func__);
    r = vibeos_inet_listen(ks_net(), sock);
    ks_unlock(ks_net_lock());
    return (r == 0) ? 0 : -VIBEOS_EINVAL;
}

long vibeos_sockfile_connect(vibeos_file_t *f, uint32_t ip, uint16_t port) {
    uint64_t deadline;
    int r, sock;

    if ((sock = vibeos_sockfile_stable(f)) < 0) {
        return -VIBEOS_EBADF;
    }
    ks_lock(ks_net_lock(), __func__);
    r = vibeos_inet_connect(ks_net(), sock, ip, port);
    ks_unlock(ks_net_lock());
    if (r != 0) {
        return -VIBEOS_EINVAL;
    }
    deadline = ks_ticks() + SOCKET_TIMEOUT_SECONDS * ks_hz();
    for (;;) {
        int st;
        if ((sock = vibeos_sockfile_stable(f)) < 0) {
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
        socket_wait_tick();
    }
}

long vibeos_sockfile_accept(vibeos_file_t *f, uint32_t owner, vibeos_file_t **child_out,
                            uint32_t *ip, uint16_t *port) {
    int child, sock;
    vibeos_file_t *c;

    *child_out = 0;
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
            return -VIBEOS_EINVAL;
        }
        socket_wait_tick();
    }
    ks_lock(ks_net_lock(), __func__);
    *ip = ks_net()->sockets[child].remote_ip;
    *port = ks_net()->sockets[child].remote_port;
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
    *child_out = c;
    return 0;
}

long vibeos_sockfile_sendto(vibeos_file_t *f, uint64_t buf, uint64_t len,
                            uint32_t ip, uint16_t port) {
    long n;
    int sock;

    if ((sock = vibeos_sockfile_stable(f)) < 0) {
        return -VIBEOS_EBADF;
    }
    if (len > sizeof(g_bounce)) {
        return -VIBEOS_EINVAL;   /* a datagram is not split */
    }
    ks_lock(ks_net_lock(), __func__);
    if (vibeos_uaccess_copy(g_bounce, (const void *)(uintptr_t)buf, len) != 0) {
        ks_unlock(ks_net_lock());
        return -VIBEOS_EFAULT;
    }
    n = vibeos_inet_sendto(ks_net(), sock, g_bounce, (uint32_t)len, ip, port);
    ks_unlock(ks_net_lock());
    return (n < 0) ? -VIBEOS_EIO : n;
}

long vibeos_sockfile_recvfrom(vibeos_file_t *f, uint64_t buf, uint64_t len,
                              uint32_t *ip, uint16_t *port) {
    uint64_t deadline = ks_ticks() + SOCKET_TIMEOUT_SECONDS * ks_hz();

    for (;;) {
        long n;
        int faulted = 0, sock;
        if ((sock = vibeos_sockfile_stable(f)) < 0) {
            return -VIBEOS_EBADF;
        }
        ks_lock(ks_net_lock(), __func__);
        n = vibeos_inet_recvfrom(ks_net(), sock, g_bounce,
                                 (uint32_t)(len < sizeof(g_bounce) ? len : sizeof(g_bounce)),
                                 ip, port);
        if (n > 0 && vibeos_uaccess_copy((void *)(uintptr_t)buf, g_bounce, (uint64_t)n) != 0) {
            faulted = 1;
        }
        ks_unlock(ks_net_lock());
        if (faulted) {
            return -VIBEOS_EFAULT;
        }
        if (n >= 0) {
            return n;
        }
        if (n != -VIBEOS_INET_EAGAIN) {
            return -VIBEOS_EINVAL;
        }
        if (ks_ticks() > deadline) {
            return -VIBEOS_EIO;
        }
        socket_wait_tick();
    }
}
