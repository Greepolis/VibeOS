/* System V IPC (docs/abi/ L7): what the three kinds share, and message queues.
 *
 * An object is a number a program was given and a key it may be found by.
 * The number is Linux's: an index and a sequence count, `seq << 15 | index`,
 * so that a queue removed and made again gets a different number and an old
 * one names nothing. The index is handed out cyclically, as Linux's idr has
 * done since 5.4, and the sequence moves on each time the cycle starts again.
 * A program that sets msg_next_id (CRIU does, and so does LTP) asks for a
 * number, and gets the next free index at or above it.
 *
 * The index is not where the object is stored. Linux's numbers go to 32767 and
 * this kernel keeps 32 objects of each kind; a slot remembers the number it was
 * given. msgmni, semmni and shmmni say the true limit - 32 unless a program
 * lowers it - because a program that reads the limit and makes that many
 * expects the last one to succeed (LTP's msgget03, shmget03).
 *
 * One lock for all of it, and nothing slow under it: the text of a message is
 * copied between user memory and a kernel page outside the lock, never into
 * the queue from a user address. A copy that faults while holding a spin lock
 * would be a page-in with interrupts off.
 *
 * Permissions are Linux's ipcperms: the owner's bits if the caller's
 * effective id is the owner or the creator, the group's if the caller is in
 * either group, otherwise everybody's; the superuser passes. Changing or
 * removing an object belongs to its owner, its creator and the superuser. */

#include "linux_internal.h"
#include "vibeos/cred.h"

static vibeos_lock_t g_ipc_lock;

void linux_ipc_lock(void) {
    ks_lock(&g_ipc_lock, __func__);
}

void linux_ipc_unlock(void) {
    ks_unlock(&g_ipc_lock);
}

int64_t linux_ipc_now(void) {
    return (int64_t)(ks_ticks() / ks_hz());   /* what time() says (misc.c) */
}

/* ---- what every kind shares ---------------------------------------------------- */

/* A control call's structure, in or out. Which one, and whether it is read or
 * written, depends on the command - more forms than a row has descriptors for
 * - and Linux judges it at different moments for different commands: IPC_SET
 * reads it before looking for the object, the STAT forms write it after. So
 * the handler judges it, here, for every kind. 0, or -1 for EFAULT. */
int linux_ipc_out(uint64_t uptr, const void *src, uint32_t len) {
    if (!linux_user_ok(uptr, len, 1)) {
        return -1;
    }
    return vibeos_uaccess_copy((void *)(uintptr_t)uptr, src, len) != 0 ? -1 : 0;
}

int linux_ipc_in(void *dst, uint64_t uptr, uint32_t len) {
    if (!linux_user_ok(uptr, len, 0)) {
        return -1;
    }
    return vibeos_uaccess_copy(dst, (const void *)(uintptr_t)uptr, len) != 0 ? -1 : 0;
}

/* The lowest index in [from, end) that no object of the kind holds; -1 if
 * there is none. Under the lock. */
static int ipc_free_index(const linux_ipc_obj_t *tab, uint32_t n, uint32_t from, uint32_t end) {
    uint32_t idx, i;

    for (idx = from; idx < end; idx++) {
        for (i = 0; i < n; i++) {
            if (tab[i].used && (tab[i].id & (LINUX_IPC_INDEXES - 1u)) == idx) {
                break;
            }
        }
        if (i == n) {
            return (int)idx;
        }
    }
    return -1;
}

int linux_ipc_lookup(const linux_ipc_obj_t *tab, uint32_t n, int32_t key) {
    uint32_t i;

    if (key == LINUX_IPC_PRIVATE) {
        return -1;   /* IPC_PRIVATE is never found: every one is new */
    }
    for (i = 0; i < n; i++) {
        if (tab[i].used && tab[i].key == key) {
            return (int)i;
        }
    }
    return -1;
}

long linux_ipc_new(linux_ipc_ns_t *ns, linux_ipc_obj_t *tab, uint32_t n, int32_t key, uint64_t flg,
                   const vibeos_cred_t *c) {
    uint32_t i, live = 0, limit = ns->max < n ? ns->max : n;
    int slot = -1, idx;
    uint32_t seq;

    for (i = 0; i < n; i++) {
        if (tab[i].used) {
            live++;
        } else if (slot < 0) {
            slot = (int)i;
        }
    }
    if (slot < 0 || live >= limit) {
        return -VIBEOS_ENOSPC;
    }
    if (ns->next_id >= 0) {
        idx = ipc_free_index(tab, n, (uint32_t)ns->next_id & (LINUX_IPC_INDEXES - 1u), LINUX_IPC_INDEXES);
        seq = (uint32_t)ns->next_id >> LINUX_IPC_SEQ_SHIFT;
        ns->next_id = -1;   /* one object, as Linux: the setting is used up */
        if (idx < 0) {
            return -VIBEOS_ENOSPC;
        }
    } else {
        /* Cyclic, as Linux has handed them out since 5.4: the next index after
         * the last one given, over a cycle of at least 64, and the sequence
         * moves on only when the cycle starts again. So a queue removed and
         * made again gets the next number, not its old one (1 2 3, not 1 2 1). */
        idx = ipc_free_index(tab, n, (uint32_t)(ns->last_idx + 1), LINUX_IPC_CYCLE);
        if (idx < 0) {
            idx = ipc_free_index(tab, n, 0, LINUX_IPC_CYCLE);
        }
        if (idx < 0) {
            return -VIBEOS_ENOSPC;
        }
        if (idx <= ns->last_idx) {
            ns->seq = (ns->seq + 1u) % LINUX_IPC_SEQ_MAX;
        }
        ns->last_idx = idx;
        seq = ns->seq;
    }
    tab[slot].used = 1;
    tab[slot].id = (seq << LINUX_IPC_SEQ_SHIFT) | (uint32_t)idx;
    tab[slot].key = key;
    tab[slot].uid = tab[slot].cuid = c->euid;
    tab[slot].gid = tab[slot].cgid = c->egid;
    tab[slot].mode = (uint32_t)flg & 0777u;
    tab[slot].ctime = linux_ipc_now();
    return slot;
}

