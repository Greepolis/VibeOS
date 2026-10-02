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
 *
 * Since A3 a socket is an open file description, and the waits a socket call
 * makes - connect, accept, recvfrom - are the socket type's
 * (kernel/abi/files/socket.c), where another personality can make them too.
 * What is left here is Linux's half: struct sockaddr_in in and out, and
 * descriptors.
 */

#include <stddef.h>

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
/* Network order is big-endian and this machine is not; bytes, so it does not
 * matter which the host is. */
static uint16_t linux_be16(uint16_t wire) {
    const uint8_t *b = (const uint8_t *)&wire;
    return (uint16_t)(((uint16_t)b[0] << 8) | b[1]);
}

static uint32_t linux_be32(uint32_t wire) {
    const uint8_t *b = (const uint8_t *)&wire;
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
           ((uint32_t)b[2] << 8) | (uint32_t)b[3];
}

/* Only the family, port and address are read: 8 bytes, which is what the rows
 * declare, and what a program passing a bare sockaddr_in prefix gets away with
 * on Linux too. */
static int linux_read_sockaddr(uint64_t uptr, uint32_t *out_ip, uint16_t *out_port) {
    linux_sockaddr_in_t a;

    if (vibeos_uaccess_copy(&a, (const void *)(uintptr_t)uptr,
                            offsetof(linux_sockaddr_in_t, sin_zero)) != 0) {
        return -1;
    }
    if (a.sin_family != LINUX_AF_INET) {
        return -1;
    }
    *out_port = linux_be16(a.sin_port);
    *out_ip = linux_be32(a.sin_addr);
    return 0;
}

static int linux_write_sockaddr(uint64_t uptr, uint32_t ip, uint16_t port) {
    linux_sockaddr_in_t a;
    uint32_t k;

    if (uptr == 0u) {
        return 0;
    }
    a.sin_family = LINUX_AF_INET;
    a.sin_port = linux_be16(port);     /* the swap is its own inverse */
    a.sin_addr = linux_be32(ip);
    for (k = 0; k < sizeof(a.sin_zero); k++) {
        a.sin_zero[k] = 0;
    }
    /* The one fallible step, and the callers' undo paths were written for it:
     * until now it could not fail, so they had never run. */
    return vibeos_uaccess_copy((void *)(uintptr_t)uptr, &a, sizeof(a)) == 0 ? 0 : -1;
}


/* The socket description a descriptor names, with a reference the caller puts;
 * 0 and *err set when the descriptor is not open or not a socket. */
static vibeos_file_t *linux_socket_get(uint64_t fd, long *err) {
    vibeos_file_t *f = linux_file_get(fd);

    if (!f) {
        *err = -VIBEOS_EBADF;
        return 0;
    }
    if (f->ops != &vibeos_fops_socket) {
        vibeos_file_put(f);
        *err = -VIBEOS_ENOTSOCK;
        return 0;
    }
    return f;
}

static long linux_sys_socket(uint64_t domain, uint64_t type) {
    vibeos_file_t *f;
    long err;
    int me, kind;

    if (domain != LINUX_AF_INET) {
        /* A family this kernel has no sockets of. Not EINVAL: a C library
         * asks for a local socket to reach a name-service daemon before it
         * gives up on a user or group name, and takes "no such family" as "no
         * daemon" and anything else as a failure - so getgrgid for a group
         * that is not in /etc/group failed with EINVAL, where it should have
         * found nothing (seven LTP tests stopped there). */
        return -VIBEOS_EAFNOSUPPORT;
    }
    if (!ks_net() || (me = ks_current()) < 0 || !ks_id(me)->is_user) {
        return -VIBEOS_EINVAL;
    }
    if ((type & 0xFFu) == LINUX_SOCK_STREAM) {
        kind = VIBEOS_INET_SOCK_TCP;
    } else if ((type & 0xFFu) == LINUX_SOCK_DGRAM) {
        kind = VIBEOS_INET_SOCK_UDP;
    } else {
        return -VIBEOS_EINVAL;
    }
    f = vibeos_sockfile_create(kind, ks_id(me)->tgid, &err);
    if (!f) {
        return err;
    }
    /* SOCK_CLOEXEC is O_CLOEXEC's bit, carried in the type. */
    return linux_fd_install(f, (type & VIBEOS_O_CLOEXEC) ? VIBEOS_FD_CLOEXEC : 0u, 0);
}

