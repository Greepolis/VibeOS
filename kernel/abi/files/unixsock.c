/* Local sockets (docs/abi/ L5 steps 2 and 3): AF_UNIX's streams and datagrams,
 * and open file descriptions passed between processes with a message.
 *
 * An endpoint is a slot in one table, under one lock. A stream connection is two
 * endpoints, each with a ring of bytes the other writes into - a pipe each way -
 * and a listener keeps the server-side endpoints connect made until accept takes
 * them. A datagram endpoint's ring holds whole messages, each framed with its
 * length and the name of whoever sent it. The rings are a page each, from the
 * frame allocator, taken when the endpoint is made.
 *
 * A name is the personality's to make: a path is walked, checked and given a
 * socket node in the filesystem by the caller, which hands this file the
 * absolute path; this file finds the node again and remembers which node it is,
 * so a rename of the node keeps the socket reachable at the new name. A name in
 * the abstract namespace (path[0] is 0) lives only here.
 *
 * Rights (SCM_RIGHTS) are the references the sender's personality took to its
 * descriptions. They wait in the receiver's endpoint, tied to the position in
 * its ring where the message that carried them begins, and a read hands them
 * over with the first byte of that message - which is why a stream read stops
 * at such a position rather than run past it, as Linux's does. Nothing collects
 * a cycle of sockets passed through each other; Linux has a garbage collector
 * for that and this file does not.
 *
 * Every wait looks, gives up the core, and looks again, stopping for a signal and
 * honouring O_NONBLOCK and MSG_DONTWAIT, as the IP sockets' do (socket.c). */

#include "files_internal.h"

#define UX_MAX       32u
#define UX_BACKLOG   8u
#define UX_RING      4096u          /* one page */
#define UX_RIGHTS_Q  4u             /* messages with rights waiting at one endpoint */
#define UX_DGRAM_HDR 3u             /* [len:2][name length:1], then the name */

typedef struct {
    uint64_t pos;                   /* where in the ring the message begins */
    uint32_t n;
    vibeos_file_t *f[VIBEOS_MSG_RIGHTS_MAX];
} ux_rights_t;

typedef struct {
    uint8_t used;
    uint8_t type;                   /* VIBEOS_SOCK_* */
    uint8_t listening;
    uint8_t connected;              /* stream: a peer was made; dgram: a default peer is named */
    uint8_t peer_gone;              /* stream: the peer closed */
    uint8_t shut_rd, shut_wr;       /* this side's shutdown */
    uint8_t peer_shut_wr;           /* stream: the peer will send nothing more */
    uint8_t server;                 /* the server side of a connection: it answers to
                                     * its listener's name and is never found by it */
    uint32_t gen;
    int peer;                       /* stream peer, or dgram default peer */
    uint32_t peer_gen;
    vibeos_sockaddr_t name;         /* bound name; path_len 0 for none */
    vibeos_fsmount_t *mnt;          /* a path's node, which a connect finds */
    uint64_t node;
    uint8_t *ring;
    uint64_t rd, wr;                /* bytes read and written, ever: ring index is mod UX_RING */
    int queue[UX_BACKLOG];          /* listener: server endpoints connect made */
    uint32_t qlen, qmax;
    ux_rights_t rights[UX_RIGHTS_Q];
    uint32_t nrights;
    uint32_t cred_pid, cred_uid, cred_gid;        /* who made it, or who listened */
    uint32_t peer_pid, peer_uid, peer_gid;        /* SO_PEERCRED */
    uint8_t has_peer_cred;
} ux_t;

static ux_t g_ux[UX_MAX];
static vibeos_lock_t g_ux_lock;
static uint32_t g_ux_gen;
static uint32_t g_ux_autobind;

/* ---- the table ------------------------------------------------------------------- */

static void ux_who(uint32_t *pid, uint32_t *uid, uint32_t *gid) {
    int me = ks_current();
    vibeos_procstate_t *ps = me >= 0 ? ks_ps(me) : 0;

    *pid = me >= 0 ? ks_id(me)->tgid : 0u;
    *uid = ps ? ps->cred.euid : 0u;
    *gid = ps ? ps->cred.egid : 0u;
}

/* A new endpoint, its ring taken. Under the lock. -1 when the table is full or
 * no page is free. */
static int ux_alloc(int type) {
    uint32_t i;

    for (i = 0; i < UX_MAX; i++) {
        ux_t *u = &g_ux[i];
        uint8_t *ring;

        if (u->used) {
            continue;
        }
        if (!(ring = (uint8_t *)ks_page_alloc())) {
            return -1;
        }
        {
            uint32_t gen = ++g_ux_gen;
            uint8_t *p = (uint8_t *)u;
            uint32_t k;

            for (k = 0; k < sizeof(*u); k++) {
                p[k] = 0;
            }
            u->gen = gen;
        }
        u->used = 1;
        u->type = (uint8_t)type;
        u->peer = -1;
        u->ring = ring;
        ux_who(&u->cred_pid, &u->cred_uid, &u->cred_gid);
        return (int)i;
    }
    return -1;
}

