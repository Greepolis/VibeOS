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

#include "arch_hw_internal.h"

/* Read a struct sockaddr_in out of user memory: family (host order), port and
 * address (both network order on the wire). */
static int hw_read_sockaddr(uint64_t uptr, uint32_t *out_ip, uint16_t *out_port) {
    const uint8_t *p;
    if (!hw_user_range_ok(uptr, 8, 0)) {
        return -1;
    }
    p = (const uint8_t *)(uintptr_t)uptr;
    if (((uint16_t)p[0] | ((uint16_t)p[1] << 8)) != 2u) {   /* AF_INET */
        return -1;
    }
    *out_port = (uint16_t)(((uint16_t)p[2] << 8) | p[3]);
    *out_ip = ((uint32_t)p[4] << 24) | ((uint32_t)p[5] << 16) |
              ((uint32_t)p[6] << 8) | (uint32_t)p[7];
    return 0;
}

static int hw_write_sockaddr(uint64_t uptr, uint32_t ip, uint16_t port) {
    uint8_t *p;
    if (uptr == 0u) {
        return 0;
    }
    if (!hw_user_range_ok(uptr, 16, 1)) {
        return -1;
    }
    p = (uint8_t *)(uintptr_t)uptr;
    p[0] = 2; p[1] = 0;
    p[2] = (uint8_t)(port >> 8);
    p[3] = (uint8_t)(port & 0xFFu);
    p[4] = (uint8_t)(ip >> 24);
    p[5] = (uint8_t)((ip >> 16) & 0xFFu);
    p[6] = (uint8_t)((ip >> 8) & 0xFFu);
    p[7] = (uint8_t)(ip & 0xFFu);
    {
        int k;
        for (k = 8; k < 16; k++) {
            p[k] = 0;
        }
    }
    return 0;
}

/* Give up the CPU until the next tick; the network is pumped from there. */
static void hw_net_wait_tick(void) {
    __asm__ __volatile__("sti; hlt" ::: "memory");
}