int linux_ipc_slot(const linux_ipc_obj_t *tab, uint32_t n, uint64_t id) {
    uint32_t i;

    if ((int64_t)(int32_t)(uint32_t)id < 0) {
        return -VIBEOS_EINVAL;
    }
    for (i = 0; i < n; i++) {
        if (tab[i].used && tab[i].id == (uint32_t)id) {
            return (int)i;
        }
    }
    return -VIBEOS_EINVAL;
}

int linux_ipc_index(const linux_ipc_obj_t *tab, uint32_t n, uint64_t index) {
    uint32_t i;

    for (i = 0; i < n; i++) {
        if (tab[i].used &&
            (tab[i].id & (LINUX_IPC_INDEXES - 1u)) == (uint32_t)index) {
            return (int)i;
        }
    }
    return -VIBEOS_EINVAL;
}

long linux_ipc_may(const linux_ipc_obj_t *o, uint32_t flg, const vibeos_cred_t *c) {
    uint32_t requested = ((flg >> 6) | (flg >> 3) | flg) & 7u;
    uint32_t granted = o->mode;

    if (c->euid == o->uid || c->euid == o->cuid) {
        granted >>= 6;
    } else if (vibeos_cred_in_group(c, o->gid, 0) || vibeos_cred_in_group(c, o->cgid, 0)) {
        granted >>= 3;
    }
    if ((requested & ~granted & 7u) != 0u && c->euid != 0u) {
        return -VIBEOS_EACCES;
    }
    return 0;
}

long linux_ipc_owner(const linux_ipc_obj_t *o, const vibeos_cred_t *c) {
    return (c->euid == 0u || c->euid == o->uid || c->euid == o->cuid) ? 0 : -VIBEOS_EPERM;
}

void linux_ipc_perm_out(const linux_ipc_obj_t *o, linux_ipc64_perm_t *p) {
    p->key = o->key;
    p->uid = o->uid;
    p->gid = o->gid;
    p->cuid = o->cuid;
    p->cgid = o->cgid;
    p->mode = o->mode;
    p->seq = (uint16_t)(o->id >> LINUX_IPC_SEQ_SHIFT);
    p->pad2 = 0;
    p->unused1 = p->unused2 = 0;
}

/* IPC_SET's part every kind shares: the owner, the group and the nine
 * permission bits - the bits above them are the kernel's (SHM_DEST, SHM_LOCKED)
 * and stay. */
void linux_ipc_set(linux_ipc_obj_t *o, const linux_ipc64_perm_t *p) {
    o->uid = p->uid;
    o->gid = p->gid;
    o->mode = (o->mode & ~0777u) | (p->mode & 0777u);
    o->ctime = linux_ipc_now();
}

uint32_t linux_ipc_live(const linux_ipc_obj_t *tab, uint32_t n) {
    uint32_t i, live = 0;

    for (i = 0; i < n; i++) {
        live += tab[i].used;
    }
    return live;
}

/* What IPC_INFO and the *_INFO commands return: the highest index in use, or
 * 0 when there is none - which is also what one object at index 0 says, as on
 * Linux. */
long linux_ipc_max_index(const linux_ipc_obj_t *tab, uint32_t n) {
    uint32_t i, top = 0;

    for (i = 0; i < n; i++) {
        if (tab[i].used && (tab[i].id & (LINUX_IPC_INDEXES - 1u)) > top) {
            top = tab[i].id & (LINUX_IPC_INDEXES - 1u);
        }
    }
    return (long)top;
}

/* ---- message queues ------------------------------------------------------------- */

/* A queue's text, in arrival order, packed; and one record a message, saying its
 * type and length. A message taken from the middle closes the gap. Sixteen
 * kilobytes is Linux's default msgmnb, and its whole: msg_qbytes may be lowered,
 * not raised past what is stored. */
#define LINUX_MSG_RECORDS 256u

typedef struct {
    int64_t type;
    uint32_t len;
} linux_msg_rec_t;