/* The endpoint a description names, if it is still that one. Under the lock. */
static ux_t *ux_of(const vibeos_file_t *f) {
    if (f->ux < 0 || (uint32_t)f->ux >= UX_MAX || !g_ux[f->ux].used || g_ux[f->ux].gen != f->sock_gen) {
        return 0;
    }
    return &g_ux[f->ux];
}

static ux_t *ux_peer(const ux_t *u) {
    if (u->peer < 0 || !g_ux[u->peer].used || g_ux[u->peer].gen != u->peer_gen) {
        return 0;
    }
    return &g_ux[u->peer];
}

static uint32_t ux_free_room(const ux_t *u) {
    return UX_RING - (uint32_t)(u->wr - u->rd);
}

static void ux_put_bytes(ux_t *u, const uint8_t *src, uint32_t n) {
    uint32_t k;

    for (k = 0; k < n; k++) {
        u->ring[(u->wr + k) % UX_RING] = src[k];
    }
    u->wr += n;
}

static void ux_get_bytes(const ux_t *u, uint64_t at, uint8_t *dst, uint32_t n) {
    uint32_t k;

    for (k = 0; k < n; k++) {
        dst[k] = u->ring[(at + k) % UX_RING];
    }
}

/* Rights waiting at `u` that a message beginning at `pos` carried; -1 for none. */
static int ux_rights_at(const ux_t *u, uint64_t pos) {
    uint32_t i;

    for (i = 0; i < u->nrights; i++) {
        if (u->rights[i].pos == pos) {
            return (int)i;
        }
    }
    return -1;
}

/* The first position after `from` and before `limit` where rights wait. */
static uint64_t ux_next_rights(const ux_t *u, uint64_t from, uint64_t limit) {
    uint32_t i;

    for (i = 0; i < u->nrights; i++) {
        if (u->rights[i].pos > from && u->rights[i].pos < limit) {
            limit = u->rights[i].pos;
        }
    }
    return limit;
}

/* The rights a message carried, handed to the reader - all of them: the
 * personality installs what its caller has room for and gives the rest back,
 * outside this lock (a description's last reference may be another local
 * socket's, whose release takes it). */
static void ux_take_rights(ux_t *u, int at, vibeos_msg_t *m) {
    ux_rights_t *r = &u->rights[at];
    uint32_t i;

    for (i = 0; i < r->n; i++) {
        m->rights[m->nrights++] = r->f[i];
    }
    u->rights[at] = u->rights[u->nrights - 1u];
    u->nrights--;
}

/* An endpoint goes: its peer sees the end, a listener's unaccepted connections
 * go with it, rights in flight are given back. Under the lock; the references
 * it drops are collected into `drop` for the caller to put after it. */
static void ux_free(int idx, vibeos_file_t **drop, uint32_t *ndrop, uint32_t cap) {
    ux_t *u = &g_ux[idx];
    ux_t *p = ux_peer(u);
    uint32_t i, k;

    if (u->type == VIBEOS_SOCK_STREAM && p) {
        p->peer_gone = 1;
    }
    for (i = 0; i < u->qlen; i++) {
        int s = u->queue[i];

        if (s >= 0 && g_ux[s].used) {
            ux_free(s, drop, ndrop, cap);
        }
    }
    for (i = 0; i < u->nrights; i++) {
        for (k = 0; k < u->rights[i].n; k++) {
            if (*ndrop < cap) {
                drop[(*ndrop)++] = u->rights[i].f[k];
            }
        }
    }
    ks_page_free(u->ring, "unix socket ring");
    u->ring = 0;
    u->used = 0;
}

/* The endpoint bound to `a`, other than `self`: an abstract name compared byte
 * for byte, a path by the node it names. Under the lock. -1 for none. */
static int ux_find(const vibeos_sockaddr_t *a, vibeos_fsmount_t *mnt, uint64_t node, int self) {
    uint32_t i, k;

    for (i = 0; i < UX_MAX; i++) {
        const ux_t *u = &g_ux[i];

        if (!u->used || (int)i == self || u->name.path_len == 0u || u->server) {
            continue;
        }
        if (a->path_len != 0u && a->path[0] == 0) {
            if (u->name.path_len == a->path_len && u->name.path[0] == 0) {
                for (k = 0; k < a->path_len && u->name.path[k] == a->path[k]; k++) {
                }
                if (k == a->path_len) {
                    return (int)i;
                }
            }
        } else if (mnt && u->mnt == mnt && u->node == node) {
            return (int)i;
        }
    }
    return -1;
}

/* The node a path names, walked by the kernel for itself: the caller judged the
 * permissions. -ENOENT, -ECONNREFUSED for a name that is not a socket. */
static long ux_node(const vibeos_sockaddr_t *a, vibeos_fsmount_t **mnt, uint64_t *node) {
    char path[VIBEOS_SA_PATH_MAX + 1u];
    vibeos_path_t w;
    uint32_t k;
    int r;

    for (k = 0; k < a->path_len && k < VIBEOS_SA_PATH_MAX; k++) {
        path[k] = a->path[k];
    }
    path[k] = 0;
    r = vibeos_path_walk("/", "/", path, 0, &w);
    if (r != 0) {
        return r;
    }
    if ((w.node.mode & VIBEOS_S_IFMT) != VIBEOS_S_IFSOCK) {
        return -VIBEOS_ECONNREFUSED;
    }
    *mnt = w.mnt;
    *node = w.node.id;
    return 0;
}

