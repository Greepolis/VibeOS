#ifndef VIBEOS_SOCKOPS_H
#define VIBEOS_SOCKOPS_H

/* What a socket can be asked, in no personality's words (docs/abi/ L5).
 *
 * Every socket type - the stack's IP sockets (kernel/abi/files/socket.c), the
 * local ones (unixsock.c) - hangs a table of these off its file type
 * (vibeos_file_ops_t.sock), and a personality's socket calls translate their
 * own layouts into these and call through the table. So the Linux handlers do
 * not know which family they hold, and a Windows personality's winsock calls
 * would make the same calls. Addresses, iovecs and flags here are the kernel's
 * own; sockaddr_in and struct msghdr are the personality's business.
 *
 * Every entry returns 0 or a byte count, or a negated errno (vibeos/abi_linux.h's
 * numbering, which is the kernel's). User memory appears only as the iovec's
 * addresses, which the type copies through vibeos_uaccess_copy. */

#include <stdint.h>

typedef struct vibeos_file vibeos_file_t;

/* Families. */
#define VIBEOS_SA_NONE 0u
#define VIBEOS_SA_INET 1u
#define VIBEOS_SA_UNIX 2u
#define VIBEOS_SA_PATH_MAX 108u

/* An address. A local one is a path of path_len bytes, or - when path[0] is 0 -
 * a name in the abstract namespace, whose bytes after the 0 are the name; an
 * unnamed local socket has path_len 0. */
typedef struct {
    uint32_t family;
    uint32_t ip;          /* host order */
    uint16_t port;
    uint16_t path_len;
    char path[VIBEOS_SA_PATH_MAX];
} vibeos_sockaddr_t;

/* A piece of user memory. */
typedef struct {
    uint64_t base;
    uint64_t len;
} vibeos_uiov_t;

/* A message's flags: asked for on the way in, reported on the way out. */
#define VIBEOS_MSG_PEEK     0x01u   /* leave what was read where it was          */
#define VIBEOS_MSG_DONTWAIT 0x02u   /* this call does not wait                   */
#define VIBEOS_MSG_NOSIGNAL 0x04u   /* a broken connection is EPIPE and no signal */
#define VIBEOS_MSG_WAITALL  0x08u   /* a stream read waits for the whole length   */
#define VIBEOS_MSG_TRUNC    0x10u   /* in: say a datagram's whole length; out: it was cut */
#define VIBEOS_MSG_CTRUNC   0x20u   /* out: rights were dropped for want of room  */

/* Open file descriptions carried with a message (SCM_RIGHTS): on a send the
 * caller's references, which the type takes over; on a receive references the
 * type hands to the caller, at most `max_rights` of them. */
#define VIBEOS_MSG_RIGHTS_MAX 16u

typedef struct {
    vibeos_sockaddr_t *addr;      /* send: where to, 0 for the peer; receive: whence, may be 0 */
    const vibeos_uiov_t *iov;
    uint32_t iovcnt;
    uint32_t flags;               /* VIBEOS_MSG_*: asked on the way in, told on the way out */
    vibeos_file_t *rights[VIBEOS_MSG_RIGHTS_MAX];
    uint32_t nrights;
    uint32_t max_rights;          /* receive: room the caller has */
    uint64_t full_len;            /* receive: a datagram's whole length */
} vibeos_msg_t;

/* How a socket is shut. */
#define VIBEOS_SHUT_RD 1
#define VIBEOS_SHUT_WR 2

/* Options, as the kernel names them. Values are int unless said. */
#define VIBEOS_SO_TYPE       1    /* VIBEOS_SOCK_STREAM or _DGRAM, read only        */
#define VIBEOS_SO_ERROR      2    /* the pending error, cleared by reading it        */
#define VIBEOS_SO_ACCEPTCONN 3    /* listening, read only                           */
#define VIBEOS_SO_RCVBUF     4
#define VIBEOS_SO_SNDBUF     5
#define VIBEOS_SO_REUSEADDR  6
#define VIBEOS_SO_KEEPALIVE  7
#define VIBEOS_SO_BROADCAST  8
#define VIBEOS_SO_LINGER     9    /* two ints: on, seconds                          */
#define VIBEOS_SO_RCVTIMEO   10   /* uint64 ticks, 0 for ever                       */
#define VIBEOS_SO_SNDTIMEO   11
#define VIBEOS_SO_DOMAIN     12   /* VIBEOS_SA_*, read only                         */
#define VIBEOS_SO_PEERCRED   13   /* three uint32: pid, uid, gid of the peer        */
#define VIBEOS_SO_OOBINLINE  14
#define VIBEOS_SO_DONTROUTE  15
#define VIBEOS_TCP_NODELAY   16
#define VIBEOS_SO_PASSCRED   17

#define VIBEOS_SO_NREAD      18   /* bytes a read would return now, read only       */
#define VIBEOS_SO_REUSEPORT  19
#define VIBEOS_SO_DEBUG      20

#define VIBEOS_SOCK_STREAM 1
#define VIBEOS_SOCK_DGRAM  2

/* vibeos_file_t.sk_flags: the on-off options. */
#define VIBEOS_SKF_REUSEADDR 0x01u
#define VIBEOS_SKF_KEEPALIVE 0x02u
#define VIBEOS_SKF_BROADCAST 0x04u
#define VIBEOS_SKF_OOBINLINE 0x08u
#define VIBEOS_SKF_DONTROUTE 0x10u
#define VIBEOS_SKF_NODELAY   0x20u
#define VIBEOS_SKF_LINGER    0x40u
#define VIBEOS_SKF_PASSCRED  0x80u
#define VIBEOS_SKF_REUSEPORT 0x100u
#define VIBEOS_SKF_DEBUG     0x200u

/* A socket description's options as a new one has them. */
void vibeos_sockopt_init(vibeos_file_t *f, int type, uint32_t family);

typedef struct vibeos_sock_ops {
    long (*bind)(vibeos_file_t *f, const vibeos_sockaddr_t *a);
    long (*listen)(vibeos_file_t *f, int backlog);
    long (*connect)(vibeos_file_t *f, const vibeos_sockaddr_t *a);
    /* A new description for the next connection, owned by process `owner`,
     * and who it is from. */
    long (*accept)(vibeos_file_t *f, uint32_t owner, vibeos_file_t **child, vibeos_sockaddr_t *peer);
    long (*sendmsg)(vibeos_file_t *f, vibeos_msg_t *m);
    long (*recvmsg)(vibeos_file_t *f, vibeos_msg_t *m);
    long (*shutdown)(vibeos_file_t *f, int how);
    /* Where it is (peer 0) or who is at the other end (peer 1): -ENOTCONN for
     * a peer it does not have. */
    long (*name)(vibeos_file_t *f, int peer, vibeos_sockaddr_t *out);
    /* An option the type has a say in; -ENOPROTOOPT for one it leaves to the
     * common ones (vibeos_sockopt_set / _get). Values in kernel memory. */
    long (*setopt)(vibeos_file_t *f, int opt, const void *val, uint32_t len);
    long (*getopt)(vibeos_file_t *f, int opt, void *val, uint32_t *len);
} vibeos_sock_ops_t;

/* The options every socket has the same way: kept in the description. */
long vibeos_sockopt_set(vibeos_file_t *f, int opt, const void *val, uint32_t len);
long vibeos_sockopt_get(vibeos_file_t *f, int opt, void *val, uint32_t *len);

#endif