typedef struct {
    uint64_t qbytes;
    uint64_t cbytes;
    uint32_t qnum;
    int64_t stime, rtime;
    uint32_t lspid, lrpid;
    linux_msg_rec_t rec[LINUX_MSG_RECORDS];
    uint8_t text[LINUX_MSGMNB];
} linux_msgq_t;

static linux_ipc_obj_t g_msg_obj[LINUX_IPC_SLOTS];
static linux_msgq_t g_msgq[LINUX_IPC_SLOTS];
static linux_ipc_ns_t g_msg_ns = {0, -1, LINUX_IPC_SLOTS, -1};

static long linux_sys_msgget(uint64_t key, uint64_t flg) {
    vibeos_cred_t c;
    int s;
    long r;

    linux_cred(&c);
    linux_ipc_lock();
    s = linux_ipc_lookup(g_msg_obj, LINUX_IPC_SLOTS, (int32_t)(uint32_t)key);
    if (s >= 0) {
        if ((flg & LINUX_IPC_CREAT) && (flg & LINUX_IPC_EXCL)) {
            r = -VIBEOS_EEXIST;
        } else {
            r = linux_ipc_may(&g_msg_obj[s], (uint32_t)flg, &c);
            if (r == 0) {
                r = (long)g_msg_obj[s].id;
            }
        }
    } else if (!(flg & LINUX_IPC_CREAT) && (int32_t)(uint32_t)key != LINUX_IPC_PRIVATE) {
        r = -VIBEOS_ENOENT;
    } else {
        r = linux_ipc_new(&g_msg_ns, g_msg_obj, LINUX_IPC_SLOTS, (int32_t)(uint32_t)key, flg, &c);
        if (r >= 0) {
            linux_msgq_t *q = &g_msgq[r];

            q->qbytes = LINUX_MSGMNB;
            q->cbytes = 0;
            q->qnum = 0;
            q->stime = q->rtime = 0;
            q->lspid = q->lrpid = 0;
            r = (long)g_msg_obj[r].id;
        }
    }
    linux_ipc_unlock();
    return r;
}

/* The two pages a message's text passes through on its way in or out. */
typedef struct {
    uint8_t *page[2];
} linux_msg_bounce_t;

static int msg_bounce_get(linux_msg_bounce_t *b) {
    b->page[0] = (uint8_t *)ks_page_alloc();
    b->page[1] = b->page[0] ? (uint8_t *)ks_page_alloc() : 0;
    if (!b->page[1]) {
        if (b->page[0]) {
            ks_page_free(b->page[0], "msg bounce");
        }
        return -VIBEOS_ENOMEM;
    }
    return 0;
}

static void msg_bounce_put(linux_msg_bounce_t *b) {
    ks_page_free(b->page[0], "msg bounce");
    ks_page_free(b->page[1], "msg bounce");
}

/* Between the bounce and a buffer: a user address when `user`, else the queue's
 * text. 0, or -1 if the user side faulted. */
static int msg_copy_in(linux_msg_bounce_t *b, uint64_t src, uint32_t len) {
    uint32_t first = len > 4096u ? 4096u : len;

    if (vibeos_uaccess_copy(b->page[0], (const void *)(uintptr_t)src, first) != 0) {
        return -1;
    }
    return len > first ? vibeos_uaccess_copy(b->page[1], (const void *)(uintptr_t)(src + first),
                                             len - first) : 0;
}

static int msg_copy_out(uint64_t dst, linux_msg_bounce_t *b, uint32_t len) {
    uint32_t first = len > 4096u ? 4096u : len;

    if (vibeos_uaccess_copy((void *)(uintptr_t)dst, b->page[0], first) != 0) {
        return -1;
    }
    return len > first ? vibeos_uaccess_copy((void *)(uintptr_t)(dst + first), b->page[1],
                                             len - first) : 0;
}

static void msg_to_queue(uint8_t *dst, const linux_msg_bounce_t *b, uint32_t len) {
    uint32_t i;

    for (i = 0; i < len; i++) {
        dst[i] = b->page[i >> 12][i & 4095u];
    }
}

static void msg_from_queue(linux_msg_bounce_t *b, const uint8_t *src, uint32_t len) {
    uint32_t i;

    for (i = 0; i < len; i++) {
        b->page[i >> 12][i & 4095u] = src[i];
    }
}

/* Where message `k`'s text starts. */
static uint32_t msg_offset(const linux_msgq_t *q, uint32_t k) {
    uint32_t i, off = 0;

    for (i = 0; i < k; i++) {
        off += q->rec[i].len;
    }
    return off;
}

typedef struct {
    uint64_t id;
    int64_t type;
    uint32_t len;
    uint64_t flg;
    linux_msg_bounce_t *b;
    vibeos_cred_t cred;
} msg_send_t;

/* One attempt to queue the message: 1 when it is in (or the call has failed,
 * with the error in r), 0 to wait. */