static long ux_wait(const vibeos_file_t *f, uint32_t msg_flags, long cut) {
    if ((f->flags & VIBEOS_O_NONBLOCK) || (msg_flags & VIBEOS_MSG_DONTWAIT)) {
        return -VIBEOS_EAGAIN;
    }
    if (ks_current() >= 0 && ks_signal_interrupts(ks_current())) {
        return cut;
    }
    ks_idle();
    return 0;
}

/* ---- the calls ------------------------------------------------------------------ */

static long ux_bind(vibeos_file_t *f, const vibeos_sockaddr_t *a) {
    vibeos_fsmount_t *mnt = 0;
    uint64_t node = 0;
    long r = 0;
    ux_t *u;

    if (a->family != VIBEOS_SA_UNIX) {
        return -VIBEOS_EINVAL;
    }
    if (a->path_len != 0u && a->path[0] != 0 && (r = ux_node(a, &mnt, &node)) != 0) {
        return r;   /* the personality made the node; it has to be there */
    }
    ks_lock(&g_ux_lock, __func__);
    if (!(u = ux_of(f))) {
        r = -VIBEOS_EBADF;
    } else if (u->name.path_len != 0u) {
        r = -VIBEOS_EINVAL;   /* bound already */
    } else if (a->path_len == 0u) {
        /* No name: Linux's autobind, five hex digits in the abstract namespace. */
        uint32_t n = ++g_ux_autobind, k;
        const char *hex = "0123456789abcdef";

        u->name.family = VIBEOS_SA_UNIX;
        u->name.path[0] = 0;
        for (k = 0; k < 5u; k++) {
            u->name.path[1u + k] = hex[(n >> (4u * (4u - k))) & 0xFu];
        }
        u->name.path_len = 6;
    } else if (ux_find(a, mnt, node, f->ux) >= 0) {
        r = -VIBEOS_EADDRINUSE;
    } else {
        u->name = *a;
        u->mnt = mnt;
        u->node = node;
    }
    ks_unlock(&g_ux_lock);
    return r;
}

static long ux_listen(vibeos_file_t *f, int backlog) {
    long r = 0;
    ux_t *u;

    ks_lock(&g_ux_lock, __func__);
    if (!(u = ux_of(f))) {
        r = -VIBEOS_EBADF;
    } else if (u->type != VIBEOS_SOCK_STREAM) {
        r = -VIBEOS_EOPNOTSUPP;
    } else if (u->name.path_len == 0u || u->connected) {
        r = -VIBEOS_EINVAL;   /* Linux listens only where it can be found */
    } else {
        u->listening = 1;
        u->qmax = backlog <= 0 ? 1u : ((uint32_t)backlog > UX_BACKLOG ? UX_BACKLOG : (uint32_t)backlog);
        ux_who(&u->cred_pid, &u->cred_uid, &u->cred_gid);
    }
    ks_unlock(&g_ux_lock);
    return r;
}

static long ux_connect(vibeos_file_t *f, const vibeos_sockaddr_t *a) {
    vibeos_fsmount_t *mnt = 0;
    uint64_t node = 0;
    long r;

    if (a->family == VIBEOS_SA_NONE) {
        ks_lock(&g_ux_lock, __func__);
        {
            ux_t *u = ux_of(f);

            if (u && u->type == VIBEOS_SOCK_DGRAM) {
                u->connected = 0;   /* AF_UNSPEC: forget the default peer */
                u->peer = -1;
            }
        }
        ks_unlock(&g_ux_lock);
        return 0;
    }
    if (a->family != VIBEOS_SA_UNIX || a->path_len == 0u) {
        return -VIBEOS_EINVAL;
    }
    if (a->path[0] != 0 && (r = ux_node(a, &mnt, &node)) != 0) {
        return r;
    }
    for (;;) {
        ux_t *u, *l;
        int li, si;

        ks_lock(&g_ux_lock, __func__);
        if (!(u = ux_of(f))) {
            ks_unlock(&g_ux_lock);
            return -VIBEOS_EBADF;
        }
        li = ux_find(a, mnt, node, f->ux);
        l = li >= 0 ? &g_ux[li] : 0;
        if (u->type == VIBEOS_SOCK_DGRAM) {
            if (!l || l->type != VIBEOS_SOCK_DGRAM) {
                ks_unlock(&g_ux_lock);
                return -VIBEOS_ECONNREFUSED;
            }
            u->peer = li;
            u->peer_gen = l->gen;
            u->connected = 1;
            ks_unlock(&g_ux_lock);
            return 0;
        }
        if (u->connected || u->listening) {
            ks_unlock(&g_ux_lock);
            return u->connected ? -VIBEOS_EISCONN : -VIBEOS_EINVAL;
        }
        if (!l || !l->listening || l->type != VIBEOS_SOCK_STREAM) {
            ks_unlock(&g_ux_lock);
            return -VIBEOS_ECONNREFUSED;
        }
        if (l->qlen < l->qmax && (si = ux_alloc(VIBEOS_SOCK_STREAM)) >= 0) {
            ux_t *s = &g_ux[si];

            s->connected = 1;
            s->server = 1;
            s->peer = f->ux;
            s->peer_gen = u->gen;
            s->name = l->name;   /* the server side answers to the listener's name */
            s->mnt = 0;          /* but is not found by it */
            s->name.path_len = l->name.path_len;
            ux_who(&s->peer_pid, &s->peer_uid, &s->peer_gid);
            s->has_peer_cred = 1;
            s->cred_pid = l->cred_pid;
            s->cred_uid = l->cred_uid;
            s->cred_gid = l->cred_gid;
            u->connected = 1;
            u->peer = si;
            u->peer_gen = s->gen;
            u->peer_pid = l->cred_pid;
            u->peer_uid = l->cred_uid;
            u->peer_gid = l->cred_gid;
            u->has_peer_cred = 1;
            l->queue[l->qlen++] = si;
            ks_unlock(&g_ux_lock);
            ks_wake_waiters();
            return 0;
        }
        ks_unlock(&g_ux_lock);
        if ((r = ux_wait(f, 0, -VIBEOS_EINTR)) != 0) {
            return r;   /* a full backlog: EAGAIN for a socket that may not wait */
        }
    }
}