long hw_sys_socket(uint64_t domain, uint64_t type) {
    hw_task_t *t;
    int fd, s;
    int kind;

    if (!g_net_up || hw_current_task() < 0 || !g_tasks[hw_current_task()].is_user) {
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
    if (s >= 0 && vibeos_inet_socket_set_owner(&g_net, s, t->tgid) != 0) {
        (void)vibeos_inet_close(&g_net, s);
        s = -1;
    }
    hw_spin_unlock(&g_net_lock);
    if (s < 0) {
        t->fds[fd].used = 0;
        return -VIBEOS_ENOMEM;
    }
    t->fds[fd].net_sock = s;
    t->fds[fd].pipe = -1;
    return 3 + fd;
}

long hw_sys_bind(uint64_t fd, uint64_t addr_uptr) {
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

long hw_sys_listen(uint64_t fd) {
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

long hw_sys_connect(uint64_t fd, uint64_t addr_uptr) {
    hw_fd_t *f = hw_fd_get(fd);
    uint32_t ip;
    uint16_t port;
    uint64_t deadline;
    int r;

    if (!f || f->net_sock < 0) {
        return -VIBEOS_EBADF;
    }
    if (hw_read_sockaddr(addr_uptr, &ip, &port) != 0) {
        return -VIBEOS_EFAULT;
    }
    hw_spin_lock(&g_net_lock);
    r = vibeos_inet_connect(&g_net, f->net_sock, ip, port);
    hw_spin_unlock(&g_net_lock);
    if (r != 0) {
        return -VIBEOS_EINVAL;
    }

    deadline = g_timer_ticks + VIBEOS_HW_NET_TIMEOUT_TICKS;
    for (;;) {
        int st;
        hw_spin_lock(&g_net_lock);
        st = vibeos_inet_socket_state(&g_net, f->net_sock);
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

long hw_sys_accept(uint64_t fd, uint64_t addr_uptr) {
    hw_fd_t *f = hw_fd_get(fd);
    hw_task_t *t;
    int child = -1;
    int nfd;

    if (!f || f->net_sock < 0 || hw_current_task() < 0) {
        return -VIBEOS_EBADF;
    }
    t = &g_tasks[hw_current_task()];
    for (;;) {
        hw_spin_lock(&g_net_lock);
        child = vibeos_inet_accept(&g_net, f->net_sock);
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
    t->fds[nfd].net_sock = child;
    t->fds[nfd].pipe = -1;
    {
        uint32_t ip;
        uint16_t port;
        hw_spin_lock(&g_net_lock);
        ip = g_net.sockets[child].remote_ip;
        port = g_net.sockets[child].remote_port;
        hw_spin_unlock(&g_net_lock);
        (void)hw_write_sockaddr(addr_uptr, ip, port);
    }
    return 3 + nfd;
}

/* Blocking stream receive: returns 0 at end of stream, like Linux. */
long hw_net_recv(hw_fd_t *f, uint64_t buf, uint64_t len) {
    uint64_t deadline = g_timer_ticks + VIBEOS_HW_NET_TIMEOUT_TICKS;

    if (!hw_user_range_ok(buf, len, 1)) {
        return -VIBEOS_EFAULT;
    }
    for (;;) {
        long n;
        hw_spin_lock(&g_net_lock);
        n = vibeos_inet_recv(&g_net, f->net_sock, (void *)(uintptr_t)buf, (uint32_t)len);
        hw_spin_unlock(&g_net_lock);
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
    if (!hw_user_range_ok(buf, len, 0)) {
        return -VIBEOS_EFAULT;
    }
    hw_spin_lock(&g_net_lock);
    n = vibeos_inet_send(&g_net, f->net_sock, (const void *)(uintptr_t)buf, (uint32_t)len);
    hw_spin_unlock(&g_net_lock);
    if (n < 0) {
        return (n == -VIBEOS_INET_EAGAIN) ? 0 : -VIBEOS_EIO;
    }
    return n;
}

long hw_sys_sendto(uint64_t fd, uint64_t buf, uint64_t len, uint64_t addr_uptr) {
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
    if (!hw_user_range_ok(buf, len, 0)) {
        return -VIBEOS_EFAULT;
    }
    hw_spin_lock(&g_net_lock);
    n = vibeos_inet_sendto(&g_net, f->net_sock, (const void *)(uintptr_t)buf,
                           (uint32_t)len, ip, port);
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
long hw_sys_netctl(uint64_t op, uint64_t arg) {
    uint64_t deadline;

    if (!g_net_up) {
        return -VIBEOS_EIO;
    }
    switch (op) {
        case 0: {
            uint32_t *out;
            if (!hw_user_range_ok(arg, 20, 1)) {
                return -VIBEOS_EFAULT;
            }
            out = (uint32_t *)(uintptr_t)arg;
            hw_spin_lock(&g_net_lock);
            out[0] = g_net.ip;
            out[1] = g_net.netmask;
            out[2] = g_net.gateway;
            out[3] = g_net.dns;
            out[4] = (uint32_t)vibeos_inet_dhcp_bound(&g_net);
            hw_spin_unlock(&g_net_lock);
            return 0;
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
            uint64_t *out;
            if (!hw_user_range_ok(arg, 32, 1)) {
                return -VIBEOS_EFAULT;
            }
            out = (uint64_t *)(uintptr_t)arg;
            hw_spin_lock(&g_net_lock);
            out[0] = g_net.tx_frames;
            out[1] = g_net.rx_frames;
            out[2] = g_net.rx_dropped;
            out[3] = g_net.tcp_retransmits;
            hw_spin_unlock(&g_net_lock);
            return 0;
        }
        default:
            return -VIBEOS_EINVAL;
    }
}

long hw_sys_recvfrom(uint64_t fd, uint64_t buf, uint64_t len, uint64_t addr_uptr) {
    hw_fd_t *f = hw_fd_get(fd);
    uint64_t deadline;

    if (!f || f->net_sock < 0) {
        return -VIBEOS_EBADF;
    }
    if (!hw_user_range_ok(buf, len, 1)) {
        return -VIBEOS_EFAULT;
    }
    deadline = g_timer_ticks + VIBEOS_HW_NET_TIMEOUT_TICKS;
    for (;;) {
        long n;
        uint32_t ip = 0;
        uint16_t port = 0;
        hw_spin_lock(&g_net_lock);
        n = vibeos_inet_recvfrom(&g_net, f->net_sock, (void *)(uintptr_t)buf,
                                 (uint32_t)len, &ip, &port);
        hw_spin_unlock(&g_net_lock);
        if (n >= 0) {
            (void)hw_write_sockaddr(addr_uptr, ip, port);
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