static long msg_send_look(void *ctx) {
    msg_send_t *m = (msg_send_t *)ctx;
    long r;
    int s;

    linux_ipc_lock();
    s = linux_ipc_slot(g_msg_obj, LINUX_IPC_SLOTS, m->id);
    if (s < 0) {
        /* Removed while the sender waited is EIDRM, as Linux's wake says;
         * a number that never named a queue was refused before the wait. */
        linux_ipc_unlock();
        return -VIBEOS_EIDRM;
    }
    if ((r = linux_ipc_may(&g_msg_obj[s], 0222u, &m->cred)) != 0) {
        linux_ipc_unlock();
        return r;
    }
    {
        linux_msgq_t *q = &g_msgq[s];

        if (q->cbytes + m->len <= q->qbytes && q->qnum + 1u <= q->qbytes &&
            q->qnum < LINUX_MSG_RECORDS) {
            msg_to_queue(&q->text[q->cbytes], m->b, m->len);
            q->rec[q->qnum].type = m->type;
            q->rec[q->qnum].len = m->len;
            q->qnum++;
            q->cbytes += m->len;
            q->lspid = ks_id(ks_current())->tgid;
            q->stime = linux_ipc_now();
            linux_ipc_unlock();
            ks_wake_waiters();
            return 1;
        }
    }
    linux_ipc_unlock();
    return (m->flg & LINUX_IPC_NOWAIT) ? -VIBEOS_EAGAIN : 0;
}

static long linux_sys_msgsnd(uint64_t id, uint64_t msgp, uint64_t sz, uint64_t flg) {
    linux_msg_bounce_t b;
    msg_send_t m;
    long r;
    int s;

    /* Linux's order: the type is read first (EFAULT), then the size and the
     * type are judged (EINVAL), then the text is read (EFAULT), and only then
     * is the queue looked for. */
    if (vibeos_uaccess_copy(&m.type, (const void *)(uintptr_t)msgp, 8u) != 0) {
        return -VIBEOS_EFAULT;
    }
    if (sz > LINUX_MSGMAX || (int64_t)(int32_t)(uint32_t)id < 0) {
        return -VIBEOS_EINVAL;
    }
    if (m.type < 1) {
        return -VIBEOS_EINVAL;
    }
    if (sz != 0u && !linux_user_ok(msgp + 8u, sz, 0)) {
        return -VIBEOS_EFAULT;
    }
    if (msg_bounce_get(&b) != 0) {
        return -VIBEOS_ENOMEM;
    }
    if (msg_copy_in(&b, msgp + 8u, (uint32_t)sz) != 0) {
        msg_bounce_put(&b);
        return -VIBEOS_EFAULT;
    }
    m.id = id;
    m.len = (uint32_t)sz;
    m.flg = flg;
    m.b = &b;
    linux_cred(&m.cred);
    linux_ipc_lock();
    s = linux_ipc_slot(g_msg_obj, LINUX_IPC_SLOTS, id);
    linux_ipc_unlock();
    if (s < 0) {
        r = s;
    } else {
        r = linux_wait_ready(msg_send_look, &m, -1, 0);
        r = r == 1 ? 0 : r;
    }
    msg_bounce_put(&b);
    return r;
}

typedef struct {
    uint64_t id;
    int64_t type;
    uint64_t room;
    uint64_t flg;
    linux_msg_bounce_t *b;
    vibeos_cred_t cred;
    int64_t got_type;
    uint32_t got_len;
} msg_recv_t;

/* Which message a receive with this type and these flags takes: -1 for none. */
static int msg_pick(const linux_msgq_t *q, int64_t type, uint64_t flg) {
    uint32_t k;
    int best = -1;

    if (flg & LINUX_MSG_COPY) {
        return (type >= 0 && (uint64_t)type < q->qnum) ? (int)type : -1;
    }
    for (k = 0; k < q->qnum; k++) {
        int64_t t = q->rec[k].type;

        if (type == 0) {
            return 0;
        }
        if (type > 0) {
            if ((flg & LINUX_MSG_EXCEPT) ? t != type : t == type) {
                return (int)k;
            }
        } else if (t <= -type && (best < 0 || t < q->rec[best].type)) {
            best = (int)k;   /* the lowest type, and the first of it */
        }
    }
    return best;
}

static long msg_recv_look(void *ctx) {
    msg_recv_t *m = (msg_recv_t *)ctx;
    linux_msgq_t *q;
    uint32_t off, len;
    long r;
    int s, k;

    linux_ipc_lock();
    s = linux_ipc_slot(g_msg_obj, LINUX_IPC_SLOTS, m->id);
    if (s < 0) {
        linux_ipc_unlock();
        return -VIBEOS_EIDRM;
    }
    if ((r = linux_ipc_may(&g_msg_obj[s], 0444u, &m->cred)) != 0) {
        linux_ipc_unlock();
        return r;
    }
    q = &g_msgq[s];
    k = msg_pick(q, m->type, m->flg);
    if (k < 0) {
        linux_ipc_unlock();
        return (m->flg & (LINUX_IPC_NOWAIT | LINUX_MSG_COPY)) ? -VIBEOS_ENOMSG : 0;
    }
    len = q->rec[k].len;
    if (len > m->room) {
        if (!(m->flg & LINUX_MSG_NOERROR)) {
            linux_ipc_unlock();
            return -VIBEOS_E2BIG;   /* and the message stays */
        }
        len = (uint32_t)m->room;
    }
    off = msg_offset(q, (uint32_t)k);
    msg_from_queue(m->b, &q->text[off], len);
    m->got_type = q->rec[k].type;
    m->got_len = len;
    if (!(m->flg & LINUX_MSG_COPY)) {
        uint32_t whole = q->rec[k].len, i;

        for (i = off; i + whole < q->cbytes; i++) {
            q->text[i] = q->text[i + whole];
        }
        for (i = (uint32_t)k; i + 1u < q->qnum; i++) {
            q->rec[i] = q->rec[i + 1u];
        }
        q->qnum--;
        q->cbytes -= whole;
        q->lrpid = ks_id(ks_current())->tgid;
        q->rtime = linux_ipc_now();
    }
    linux_ipc_unlock();
    ks_wake_waiters();
    return 1;
}