static long ux_accept(vibeos_file_t *f, uint32_t owner, vibeos_file_t **child, vibeos_sockaddr_t *peer) {
    long r;

    (void)owner;
    *child = 0;
    for (;;) {
        ux_t *u;

        ks_lock(&g_ux_lock, __func__);
        if (!(u = ux_of(f))) {
            ks_unlock(&g_ux_lock);
            return -VIBEOS_EBADF;
        }
        if (!u->listening) {
            ks_unlock(&g_ux_lock);
            return u->type == VIBEOS_SOCK_STREAM ? -VIBEOS_EINVAL : -VIBEOS_EOPNOTSUPP;
        }
        if (u->qlen != 0u) {
            int si = u->queue[0];
            uint32_t i;
            ux_t *s = &g_ux[si], *c;
            vibeos_file_t *nf;

            for (i = 1; i < u->qlen; i++) {
                u->queue[i - 1u] = u->queue[i];
            }
            u->qlen--;
            c = ux_peer(s);
            if (c && c->name.path_len != 0u) {
                *peer = c->name;
            } else {
                peer->family = VIBEOS_SA_UNIX;
                peer->path_len = 0;
            }
            ks_unlock(&g_ux_lock);
            nf = vibeos_file_alloc(&vibeos_fops_unix, VIBEOS_O_RDWR);
            ks_lock(&g_ux_lock, __func__);
            if (!nf) {
                vibeos_file_t *drop[UX_RIGHTS_Q * VIBEOS_MSG_RIGHTS_MAX];
                uint32_t nd = 0, k;

                ux_free(si, drop, &nd, sizeof(drop) / sizeof(drop[0]));
                ks_unlock(&g_ux_lock);
                for (k = 0; k < nd; k++) {
                    vibeos_file_put(drop[k]);
                }
                return -VIBEOS_ENFILE;
            }
            nf->ux = si;
            nf->sock_gen = s->gen;
            ks_unlock(&g_ux_lock);
            vibeos_sockopt_init(nf, VIBEOS_SOCK_STREAM, VIBEOS_SA_UNIX);
            *child = nf;
            return 0;
        }
        ks_unlock(&g_ux_lock);
        if ((r = ux_wait(f, 0, -VIBEOS_RESTART_CALL)) != 0) {
            return r;
        }
    }
}

static uint64_t ux_total(const vibeos_msg_t *m) {
    uint64_t t = 0;
    uint32_t i;

    for (i = 0; i < m->iovcnt; i++) {
        t += m->iov[i].len;
    }
    return t;
}