static long linux_sys_bind(uint64_t fd, uint64_t addr_uptr) {
    uint32_t ip;
    uint16_t port;
    long r;
    vibeos_file_t *f = linux_socket_get(fd, &r);

    if (!f) {
        return r;
    }
    r = (linux_read_sockaddr(addr_uptr, &ip, &port) != 0) ? -VIBEOS_EFAULT
                                                           : vibeos_sockfile_bind(f, port);
    vibeos_file_put(f);
    return r;
}

static long linux_sys_listen(uint64_t fd) {
    long r;
    vibeos_file_t *f = linux_socket_get(fd, &r);

    if (!f) {
        return r;
    }
    r = vibeos_sockfile_listen(f);
    vibeos_file_put(f);
    return r;
}

static long linux_sys_connect(uint64_t fd, uint64_t addr_uptr) {
    uint32_t ip;
    uint16_t port;
    long r;
    vibeos_file_t *f = linux_socket_get(fd, &r);

    if (!f) {
        return r;
    }
    r = (linux_read_sockaddr(addr_uptr, &ip, &port) != 0) ? -VIBEOS_EFAULT
                                                           : vibeos_sockfile_connect(f, ip, port);
    vibeos_file_put(f);
    return r;
}

static long linux_sys_accept(uint64_t fd, uint64_t addr_uptr) {
    vibeos_file_t *child = 0;
    uint32_t ip = 0;
    uint16_t port = 0;
    long r, nfd;
    vibeos_file_t *f = linux_socket_get(fd, &r);

    if (!f || ks_current() < 0) {
        if (f) {
            vibeos_file_put(f);
        }
        return f ? -VIBEOS_EBADF : r;
    }
    /* A bad peer-address pointer is refused by the row, before a connection is
     * consumed (M-032). */
    r = vibeos_sockfile_accept(f, ks_id(ks_current())->tgid, &child, &ip, &port);
    vibeos_file_put(f);
    if (r != 0) {
        return r;
    }
    nfd = linux_fd_install(child, 0, 0);
    if (nfd < 0) {
        return nfd;   /* the install released the child, which closed it */
    }
    if (linux_write_sockaddr(addr_uptr, ip, port) != 0) {
        /* The pointer went bad after the pre-check: undo the accept rather than
         * hand back a connection with no way to learn of it. */
        (void)linux_fd_close((uint64_t)nfd);
        return -VIBEOS_EFAULT;
    }
    return nfd;
}

static long linux_sys_sendto(uint64_t fd, uint64_t buf, uint64_t len, uint64_t addr_uptr) {
    uint32_t ip;
    uint16_t port;
    long r;
    vibeos_file_t *f = linux_socket_get(fd, &r);

    if (!f) {
        return r;
    }
    if (addr_uptr == 0u) {
        r = f->ops->write(f, buf, len);
    } else if (linux_read_sockaddr(addr_uptr, &ip, &port) != 0) {
        r = -VIBEOS_EFAULT;
    } else {
        r = vibeos_sockfile_sendto(f, buf, len, ip, port);
    }
    vibeos_file_put(f);
    return r;
}

static long linux_sys_recvfrom(uint64_t fd, uint64_t buf, uint64_t len, uint64_t addr_uptr) {
    uint32_t ip = 0;
    uint16_t port = 0;
    long r;
    vibeos_file_t *f = linux_socket_get(fd, &r);

    if (!f) {
        return r;
    }
    /* The buffer and the source-address pointer are refused by the row, before
     * the datagram is dequeued - or a bad pointer loses it with no error (M-032). */
    r = vibeos_sockfile_recvfrom(f, buf, len, &ip, &port);
    vibeos_file_put(f);
    if (r >= 0 && linux_write_sockaddr(addr_uptr, ip, port) != 0) {
        return -VIBEOS_EFAULT;
    }
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