static long linux_sys_msgrcv(uint64_t id, uint64_t msgp, uint64_t sz, uint64_t type, uint64_t flg) {
    linux_msg_bounce_t b;
    msg_recv_t m;
    long r;
    int s;

    if ((int64_t)sz < 0 || (int64_t)(int32_t)(uint32_t)id < 0) {
        return -VIBEOS_EINVAL;
    }
    if (flg & LINUX_MSG_COPY) {
        /* A copy by position, for a checkpoint: only without waiting, and
         * "except" means nothing to it (Linux's EINVAL for both). */
        if ((flg & LINUX_MSG_EXCEPT) || !(flg & LINUX_IPC_NOWAIT)) {
            return -VIBEOS_EINVAL;
        }
    }
    m.id = id;
    m.type = (int64_t)type;
    m.room = sz > LINUX_MSGMAX ? LINUX_MSGMAX : sz;   /* no message is longer */
    m.flg = flg;
    linux_cred(&m.cred);
    linux_ipc_lock();
    s = linux_ipc_slot(g_msg_obj, LINUX_IPC_SLOTS, id);
    if (s >= 0) {
        r = linux_ipc_may(&g_msg_obj[s], 0444u, &m.cred);
        s = r < 0 ? (int)r : s;
    }
    linux_ipc_unlock();
    if (s < 0) {
        return s;
    }
    if (msg_bounce_get(&b) != 0) {
        return -VIBEOS_ENOMEM;
    }
    m.b = &b;
    r = linux_wait_ready(msg_recv_look, &m, -1, 0);
    if (r == 1) {
        /* Taken off the queue already: a buffer that faults now loses it, as
         * it does on Linux, which copies after the unlink too. */
        if (!linux_user_ok(msgp, 8u + (uint64_t)m.got_len, 1) ||
            vibeos_uaccess_copy((void *)(uintptr_t)msgp, &m.got_type, 8u) != 0 ||
            msg_copy_out(msgp + 8u, &b, m.got_len) != 0) {
            r = -VIBEOS_EFAULT;
        } else {
            r = (long)m.got_len;
        }
    }
    msg_bounce_put(&b);
    return r;
}

static void msg_stat_of(int s, linux_msqid64_ds_t *out) {
    const linux_msgq_t *q = &g_msgq[s];
    uint8_t *raw = (uint8_t *)out;
    uint32_t i;

    for (i = 0; i < (uint32_t)sizeof(*out); i++) {
        raw[i] = 0;
    }
    linux_ipc_perm_out(&g_msg_obj[s], &out->msg_perm);
    out->msg_stime = q->stime;
    out->msg_rtime = q->rtime;
    out->msg_ctime = g_msg_obj[s].ctime;
    out->msg_cbytes = q->cbytes;
    out->msg_qnum = q->qnum;
    out->msg_qbytes = q->qbytes;
    out->msg_lspid = (int32_t)q->lspid;
    out->msg_lrpid = (int32_t)q->lrpid;
}