/* Bytes of the iovec from `skip` into or out of `buf`. 0, or -1. */
static int ux_iov(const vibeos_msg_t *m, uint64_t skip, uint8_t *buf, uint64_t n, int to_user) {
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

static long ux_epipe(uint32_t flags) {
    if (!(flags & VIBEOS_MSG_NOSIGNAL) && ks_current() >= 0) {
        (void)ks_signal_raise(ks_current(), VIBEOS_SIGPIPE);
    }
    return -VIBEOS_EPIPE;
}

/* Where staged bytes wait between user memory and a ring: copied in before the
 * lock (a copy may fault, and the lock masks the timer), into the ring under it.
 * One page, one at a time: a send copies at most this much per pass. */
static uint8_t g_ux_stage[UX_RING];
static vibeos_lock_t g_ux_stage_lock;

/* A send, in two shapes. A datagram goes whole or not at all: it waits until
 * its receiver's ring has room for the frame - length, sender's name, bytes -
 * and is refused outright if it could never fit. A stream sends what the
 * peer has room for and returns once anything went, as Linux's short send
 * does; it waits only for the first byte to fit. Either way the bytes are
 * staged out of user memory before the table's lock is taken (a copy may
 * fault, and the lock masks the timer), and rights ride with the first byte
 * of their message: the reader is handed them when it reaches that byte. */
static long ux_sendmsg(vibeos_file_t *f, vibeos_msg_t *m) {
    uint64_t total = ux_total(m), sent = 0;
    uint32_t give = m->nrights, i;
    long r;

    if (f->sk_type == VIBEOS_SOCK_DGRAM) {
        vibeos_fsmount_t *mnt = 0;
        uint64_t node = 0;

        /* The frame with the longest name a sender can have must fit the
         * ring, or this message could never be delivered and the wait below
         * would be for ever. */
        if (total + UX_DGRAM_HDR + VIBEOS_SA_PATH_MAX > UX_RING) {
            return -VIBEOS_EMSGSIZE;
        }
        if (m->addr && m->addr->family == VIBEOS_SA_UNIX && m->addr->path_len != 0u &&
            m->addr->path[0] != 0 && (r = ux_node(m->addr, &mnt, &node)) != 0) {
            return r;
        }
        for (;;) {
            ux_t *u, *t;
            int ti;
            uint8_t hdr[UX_DGRAM_HDR];

            ks_lock(&g_ux_stage_lock, __func__);
            if (ux_iov(m, 0, g_ux_stage, total, 0) != 0) {
                ks_unlock(&g_ux_stage_lock);
                return -VIBEOS_EFAULT;
            }
            ks_lock(&g_ux_lock, __func__);
            if (!(u = ux_of(f))) {
                r = -VIBEOS_EBADF;
                goto dgram_out;
            }
            /* The receiver: the name given, looked up again on every pass
             * because the socket bound to it can go while this waits; else
             * the connected peer. */
            if (m->addr && m->addr->family == VIBEOS_SA_UNIX && m->addr->path_len != 0u) {
                ti = ux_find(m->addr, mnt, node, -1);
            } else if (u->connected) {
                ti = ux_peer(u) ? u->peer : -1;
            } else {
                r = -VIBEOS_ENOTCONN;
                goto dgram_out;
            }
            t = ti >= 0 ? &g_ux[ti] : 0;
            if (!t || t->type != VIBEOS_SOCK_DGRAM) {
                r = -VIBEOS_ECONNREFUSED;
                goto dgram_out;
            }
            if (t->shut_rd) {
                r = -VIBEOS_EPIPE;
                goto dgram_out;
            }
            if (ux_free_room(t) >= total + UX_DGRAM_HDR + u->name.path_len &&
                (give == 0u || t->nrights < UX_RIGHTS_Q)) {
                if (give != 0u) {
                    ux_rights_t *rr = &t->rights[t->nrights++];

                    rr->pos = t->wr;
                    rr->n = give;
                    for (i = 0; i < give; i++) {
                        rr->f[i] = m->rights[i];
                    }
                    m->nrights = 0;   /* taken over */
                }
                hdr[0] = (uint8_t)(total >> 8);
                hdr[1] = (uint8_t)total;
                hdr[2] = (uint8_t)u->name.path_len;
                ux_put_bytes(t, hdr, UX_DGRAM_HDR);
                ux_put_bytes(t, (const uint8_t *)u->name.path, u->name.path_len);
                ux_put_bytes(t, g_ux_stage, (uint32_t)total);
                r = (long)total;
                goto dgram_out;
            }
            ks_unlock(&g_ux_lock);
            ks_unlock(&g_ux_stage_lock);
            if ((r = ux_wait(f, m->flags, -VIBEOS_RESTART_CALL)) != 0) {
                return r;
            }
            continue;
dgram_out:
            ks_unlock(&g_ux_lock);
            ks_unlock(&g_ux_stage_lock);
            if (r >= 0) {
                ks_wake_waiters();
            }
            return r;
        }
    }
    for (;;) {
        ux_t *u, *p;
        uint64_t chunk = total - sent;

        if (chunk > UX_RING) {
            chunk = UX_RING;
        }
        ks_lock(&g_ux_stage_lock, __func__);
        if (ux_iov(m, sent, g_ux_stage, chunk, 0) != 0) {
            ks_unlock(&g_ux_stage_lock);
            return sent ? (long)sent : -VIBEOS_EFAULT;
        }
        ks_lock(&g_ux_lock, __func__);
        if (!(u = ux_of(f))) {
            ks_unlock(&g_ux_lock);
            ks_unlock(&g_ux_stage_lock);
            return sent ? (long)sent : -VIBEOS_EBADF;
        }
        if (!u->connected) {
            ks_unlock(&g_ux_lock);
            ks_unlock(&g_ux_stage_lock);
            return -VIBEOS_ENOTCONN;
        }
        p = ux_peer(u);
        if (!p || u->peer_gone || u->shut_wr || p->shut_rd) {
            ks_unlock(&g_ux_lock);
            ks_unlock(&g_ux_stage_lock);
            return sent ? (long)sent : ux_epipe(m->flags);
        }
        {
            uint32_t room = ux_free_room(p);

            if (room != 0u && (give == 0u || p->nrights < UX_RIGHTS_Q)) {
                uint32_t n = chunk < room ? (uint32_t)chunk : room;

                if (give != 0u) {
                    ux_rights_t *rr = &p->rights[p->nrights++];

                    rr->pos = p->wr;
                    rr->n = give;
                    for (i = 0; i < give; i++) {
                        rr->f[i] = m->rights[i];
                    }
                    m->nrights = 0;
                    give = 0;
                }
                ux_put_bytes(p, g_ux_stage, n);
                sent += n;
                ks_unlock(&g_ux_lock);
                ks_unlock(&g_ux_stage_lock);
                ks_wake_waiters();
                if (sent >= total) {
                    return (long)sent;
                }
                continue;
            }
        }
        ks_unlock(&g_ux_lock);
        ks_unlock(&g_ux_stage_lock);
        if (total == 0u) {
            return 0;
        }
        if (sent != 0u) {
            return (long)sent;   /* a short send, which a stream allows */
        }
        if ((r = ux_wait(f, m->flags, -VIBEOS_RESTART_CALL)) != 0) {
            return r;
        }
    }
}

static long ux_recvmsg(vibeos_file_t *f, vibeos_msg_t *m) {
    uint64_t total = ux_total(m), got = 0;
    uint32_t asked = m->flags;
    int peek = (asked & VIBEOS_MSG_PEEK) != 0;
    long r;

    m->flags = 0;
    m->nrights = 0;
    for (;;) {
        ux_t *u;

        ks_lock(&g_ux_stage_lock, __func__);
        ks_lock(&g_ux_lock, __func__);
        if (!(u = ux_of(f))) {
            r = got ? (long)got : -VIBEOS_EBADF;
            goto out;
        }
        if (u->type == VIBEOS_SOCK_DGRAM) {
            if (u->wr != u->rd) {
                uint8_t hdr[UX_DGRAM_HDR];
                uint32_t len, nl, take;
                int ra;

                ux_get_bytes(u, u->rd, hdr, UX_DGRAM_HDR);
                len = ((uint32_t)hdr[0] << 8) | hdr[1];
                nl = hdr[2];
                take = len < total ? len : (uint32_t)total;
                if (m->addr) {
                    /* An unnamed sender is no name at all: Linux's length 0. */
                    m->addr->family = nl != 0u ? VIBEOS_SA_UNIX : VIBEOS_SA_NONE;
                    m->addr->path_len = (uint16_t)nl;
                    ux_get_bytes(u, u->rd + UX_DGRAM_HDR, (uint8_t *)m->addr->path, nl);
                }
                ux_get_bytes(u, u->rd + UX_DGRAM_HDR + nl, g_ux_stage, take);
                m->full_len = len;
                if (len > take) {
                    m->flags |= VIBEOS_MSG_TRUNC;
                }
                if ((ra = ux_rights_at(u, u->rd)) >= 0 && !peek) {
                    ux_take_rights(u, ra, m);
                }
                if (!peek) {
                    u->rd += UX_DGRAM_HDR + nl + len;
                }
                ks_unlock(&g_ux_lock);
                r = ux_iov(m, 0, g_ux_stage, take, 1) != 0 ? -VIBEOS_EFAULT
                                                           : ((asked & VIBEOS_MSG_TRUNC) ? (long)len : (long)take);
                ks_unlock(&g_ux_stage_lock);
                ks_wake_waiters();
                return r;
            }
            if (u->shut_rd) {
                r = 0;
                goto out;
            }
        } else {
            uint64_t avail = u->wr - u->rd, limit, want;
            int ra;

            if (!u->connected && !u->peer_gone) {
                r = got ? (long)got : -VIBEOS_EINVAL;   /* Linux: a stream nobody connected */
                goto out;
            }
            if (avail != 0u && total != 0u) {
                /* Not past a message that carried rights: they come with its
                 * first byte, and a read that started before it stops short. */
                limit = ux_next_rights(u, u->rd, u->wr);
                want = limit - u->rd;
                if (want > total - got) {
                    want = total - got;
                }
                if ((ra = ux_rights_at(u, u->rd)) >= 0 && !peek) {
                    if (got != 0u) {
                        r = (long)got;   /* deliver those with a read of their own */
                        goto out;
                    }
                    ux_take_rights(u, ra, m);
                }
                ux_get_bytes(u, u->rd, g_ux_stage, (uint32_t)want);
                if (!peek) {
                    u->rd += want;
                }
                ks_unlock(&g_ux_lock);
                if (ux_iov(m, got, g_ux_stage, want, 1) != 0) {
                    ks_unlock(&g_ux_stage_lock);
                    return got ? (long)got : -VIBEOS_EFAULT;
                }
                ks_unlock(&g_ux_stage_lock);
                ks_wake_waiters();
                got += want;
                if (!(asked & VIBEOS_MSG_WAITALL) || peek || got >= total || m->nrights != 0u) {
                    return (long)got;
                }
                continue;
            }
            if (total == 0u || u->peer_gone || u->peer_shut_wr || u->shut_rd) {
                r = (long)got;   /* the end of the stream */
                goto out;
            }
        }
        ks_unlock(&g_ux_lock);
        ks_unlock(&g_ux_stage_lock);
        if (got != 0u) {
            return (long)got;
        }
        if ((r = ux_wait(f, asked, -VIBEOS_RESTART_CALL)) != 0) {
            return r;
        }
        continue;
out:
        ks_unlock(&g_ux_lock);
        ks_unlock(&g_ux_stage_lock);
        return r;
    }
}

static long ux_shutdown(vibeos_file_t *f, int how) {
    long r = 0;
    ux_t *u, *p;

    ks_lock(&g_ux_lock, __func__);
    if (!(u = ux_of(f))) {
        r = -VIBEOS_EBADF;
    } else {
        p = ux_peer(u);
        if (how & VIBEOS_SHUT_RD) {
            u->shut_rd = 1;
        }
        if (how & VIBEOS_SHUT_WR) {
            u->shut_wr = 1;
            if (p && u->type == VIBEOS_SOCK_STREAM) {
                p->peer_shut_wr = 1;
            }
        }
    }
    ks_unlock(&g_ux_lock);
    ks_wake_waiters();
    return r;
}

static long ux_name(vibeos_file_t *f, int peer, vibeos_sockaddr_t *out) {
    long r = 0;
    ux_t *u, *p;

    ks_lock(&g_ux_lock, __func__);
    if (!(u = ux_of(f))) {
        r = -VIBEOS_EBADF;
    } else if (!peer) {
        *out = u->name;
        out->family = VIBEOS_SA_UNIX;
    } else if (!u->connected || !(p = ux_peer(u))) {
        r = -VIBEOS_ENOTCONN;
    } else {
        *out = p->name;
        out->family = VIBEOS_SA_UNIX;
    }
    ks_unlock(&g_ux_lock);
    return r;
}

static long ux_setopt(vibeos_file_t *f, int opt, const void *val, uint32_t len) {
    return vibeos_sockopt_set(f, opt, val, len);
}

static long ux_getopt(vibeos_file_t *f, int opt, void *val, uint32_t *len) {
    ux_t *u;

    if (opt == VIBEOS_SO_PEERCRED) {
        uint32_t *c = (uint32_t *)val;

        if (*len < 3u * sizeof(uint32_t)) {
            return -VIBEOS_EINVAL;
        }
        ks_lock(&g_ux_lock, __func__);
        u = ux_of(f);
        /* Nobody at the other end: Linux's pid 0, uid and gid -1. */
        c[0] = (u && u->has_peer_cred) ? u->peer_pid : 0u;
        c[1] = (u && u->has_peer_cred) ? u->peer_uid : 0xFFFFFFFFu;
        c[2] = (u && u->has_peer_cred) ? u->peer_gid : 0xFFFFFFFFu;
        ks_unlock(&g_ux_lock);
        *len = 3u * sizeof(uint32_t);
        return 0;
    }
    if (opt == VIBEOS_SO_ACCEPTCONN || opt == VIBEOS_SO_NREAD) {
        int32_t v = 0;

        if (*len < sizeof(v)) {
            return -VIBEOS_EINVAL;
        }
        ks_lock(&g_ux_lock, __func__);
        u = ux_of(f);
        if (u && opt == VIBEOS_SO_ACCEPTCONN) {
            v = u->listening;
        } else if (u && u->type == VIBEOS_SOCK_DGRAM && u->wr != u->rd) {
            uint8_t hdr[UX_DGRAM_HDR];

            ux_get_bytes(u, u->rd, hdr, UX_DGRAM_HDR);
            v = (int32_t)(((uint32_t)hdr[0] << 8) | hdr[1]);
        } else if (u) {
            v = (int32_t)(u->wr - u->rd);
        }
        ks_unlock(&g_ux_lock);
        *(int32_t *)val = v;
        *len = sizeof(v);
        return 0;
    }
    return vibeos_sockopt_get(f, opt, val, len);
}

static const vibeos_sock_ops_t g_unix_sockops = {
    .bind = ux_bind,
    .listen = ux_listen,
    .connect = ux_connect,
    .accept = ux_accept,
    .sendmsg = ux_sendmsg,
    .recvmsg = ux_recvmsg,
    .shutdown = ux_shutdown,
    .name = ux_name,
    .setopt = ux_setopt,
    .getopt = ux_getopt,
};

/* ---- the file type ------------------------------------------------------------------ */

static long ux_read(vibeos_file_t *f, uint64_t buf, uint64_t len) {
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
    return ux_recvmsg(f, &m);
}

static long ux_write(vibeos_file_t *f, uint64_t buf, uint64_t len) {
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
    return ux_sendmsg(f, &m);
}

static int ux_stat(vibeos_file_t *f, vibeos_file_stat_t *out) {
    out->mode = VIBEOS_S_IFSOCK | 0777u;
    out->size = 0;
    out->ino = 0x30000u + (uint64_t)(uint32_t)f->ux;
    return 0;
}

/* What poll may say: data or the end to read, a connection to accept, room at
 * the peer to write into; the peer's going is a hangup, its shutdown RDHUP. */
static uint32_t ux_ready(vibeos_file_t *f) {
    uint32_t r = 0;
    ux_t *u, *p;

    ks_lock(&g_ux_lock, __func__);
    if (!(u = ux_of(f))) {
        ks_unlock(&g_ux_lock);
        return VIBEOS_READY_HUP;
    }
    if (u->listening) {
        r = u->qlen != 0u ? VIBEOS_READY_IN : 0u;
    } else if (u->type == VIBEOS_SOCK_DGRAM) {
        r |= u->wr != u->rd ? VIBEOS_READY_IN : 0u;
        p = u->connected ? ux_peer(u) : 0;
        r |= (!u->connected || (p && ux_free_room(p) > UX_DGRAM_HDR + VIBEOS_SA_PATH_MAX)) ? VIBEOS_READY_OUT : 0u;
    } else if (!u->connected && !u->peer_gone) {
        r = VIBEOS_READY_OUT | VIBEOS_READY_HUP;   /* never connected */
    } else {
        p = ux_peer(u);
        if (u->wr != u->rd || u->peer_gone || u->peer_shut_wr || u->shut_rd) {
            r |= VIBEOS_READY_IN;
        }
        if (u->peer_shut_wr || u->peer_gone) {
            r |= VIBEOS_READY_RDHUP;
        }
        if (u->peer_gone || (u->shut_wr && u->peer_shut_wr)) {
            r |= VIBEOS_READY_HUP;
        }
        if (u->peer_gone || (p && ux_free_room(p) != 0u)) {
            r |= VIBEOS_READY_OUT;
        }
    }
    ks_unlock(&g_ux_lock);
    return r;
}

static void ux_release(vibeos_file_t *f) {
    vibeos_file_t *drop[(UX_BACKLOG + 1u) * UX_RIGHTS_Q * VIBEOS_MSG_RIGHTS_MAX];
    uint32_t nd = 0, k;

    ks_lock(&g_ux_lock, __func__);
    if (ux_of(f)) {
        ux_free(f->ux, drop, &nd, sizeof(drop) / sizeof(drop[0]));
    }
    ks_unlock(&g_ux_lock);
    /* Given back outside the lock: a description's last reference runs its own
     * release, which may be another local socket's and take this lock. */
    for (k = 0; k < nd; k++) {
        vibeos_file_put(drop[k]);
    }
    f->ux = -1;
    ks_wake_waiters();
}

const vibeos_file_ops_t vibeos_fops_unix = {
    .name = "unix",
    .read = ux_read,
    .write = ux_write,
    .stat = ux_stat,
    .ready = ux_ready,
    .release = ux_release,
    .sockops = &g_unix_sockops,
};

/* A description for endpoint `idx`. Under the lock; 0 if none is free. */
static vibeos_file_t *ux_open(int idx, int type, uint32_t flags) {
    vibeos_file_t *f = vibeos_file_alloc(&vibeos_fops_unix, VIBEOS_O_RDWR | flags);

    if (f) {
        f->ux = idx;
        f->sock_gen = g_ux[idx].gen;
        vibeos_sockopt_init(f, type, VIBEOS_SA_UNIX);
    }
    return f;
}

vibeos_file_t *vibeos_unix_create(int type, uint32_t flags, long *err) {
    vibeos_file_t *f;
    int idx;

    ks_lock(&g_ux_lock, __func__);
    idx = ux_alloc(type);
    ks_unlock(&g_ux_lock);
    if (idx < 0) {
        *err = -VIBEOS_ENOBUFS;
        return 0;
    }
    if (!(f = ux_open(idx, type, flags))) {
        vibeos_file_t *drop[1];
        uint32_t nd = 0;

        ks_lock(&g_ux_lock, __func__);
        ux_free(idx, drop, &nd, 1);
        ks_unlock(&g_ux_lock);
        *err = -VIBEOS_ENFILE;
        return 0;
    }
    *err = 0;
    return f;
}

long vibeos_unix_pair(int type, uint32_t flags, vibeos_file_t **a, vibeos_file_t **b) {
    long err;
    ux_t *ua, *ub;

    if (!(*a = vibeos_unix_create(type, flags, &err))) {
        return err;
    }
    if (!(*b = vibeos_unix_create(type, flags, &err))) {
        vibeos_file_put(*a);
        *a = 0;
        return err;
    }
    ks_lock(&g_ux_lock, __func__);
    ua = ux_of(*a);
    ub = ux_of(*b);
    if (!ua || !ub) {
        /* Not reachable while the caller holds both new descriptions, which
         * it does; asked anyway, as every other caller of ux_of asks. */
        ks_unlock(&g_ux_lock);
        vibeos_file_put(*a);
        vibeos_file_put(*b);
        *a = *b = 0;
        return -VIBEOS_EBADF;
    }
    ua->peer = (*b)->ux;
    ua->peer_gen = ub->gen;
    ub->peer = (*a)->ux;
    ub->peer_gen = ua->gen;
    ua->connected = ub->connected = 1;
    /* Both ends are the caller's: each one's peer is the caller. */
    ua->peer_pid = ub->peer_pid = ua->cred_pid;
    ua->peer_uid = ub->peer_uid = ua->cred_uid;
    ua->peer_gid = ub->peer_gid = ua->cred_gid;
    ua->has_peer_cred = ub->has_peer_cred = 1;
    ks_unlock(&g_ux_lock);
    return 0;
}

/* For the tests: every endpoint gone, as at boot. */
void vibeos_unix_reset(void) {
    uint32_t i;

    for (i = 0; i < UX_MAX; i++) {
        if (g_ux[i].used && g_ux[i].ring) {
            ks_page_free(g_ux[i].ring, "unix socket reset");
        }
        g_ux[i].used = 0;
        g_ux[i].ring = 0;
    }
}