static long linux_sys_msgctl(uint64_t id, uint64_t cmd, uint64_t buf) {
    vibeos_cred_t c;
    linux_msqid64_ds_t ds;
    long r = 0;
    int s;

    linux_cred(&c);
    switch (VIBEOS_ARG_INT(cmd)) {
        case LINUX_IPC_INFO:
        case LINUX_MSG_INFO: {
            linux_msginfo_t mi;
            uint8_t *raw = (uint8_t *)&mi;
            uint32_t k, msgs = 0;
            uint64_t bytes = 0;

            /* Every byte, the two after msgseg included: they went to the
             * program as whatever the kernel stack held (M-109), as shm.c's
             * IPC_INFO already knew. */
            for (k = 0; k < (uint32_t)sizeof(mi); k++) {
                raw[k] = 0;
            }
            linux_ipc_lock();
            for (k = 0; k < LINUX_IPC_SLOTS; k++) {
                if (g_msg_obj[k].used) {
                    msgs += g_msgq[k].qnum;
                    bytes += g_msgq[k].cbytes;
                }
            }
            mi.msgmni = (int32_t)(g_msg_ns.max < LINUX_IPC_SLOTS ? g_msg_ns.max : LINUX_IPC_SLOTS);
            mi.msgmax = LINUX_MSGMAX;
            mi.msgmnb = LINUX_MSGMNB;
            mi.msgssz = 16;
            mi.msgseg = 0xffffu;
            if (VIBEOS_ARG_INT(cmd) == LINUX_MSG_INFO) {
                /* MSG_INFO reports use where IPC_INFO reports limits. */
                mi.msgpool = (int32_t)linux_ipc_live(g_msg_obj, LINUX_IPC_SLOTS);
                mi.msgmap = (int32_t)msgs;
                mi.msgtql = (int32_t)bytes;
            } else {
                mi.msgpool = (int32_t)(LINUX_IPC_SLOTS * LINUX_MSGMNB / 1024u);
                mi.msgmap = LINUX_MSGMNB;
                mi.msgtql = LINUX_MSGMNB;
            }
            r = linux_ipc_max_index(g_msg_obj, LINUX_IPC_SLOTS);
            linux_ipc_unlock();
            return linux_ipc_out(buf, &mi, sizeof(mi)) != 0 ? -VIBEOS_EFAULT : r;
        }
        case LINUX_IPC_STAT:
        case LINUX_MSG_STAT:
        case LINUX_MSG_STAT_ANY:
            linux_ipc_lock();
            s = VIBEOS_ARG_INT(cmd) == LINUX_IPC_STAT ? linux_ipc_slot(g_msg_obj, LINUX_IPC_SLOTS, id)
                                                      : linux_ipc_index(g_msg_obj, LINUX_IPC_SLOTS, id);
            if (s >= 0 && VIBEOS_ARG_INT(cmd) != LINUX_MSG_STAT_ANY) {
                r = linux_ipc_may(&g_msg_obj[s], 0444u, &c);
            }
            if (s >= 0 && r == 0) {
                msg_stat_of(s, &ds);
                /* The *_STAT forms take an index and answer with the number. */
                r = VIBEOS_ARG_INT(cmd) == LINUX_IPC_STAT ? 0 : (long)g_msg_obj[s].id;
            }
            linux_ipc_unlock();
            if (s < 0) {
                return s;
            }
            if (r >= 0 && linux_ipc_out(buf, &ds, sizeof(ds)) != 0) {
                return -VIBEOS_EFAULT;
            }
            return r;
        case LINUX_IPC_SET:
            if (linux_ipc_in(&ds, buf, sizeof(ds)) != 0) {
                return -VIBEOS_EFAULT;
            }
            linux_ipc_lock();
            s = linux_ipc_slot(g_msg_obj, LINUX_IPC_SLOTS, id);
            if (s >= 0) {
                r = linux_ipc_owner(&g_msg_obj[s], &c);
            }
            if (s >= 0 && r == 0 && ds.msg_qbytes > LINUX_MSGMNB) {
                /* Raising it past msgmnb is CAP_SYS_RESOURCE's on Linux; here
                 * nobody's, because the queue has no more room than that. */
                r = c.euid == 0u ? -VIBEOS_EINVAL : -VIBEOS_EPERM;
            }
            if (s >= 0 && r == 0) {
                linux_ipc_set(&g_msg_obj[s], &ds.msg_perm);
                g_msgq[s].qbytes = ds.msg_qbytes;
            }
            linux_ipc_unlock();
            if (r == 0) {
                ks_wake_waiters();   /* a sender may fit now, or not be allowed to */
            }
            return s < 0 ? s : r;
        case LINUX_IPC_RMID:
            linux_ipc_lock();
            s = linux_ipc_slot(g_msg_obj, LINUX_IPC_SLOTS, id);
            if (s >= 0) {
                r = linux_ipc_owner(&g_msg_obj[s], &c);
            }
            if (s >= 0 && r == 0) {
                g_msg_obj[s].used = 0;   /* the messages go with it */
            }
            linux_ipc_unlock();
            if (s >= 0 && r == 0) {
                ks_wake_waiters();   /* everyone waiting on it: EIDRM */
            }
            return s < 0 ? s : r;
        default:
            return -VIBEOS_EINVAL;
    }
}

/* ---- what /proc shows -------------------------------------------------------- */

/* `v` right-aligned in `width` columns: decimal, or octal for permissions. */
void linux_ipc_putn(linux_ipc_put_t put, void *o, int64_t v, uint32_t width, uint32_t base) {
    char d[24];
    uint32_t n = 0, neg = v < 0;
    uint64_t u = neg ? (uint64_t)(-(v + 1)) + 1u : (uint64_t)v;

    do {
        d[sizeof(d) - 1u - n++] = (char)('0' + u % base);
        u /= base;
    } while (u != 0u);
    if (neg) {
        d[sizeof(d) - 1u - n++] = '-';
    }
    while (n < width) {
        d[sizeof(d) - 1u - n++] = ' ';
    }
    put(o, &d[sizeof(d) - n], n);
}

void linux_ipc_puts(linux_ipc_put_t put, void *o, const char *s) {
    uint32_t n = 0;

    while (s[n]) {
        n++;
    }
    put(o, s, n);
}

void linux_ipc_proc_msg(linux_ipc_put_t put, void *o) {
    uint32_t s;

    linux_ipc_puts(put, o, "       key      msqid perms      cbytes       qnum lspid lrpid   uid   gid"
                        "  cuid  cgid      stime      rtime      ctime\n");
    linux_ipc_lock();
    for (s = 0; s < LINUX_IPC_SLOTS; s++) {
        const linux_ipc_obj_t *ob = &g_msg_obj[s];
        const linux_msgq_t *q = &g_msgq[s];

        if (!ob->used) {
            continue;
        }
        linux_ipc_putn(put, o, ob->key, 10u, 10u);
        linux_ipc_putn(put, o, ob->id, 11u, 10u);
        linux_ipc_puts(put, o, "  ");
        linux_ipc_putn(put, o, ob->mode, 4u, 8u);
        linux_ipc_putn(put, o, (int64_t)q->cbytes, 12u, 10u);
        linux_ipc_putn(put, o, q->qnum, 11u, 10u);
        linux_ipc_putn(put, o, q->lspid, 6u, 10u);
        linux_ipc_putn(put, o, q->lrpid, 6u, 10u);
        linux_ipc_putn(put, o, ob->uid, 6u, 10u);
        linux_ipc_putn(put, o, ob->gid, 6u, 10u);
        linux_ipc_putn(put, o, ob->cuid, 6u, 10u);
        linux_ipc_putn(put, o, ob->cgid, 6u, 10u);
        linux_ipc_putn(put, o, q->stime, 11u, 10u);
        linux_ipc_putn(put, o, q->rtime, 11u, 10u);
        linux_ipc_putn(put, o, ob->ctime, 11u, 10u);
        put(o, "\n", 1u);
    }
    linux_ipc_unlock();
}


/* ---- the tunables under /proc/sys/kernel ------------------------------------- */

/* A number and a newline, as every sysctl reads. */
static void ipc_put_value(linux_ipc_put_t put, void *o, int64_t v) {
    linux_ipc_putn(put, o, v, 0u, 10u);
    put(o, "\n", 1u);
}

/* What a write says: one decimal number, signed, with blanks around it. */
int linux_ipc_parse(const char *buf, uint32_t len, int64_t *out) {
    uint32_t i = 0;
    int neg = 0, digits = 0;
    uint64_t v = 0;

    while (i < len && (buf[i] == ' ' || buf[i] == '\t')) {
        i++;
    }
    if (i < len && buf[i] == '-') {
        neg = 1;
        i++;
    }
    while (i < len && buf[i] >= '0' && buf[i] <= '9') {
        v = v * 10u + (uint64_t)(buf[i++] - '0');
        if (v > 0x80000000ull) {
            return -VIBEOS_EINVAL;
        }
        digits = 1;
    }
    while (i < len && (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\n')) {
        i++;
    }
    if (!digits || i != len) {
        return -VIBEOS_EINVAL;
    }
    *out = neg ? -(int64_t)v : (int64_t)v;
    return 0;
}

/* The limit on how many there may be: what a program reads before it makes
 * that many. Linux accepts up to IPCMNI; above what this kernel stores, the
 * value is kept and the table is the limit. */
static int ipc_set_max(linux_ipc_ns_t *ns, const char *buf, uint32_t len) {
    int64_t v;

    if (linux_ipc_parse(buf, len, &v) != 0 || v < 0 || v > (int64_t)LINUX_IPC_INDEXES) {
        return -VIBEOS_EINVAL;
    }
    linux_ipc_lock();
    ns->max = (uint32_t)v;
    linux_ipc_unlock();
    return 0;
}

/* The number the next object is to have, or -1: used up by that object. */
static int ipc_set_next_id(linux_ipc_ns_t *ns, const char *buf, uint32_t len) {
    int64_t v;

    if (linux_ipc_parse(buf, len, &v) != 0 || v < -1 || v > 0x7fffffff) {
        return -VIBEOS_EINVAL;
    }
    linux_ipc_lock();
    ns->next_id = (int32_t)v;
    linux_ipc_unlock();
    return 0;
}

static void ipc_msgmni_text(linux_ipc_put_t put, void *o) {
    ipc_put_value(put, o, g_msg_ns.max);
}

static int ipc_msgmni_write(const char *buf, uint32_t len) {
    return ipc_set_max(&g_msg_ns, buf, len);
}

static void ipc_msg_next_id_text(linux_ipc_put_t put, void *o) {
    ipc_put_value(put, o, g_msg_ns.next_id);
}

static int ipc_msg_next_id_write(const char *buf, uint32_t len) {
    return ipc_set_next_id(&g_msg_ns, buf, len);
}

/* msgmax and msgmnb are the sizes of what this kernel stores, and fixed:
 * writable on Linux, read-only here, because a larger value would be a
 * promise the queue's storage cannot keep. */
static void ipc_msgmax_text(linux_ipc_put_t put, void *o) {
    ipc_put_value(put, o, LINUX_MSGMAX);
}

static void ipc_msgmnb_text(linux_ipc_put_t put, void *o) {
    ipc_put_value(put, o, LINUX_MSGMNB);
}

static void ipc_shmmni_text(linux_ipc_put_t put, void *o) {
    ipc_put_value(put, o, linux_ipc_shm_ns()->max);
}

static int ipc_shmmni_write(const char *buf, uint32_t len) {
    return ipc_set_max(linux_ipc_shm_ns(), buf, len);
}

static void ipc_shm_next_id_text(linux_ipc_put_t put, void *o) {
    ipc_put_value(put, o, linux_ipc_shm_ns()->next_id);
}

static int ipc_shm_next_id_write(const char *buf, uint32_t len) {
    return ipc_set_next_id(linux_ipc_shm_ns(), buf, len);
}

static void ipc_sem_next_id_text(linux_ipc_put_t put, void *o) {
    ipc_put_value(put, o, linux_ipc_sem_ns()->next_id);
}

static int ipc_sem_next_id_write(const char *buf, uint32_t len) {
    return ipc_set_next_id(linux_ipc_sem_ns(), buf, len);
}

/* /proc/sys/fs/pipe-max-size: the largest a pipe may become, Linux's default -
 * read-only, because it is also the most this kernel keeps for one. */
static void ipc_pipe_max_text(linux_ipc_put_t put, void *o) {
    ipc_put_value(put, o, VIBEOS_PIPE_MAX_BYTES);
}

static const vibeos_procfs_extra_t g_ipc_files[] = {
    {"sysvipc/msg", linux_ipc_proc_msg, 0},
    {"sysvipc/sem", linux_ipc_proc_sem, 0},
    {"sysvipc/shm", linux_ipc_proc_shm, 0},
    {"sys/kernel/msgmax", ipc_msgmax_text, 0},
    {"sys/kernel/msgmnb", ipc_msgmnb_text, 0},
    {"sys/kernel/msgmni", ipc_msgmni_text, ipc_msgmni_write},
    {"sys/kernel/msg_next_id", ipc_msg_next_id_text, ipc_msg_next_id_write},
    {"sys/kernel/sem", linux_ipc_proc_semlimits, 0},
    {"sys/kernel/sem_next_id", ipc_sem_next_id_text, ipc_sem_next_id_write},
    {"sys/kernel/shmall", linux_ipc_shmall_text, linux_ipc_shmall_write},
    {"sys/kernel/shmmax", linux_ipc_shmmax_text, linux_ipc_shmmax_write},
    {"sys/kernel/shmmni", ipc_shmmni_text, ipc_shmmni_write},
    {"sys/kernel/shm_next_id", ipc_shm_next_id_text, ipc_shm_next_id_write},
    {"sys/fs/mqueue/msg_default", linux_mq_msg_default_text, linux_mq_msg_default_write},
    {"sys/fs/mqueue/msg_max", linux_mq_msg_max_text, linux_mq_msg_max_write},
    {"sys/fs/mqueue/msgsize_default", linux_mq_msgsize_default_text, linux_mq_msgsize_default_write},
    {"sys/fs/mqueue/msgsize_max", linux_mq_msgsize_max_text, linux_mq_msgsize_max_write},
    {"sys/fs/mqueue/queues_max", linux_mq_queues_max_text, linux_mq_queues_max_write},
    {"sys/fs/pipe-max-size", ipc_pipe_max_text, 0},
};

const vibeos_procfs_extra_t *linux_ipc_proc_files(uint32_t *count) {
    *count = (uint32_t)(sizeof(g_ipc_files) / sizeof(g_ipc_files[0]));
    return g_ipc_files;
}

/* Back to a machine with no objects, for the host tests' fresh kernel. */
void linux_ipc_reset(void) {
    uint32_t i;

    linux_ipc_lock();
    for (i = 0; i < LINUX_IPC_SLOTS; i++) {
        g_msg_obj[i].used = 0;
    }
    g_msg_ns.seq = 0;
    g_msg_ns.next_id = -1;
    g_msg_ns.max = LINUX_IPC_SLOTS;
    g_msg_ns.last_idx = -1;
    linux_ipc_sem_reset();
    linux_ipc_shm_reset();
    linux_ipc_unlock();
}

/* ---- the syscalls this file implements --------------------------------------- */
#define LINUX_IPC_SYSCALLS(X) \
    X(68,  msgget,  IPC_GET,  NOPTR, linux_sys_msgget(ARG(0), ARG(1))) \
    X(69,  msgsnd,  IPC_SEND, PTRS(IN(1, 8)), linux_sys_msgsnd(ARG(0), ARG(1), ARG(2), ARG(3))) \
    X(70,  msgrcv,  IPC_RECV, NOPTR, linux_sys_msgrcv(ARG(0), ARG(1), ARG(2), ARG(3), ARG(4))) \
    X(71,  msgctl,  IPC_CTL,  NOPTR, linux_sys_msgctl(ARG(0), ARG(1), ARG(2)))

LINUX_DEFINE_SYSCALLS(ipc, LINUX_IPC_SYSCALLS)
