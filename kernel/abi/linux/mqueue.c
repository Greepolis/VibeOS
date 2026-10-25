/* POSIX message queues (docs/abi/ L7 step 4).
 *
 * A queue has a name, outside every filesystem - Linux keeps them on mqueue,
 * which a program may mount at /dev/mqueue and none here does - and a
 * descriptor names an open queue as it names an open file: an open file
 * description of this type, holding one reference to the queue. A queue that
 * has been unlinked lives until its last description goes.
 *
 * Messages are the highest priority first and in order of arrival within
 * one. Each is a header page and the pages of its text, allocated by the
 * sender before it takes the lock and freed by the receiver after it lets go:
 * nothing slow, and nothing that can fault, under the lock. A queue's limits
 * are its own, set when it is made - which is why the text is not stored in
 * advance: the superuser may make a queue far larger than the defaults (LTP's
 * mq_open01 asks for twenty messages of sixteen kilobytes), and its memory is
 * spent only by what is actually in it.
 *
 * mq_notify asks for one signal when a message arrives in an empty queue that
 * nobody is waiting on, and then it is gone. A thread notification is a
 * netlink socket on Linux, and there is none here: SIGEV_THREAD is refused. */

#include "linux_internal.h"
#include "vibeos/procfs.h"
#include "vibeos/cred.h"

#define LINUX_MQ_QUEUES   32u
#define LINUX_MQ_NAME_MAX 255u
#define LINUX_MQ_TEXT_PAGES 500u    /* pointers a header page holds: 2 MB a message */
/* The largest message this queue can hold, and so the most any limit on one
 * may say: Linux's own hard limit is 16 MiB, but a message here is a header
 * page and LINUX_MQ_TEXT_PAGES pages of text. The two tunables took 16 MiB, and
 * mq_timedsend then wrote page pointers past the header's array (H-035). */
#define LINUX_MQ_MSGSIZE_CAP ((int64_t)LINUX_MQ_TEXT_PAGES * 4096)
#define LINUX_MQ_HARD_MSGMAX 65536

typedef struct linux_mq_msg {
    struct linux_mq_msg *next;
    uint32_t prio;
    uint32_t len;
    uint32_t npages;
    uint8_t *page[LINUX_MQ_TEXT_PAGES];
} linux_mq_msg_t;

typedef struct {
    uint8_t used;
    uint8_t named;            /* still has its name: mq_unlink has not run */
    uint32_t refs;            /* descriptions that name it */
    char name[LINUX_MQ_NAME_MAX + 1u];
    uint32_t uid, gid, mode;
    int64_t maxmsg, msgsize, curmsgs;
    uint64_t bytes;
    linux_mq_msg_t *head;
    /* The last message: a send of a priority no higher than its goes after
     * it at once, where the walk from the head used to cross every message
     * of the queue (#110) - and that is how most programs send, at one
     * priority. Null with head; kept by every place that changes the list. */
    linux_mq_msg_t *tail;
    uint32_t receivers;       /* waiting in a receive now */
    uint32_t notify_tgid;     /* 0: nobody asked */
    uint32_t notify_sig;
    uint32_t notify_how;      /* SIGEV_SIGNAL or SIGEV_NONE */
    uint64_t notify_value;
} linux_mq_t;

static linux_mq_t g_mq[LINUX_MQ_QUEUES];

/* The /proc/sys/fs/mqueue tunables, as Linux starts them. queues_max is a
 * limit on everybody but the superuser, and the table is the limit on all. */
static int64_t g_mq_msg_max = 10, g_mq_msgsize_max = 8192;
static int64_t g_mq_msg_default = 10, g_mq_msgsize_default = 8192;
static int64_t g_mq_queues_max = 256;

_Static_assert(sizeof(linux_mq_msg_t) <= 4096u, "a message's header is one page");

static void mq_msg_free(linux_mq_msg_t *m) {
    uint32_t i;

    for (i = 0; i < m->npages; i++) {
        ks_page_free(m->page[i], "mq text");
    }
    ks_page_free(m, "mq header");
}

/* The name a call names: copied, judged as Linux judges a name in mqueue's
 * directory - nothing there is ENOENT, a slash or a dot entry is not a name
 * (EACCES), and longer than NAME_MAX is ENAMETOOLONG. */
static long mq_name_in(uint64_t uptr, char *out) {
    uint32_t n;

    if (ks_copy_user_string(uptr, out, (int)LINUX_MQ_NAME_MAX + 2) != 0) {
        return -VIBEOS_EFAULT;
    }
    for (n = 0; out[n]; n++) {
        if (n >= LINUX_MQ_NAME_MAX) {
            return -VIBEOS_ENAMETOOLONG;
        }
        if (out[n] == '/') {
            return -VIBEOS_EACCES;
        }
    }
    if (n == 0u) {
        return -VIBEOS_ENOENT;
    }
    if ((n == 1u && out[0] == '.') || (n == 2u && out[0] == '.' && out[1] == '.')) {
        return -VIBEOS_EACCES;
    }
    return 0;
}

static int mq_same(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/* The queue by name, under the lock: its slot, or -1. */
static int mq_find(const char *name) {
    uint32_t i;

    for (i = 0; i < LINUX_MQ_QUEUES; i++) {
        if (g_mq[i].used && g_mq[i].named && mq_same(g_mq[i].name, name)) {
            return (int)i;
        }
    }
    return -1;
}

static void mq_drop_ref(int q) {
    linux_mq_msg_t *list = 0;

    linux_ipc_lock();
    if (--g_mq[q].refs == 0u && !g_mq[q].named) {
        list = g_mq[q].head;
        g_mq[q].head = 0;
        g_mq[q].tail = 0;
        g_mq[q].used = 0;
    }
    linux_ipc_unlock();
    while (list) {
        linux_mq_msg_t *next = list->next;

        mq_msg_free(list);
        list = next;
    }
}

/* ---- the description ------------------------------------------------------- */

static int mq_of(const vibeos_file_t *f) {
    return f->ev_index;
}

/* What reading a queue's file says, as /dev/mqueue's files say it. */
static long mq_read(vibeos_file_t *f, uint64_t ubuf, uint64_t len) {
    char text[96];
    uint32_t n = 0, i;
    int q = mq_of(f);
    struct {
        const char *label;
        int64_t v;
        uint32_t width;
    } col[4];

    linux_ipc_lock();
    col[0].label = "QSIZE:";
    col[0].v = (int64_t)g_mq[q].bytes;
    col[0].width = 10u;
    col[1].label = " NOTIFY:";
    col[1].v = g_mq[q].notify_tgid ? (int64_t)g_mq[q].notify_how : 0;
    col[1].width = 5u;
    col[2].label = " SIGNO:";
    col[2].v = g_mq[q].notify_tgid ? (int64_t)g_mq[q].notify_sig : 0;
    col[2].width = 5u;
    col[3].label = " NOTIFY_PID:";
    col[3].v = g_mq[q].notify_tgid;
    col[3].width = 6u;
    linux_ipc_unlock();
    for (i = 0; i < 4u; i++) {
        char d[24];
        uint32_t k = 0, w;
        uint64_t v = (uint64_t)col[i].v;
        const char *s = col[i].label;

        while (*s) {
            text[n++] = *s++;
        }
        do {
            d[k++] = (char)('0' + v % 10u);
            v /= 10u;
        } while (v != 0u);
        for (w = k; k > 0u;) {
            text[n++] = d[--k];
        }
        while (w++ < col[i].width) {
            text[n++] = ' ';   /* left-aligned, as %-10lu */
        }
    }
    text[n++] = '\n';
    if (f->pos >= n) {
        return 0;
    }
    if (len > n - f->pos) {
        len = n - f->pos;
    }
    if (vibeos_uaccess_copy((void *)(uintptr_t)ubuf, text + f->pos, len) != 0) {
        return -VIBEOS_EFAULT;
    }
    f->pos += len;
    return (long)len;
}

static uint32_t mq_ready(vibeos_file_t *f) {
    int q = mq_of(f);
    uint32_t r = 0;

    linux_ipc_lock();
    if (g_mq[q].curmsgs > 0) {
        r |= VIBEOS_READY_IN;
    }
    if (g_mq[q].curmsgs < g_mq[q].maxmsg) {
        r |= VIBEOS_READY_OUT;
    }
    linux_ipc_unlock();
    return r;
}

static void mq_release(vibeos_file_t *f) {
    int q = mq_of(f);
    uint32_t me = ks_current() >= 0 ? ks_id(ks_current())->tgid : 0u;

    if (q < 0) {
        return;
    }
    /* The last reference: a process that ends without closing. A close
     * takes the notification before this (linux_mq_on_close). */
    linux_ipc_lock();
    if (g_mq[q].notify_tgid == me) {
        g_mq[q].notify_tgid = 0;
    }
    linux_ipc_unlock();
    mq_drop_ref(q);
    f->ev_index = -1;
}

/* A close of any descriptor of the queue by the process that registered takes
 * its notification back - another description's, or a dup's, as much as the
 * one it registered through: Linux does it in the queue's flush, which runs on
 * every close (#35, measured on Linux 5.15). Releasing the description alone
 * missed a dup's close, whose description lives on. */
void linux_mq_on_close(const vibeos_file_t *f) {
    int q = mq_of(f);
    uint32_t me = ks_current() >= 0 ? ks_id(ks_current())->tgid : 0u;

    if (q < 0 || me == 0u) {
        return;
    }
    linux_ipc_lock();
    if (g_mq[q].notify_tgid == me) {
        g_mq[q].notify_tgid = 0;
    }
    linux_ipc_unlock();
}

const vibeos_file_ops_t linux_fops_mqueue = {
    .name = "mqueue",
    .read = mq_read,
    .ready = mq_ready,
    .release = mq_release,
};

/* The queue a descriptor names, held by the caller's reference: -EBADF if it
 * is not one. */
static vibeos_file_t *mq_file(uint64_t fd, long *err) {
    vibeos_file_t *f = linux_file_get(fd);

    if (!f || f->ops != &linux_fops_mqueue) {
        if (f) {
            vibeos_file_put(f);
        }
        *err = -VIBEOS_EBADF;
        return 0;
    }
    return f;
}

/* ---- mq_open, mq_unlink ------------------------------------------------------ */

static long linux_sys_mq_open(uint64_t name_uptr, uint64_t oflag, uint64_t mode, uint64_t attr_uptr) {
    const uint32_t acc = (uint32_t)oflag & VIBEOS_O_ACCMODE;
    char name[LINUX_MQ_NAME_MAX + 2u];
    linux_mq_attr_t attr;
    vibeos_cred_t c;
    vibeos_file_t *f;
    int q, made = 0;
    long r;

    if ((r = mq_name_in(name_uptr, name)) != 0) {
        return r;
    }
    if (acc == 3u) {
        return -VIBEOS_EINVAL;
    }
    if ((oflag & VIBEOS_O_CREAT) && attr_uptr != 0u) {
        if (linux_ipc_in(&attr, attr_uptr, sizeof(attr)) != 0) {
            return -VIBEOS_EFAULT;
        }
    }
    linux_cred(&c);
    f = vibeos_file_alloc(&linux_fops_mqueue, acc | ((uint32_t)oflag & VIBEOS_O_NONBLOCK));
    if (!f) {
        return -VIBEOS_ENFILE;
    }
    f->ev_index = -1;
    linux_ipc_lock();
    q = mq_find(name);
    if (q >= 0) {
        if ((oflag & VIBEOS_O_CREAT) && (oflag & VIBEOS_O_EXCL)) {
            r = -VIBEOS_EEXIST;
        } else {
            uint32_t want = acc == VIBEOS_O_RDONLY ? VIBEOS_MAY_READ
                          : acc == VIBEOS_O_WRONLY ? VIBEOS_MAY_WRITE : VIBEOS_MAY_READ | VIBEOS_MAY_WRITE;
            r = vibeos_cred_may(&c, 0, g_mq[q].mode, g_mq[q].uid, g_mq[q].gid, want);
        }
    } else if (!(oflag & VIBEOS_O_CREAT)) {
        r = -VIBEOS_ENOENT;
    } else {
        int64_t maxmsg = g_mq_msg_default, msgsize = g_mq_msgsize_default;
        uint32_t live = 0, i;

        r = 0;
        if (attr_uptr != 0u) {
            /* The superuser may go past the tunables, as far as Linux's
             * hard limits; anybody else to them. */
            maxmsg = attr.mq_maxmsg;
            msgsize = attr.mq_msgsize;
            if (maxmsg <= 0 || msgsize <= 0) {
                r = -VIBEOS_EINVAL;
            } else if (c.euid != 0u ? (maxmsg > g_mq_msg_max || msgsize > g_mq_msgsize_max)
                                    : (maxmsg > LINUX_MQ_HARD_MSGMAX ||
                                       msgsize > LINUX_MQ_MSGSIZE_CAP)) {
                r = -VIBEOS_EINVAL;
            }
        }
        for (i = 0; i < LINUX_MQ_QUEUES; i++) {
            live += g_mq[i].used;
        }
        if (r == 0 && ((live >= (uint64_t)g_mq_queues_max && c.euid != 0u) || live >= LINUX_MQ_QUEUES)) {
            r = -VIBEOS_ENOSPC;
        }
        for (i = 0; r == 0 && i < LINUX_MQ_QUEUES && g_mq[i].used; i++) {
        }
        if (r == 0) {
            linux_mq_t *m = &g_mq[i];
            uint32_t k;

            for (k = 0; name[k]; k++) {
                m->name[k] = name[k];
            }
            m->name[k] = 0;
            m->used = m->named = 1;
            m->refs = 0;
            m->uid = c.fsuid;
            m->gid = c.fsgid;
            m->mode = (uint32_t)mode & 0777u & ~linux_umask();
            m->maxmsg = maxmsg;
            m->msgsize = msgsize;
            m->curmsgs = 0;
            m->bytes = 0;
            m->head = 0;
            m->tail = 0;
            m->receivers = 0;
            m->notify_tgid = 0;
            q = (int)i;
            made = 1;
        }
    }
    if (r == 0) {
        g_mq[q].refs++;
        f->ev_index = q;
    }
    linux_ipc_unlock();
    if (r != 0) {
        vibeos_file_put(f);
        return r;
    }
    r = linux_fd_install(f, (oflag & VIBEOS_O_CLOEXEC) ? VIBEOS_FD_CLOEXEC : 0u, 0);
    if (r < 0 && made) {
        /* No descriptor for it (EMFILE): as if it had never been made. The
         * description released the reference; the name goes too. */
        linux_ipc_lock();
        if (g_mq[q].used && g_mq[q].named && mq_same(g_mq[q].name, name)) {
            g_mq[q].named = 0;
            if (g_mq[q].refs == 0u) {
                g_mq[q].used = 0;
            }
        }
        linux_ipc_unlock();
    }
    return r;
}

static long linux_sys_mq_unlink(uint64_t name_uptr) {
    char name[LINUX_MQ_NAME_MAX + 2u];
    vibeos_cred_t c;
    linux_mq_msg_t *list = 0;
    long r;
    int q;

    if ((r = mq_name_in(name_uptr, name)) != 0) {
        return r;
    }
    linux_cred(&c);
    linux_ipc_lock();
    q = mq_find(name);
    if (q < 0) {
        r = -VIBEOS_ENOENT;
    } else if (c.fsuid != 0u && c.fsuid != g_mq[q].uid) {
        /* mqueue's directory is sticky: only the owner removes a name. */
        r = -VIBEOS_EACCES;
    } else {
        g_mq[q].named = 0;
        if (g_mq[q].refs == 0u) {
            list = g_mq[q].head;
            g_mq[q].head = 0;
            g_mq[q].tail = 0;
            g_mq[q].used = 0;
        }
    }
    linux_ipc_unlock();
    while (list) {
        linux_mq_msg_t *next = list->next;

        mq_msg_free(list);
        list = next;
    }
    return r;
}

/* ---- sending and receiving ------------------------------------------------- */

/* An absolute timeout on CLOCK_REALTIME, as ticks from now: -1 for none, and
 * 0 for one that has passed (look once). */
static long mq_deadline(uint64_t ts_uptr, int64_t *ticks) {
    linux_timespec_t ts;
    int64_t at, now;

    *ticks = -1;
    if (ts_uptr == 0u) {
        return 0;
    }
    if (linux_ipc_in(&ts, ts_uptr, sizeof(ts)) != 0) {
        return -VIBEOS_EFAULT;
    }
    if ((at = linux_ticks_of(&ts)) < 0) {
        return -VIBEOS_EINVAL;
    }
    now = linux_clock_read(LINUX_CLOCK_REALTIME);
    *ticks = at > now ? at - now : 0;
    return 0;
}

/* Raise the notification a sender owes, outside the queue's lock. */
static void mq_notify_send(uint32_t tgid, uint32_t sig, uint64_t value) {
    vibeos_siginfo_t why;
    uint32_t i;
    int target;

    for (i = 0; i < sizeof(why); i++) {
        ((unsigned char *)&why)[i] = 0;
    }
    why.from = VIBEOS_SIG_FROM_QUEUE;
    why.code = LINUX_SI_MESGQ;
    why.pid = ks_id(ks_current())->tgid;
    {
        vibeos_cred_t c;

        linux_cred(&c);
        why.uid = c.uid;
    }
    why.addr = value;
    ks_lock(ks_sched_lock(), __func__);
    target = ks_task_by_tid(tgid);
    if (target >= 0) {
        (void)ks_signal_send(target, sig, &why);
    }
    ks_unlock(ks_sched_lock());
}

typedef struct {
    int q;
    linux_mq_msg_t *m;
    int nonblock;
    uint32_t notify_tgid, notify_sig;
    uint64_t notify_value;
} mq_send_t;

static long mq_send_look(void *ctx) {
    mq_send_t *s = (mq_send_t *)ctx;
    linux_mq_t *q = &g_mq[s->q];

    linux_ipc_lock();
    if (q->curmsgs < q->maxmsg) {
        linux_mq_msg_t **at = (q->tail && q->tail->prio >= s->m->prio) ? &q->tail->next : &q->head;

        while (*at && (*at)->prio >= s->m->prio) {
            at = &(*at)->next;   /* after every message of its priority or above */
        }
        s->m->next = *at;
        if (*at == 0) {
            q->tail = s->m;   /* it went last */
        }
        *at = s->m;
        q->bytes += s->m->len;
        s->m = 0;
        if (q->curmsgs == 0 && q->receivers == 0u && q->notify_tgid != 0u) {
            /* The one notification: into an empty queue nobody waits on. */
            if (q->notify_how == LINUX_SIGEV_SIGNAL) {
                s->notify_tgid = q->notify_tgid;
                s->notify_sig = q->notify_sig;
                s->notify_value = q->notify_value;
            }
            q->notify_tgid = 0;
        }
        q->curmsgs++;
        linux_ipc_unlock();
        ks_wake_waiters();
        return 1;
    }
    linux_ipc_unlock();
    return s->nonblock ? -VIBEOS_EAGAIN : 0;
}

static long linux_sys_mq_timedsend(uint64_t fd, uint64_t uptr, uint64_t len, uint64_t prio,
                                   uint64_t ts_uptr) {
    vibeos_file_t *f;
    mq_send_t s;
    int64_t ticks;
    uint32_t i;
    long r = 0;

    /* Linux's order: the timeout is read and judged before the call proper
     * starts, then the priority, then the descriptor. */
    if ((r = mq_deadline(ts_uptr, &ticks)) != 0) {
        return r;
    }
    if (prio >= LINUX_MQ_PRIO_MAX) {
        return -VIBEOS_EINVAL;
    }
    s.m = 0;
    if (!(f = mq_file(fd, &r))) {
        return r;
    }
    if ((f->flags & VIBEOS_O_ACCMODE) == VIBEOS_O_RDONLY) {
        vibeos_file_put(f);
        return -VIBEOS_EBADF;
    }
    s.q = mq_of(f);
    linux_ipc_lock();
    r = (int64_t)len > g_mq[s.q].msgsize ? -VIBEOS_EMSGSIZE : 0;
    linux_ipc_unlock();
    if (r == 0 && len != 0u && !linux_user_ok(uptr, len, 0)) {
        r = -VIBEOS_EFAULT;
    }
    s.m = r == 0 ? (linux_mq_msg_t *)ks_page_alloc() : 0;
    if (r == 0 && !s.m) {
        r = -VIBEOS_ENOMEM;
    }
    if (r == 0) {
        s.m->next = 0;
        s.m->prio = (uint32_t)prio;
        s.m->len = (uint32_t)len;
        s.m->npages = 0;
        for (i = 0; r == 0 && i * 4096ull < len; i++) {
            uint64_t chunk = len - i * 4096ull > 4096u ? 4096u : len - i * 4096ull;

            /* Held by the limits on msgsize; said again where the array is
             * written, so that a limit that drifts is a refusal here and not a
             * store past the header (H-035). */
            if (i >= LINUX_MQ_TEXT_PAGES) {
                r = -VIBEOS_EMSGSIZE;
                break;
            }
            if (!(s.m->page[i] = (uint8_t *)ks_page_alloc())) {
                r = -VIBEOS_ENOMEM;
                break;
            }
            s.m->npages++;
            if (vibeos_uaccess_copy(s.m->page[i], (const void *)(uintptr_t)(uptr + i * 4096ull), chunk) != 0) {
                r = -VIBEOS_EFAULT;
            }
        }
    }
    if (r == 0) {
        s.nonblock = (f->flags & VIBEOS_O_NONBLOCK) != 0u;
        s.notify_tgid = 0;
        r = linux_wait_ready(mq_send_look, &s, s.nonblock ? 0 : ticks, 0);
        /* Done is 1; a deadline that passed with the queue still full is
         * ETIMEDOUT, as Linux answers mq_timedsend. */
        r = r == 1 ? 0 : r == 0 ? -VIBEOS_ETIMEDOUT : r;
        if (s.notify_tgid != 0u) {
            mq_notify_send(s.notify_tgid, s.notify_sig, s.notify_value);
        }
    }
    if (s.m) {
        mq_msg_free(s.m);   /* never queued: refused, timed out or interrupted */
    }
    vibeos_file_put(f);
    return r;
}

typedef struct {
    int q;
    int nonblock;
    int counted;
    linux_mq_msg_t *m;
} mq_recv_t;

static long mq_recv_look(void *ctx) {
    mq_recv_t *s = (mq_recv_t *)ctx;
    linux_mq_t *q = &g_mq[s->q];

    linux_ipc_lock();
    if (q->head) {
        s->m = q->head;
        q->head = s->m->next;
        if (q->head == 0) {
            q->tail = 0;
        }
        q->curmsgs--;
        q->bytes -= s->m->len;
        if (s->counted) {
            q->receivers--;
            s->counted = 0;
        }
        linux_ipc_unlock();
        ks_wake_waiters();   /* there is room for a sender now */
        return 1;
    }
    if (!s->counted && !s->nonblock) {
        q->receivers++;   /* a sender owes no notification while somebody waits */
        s->counted = 1;
    }
    linux_ipc_unlock();
    return s->nonblock ? -VIBEOS_EAGAIN : 0;
}

static long linux_sys_mq_timedreceive(uint64_t fd, uint64_t uptr, uint64_t len, uint64_t prio_uptr,
                                      uint64_t ts_uptr) {
    vibeos_file_t *f;
    mq_recv_t s;
    int64_t ticks;
    uint32_t i;
    long r = 0;

    if ((r = mq_deadline(ts_uptr, &ticks)) != 0) {
        return r;
    }
    if (!(f = mq_file(fd, &r))) {
        return r;
    }
    if ((f->flags & VIBEOS_O_ACCMODE) == VIBEOS_O_WRONLY) {
        vibeos_file_put(f);
        return -VIBEOS_EBADF;
    }
    s.q = mq_of(f);
    linux_ipc_lock();
    r = (int64_t)len < g_mq[s.q].msgsize ? -VIBEOS_EMSGSIZE : 0;   /* before anything waits */
    linux_ipc_unlock();
    if (r != 0) {
        vibeos_file_put(f);
        return r;
    }
    s.nonblock = (f->flags & VIBEOS_O_NONBLOCK) != 0u;
    s.counted = 0;
    s.m = 0;
    r = linux_wait_ready(mq_recv_look, &s, s.nonblock ? 0 : ticks, 0);
    if (s.counted) {
        linux_ipc_lock();
        g_mq[s.q].receivers--;
        linux_ipc_unlock();
    }
    if (r == 1) {
        /* Off the queue already: a buffer that faults now loses it, as on
         * Linux, which copies after taking it too. */
        r = (long)s.m->len;
        if (s.m->len != 0u && !linux_user_ok(uptr, s.m->len, 1)) {
            r = -VIBEOS_EFAULT;
        }
        for (i = 0; r >= 0 && i < s.m->npages; i++) {
            uint64_t chunk = s.m->len - i * 4096u > 4096u ? 4096u : s.m->len - i * 4096u;

            if (vibeos_uaccess_copy((void *)(uintptr_t)(uptr + i * 4096ull), s.m->page[i], chunk) != 0) {
                r = -VIBEOS_EFAULT;
            }
        }
        if (r >= 0 && prio_uptr != 0u && linux_ipc_out(prio_uptr, &s.m->prio, 4u) != 0) {
            r = -VIBEOS_EFAULT;
        }
        mq_msg_free(s.m);
    } else if (r == 0) {
        r = -VIBEOS_ETIMEDOUT;
    }
    vibeos_file_put(f);
    return r;
}

/* ---- mq_notify, mq_getsetattr ---------------------------------------------- */

static long linux_sys_mq_notify(uint64_t fd, uint64_t sev_uptr) {
    linux_sigevent_t ev;
    vibeos_file_t *f;
    uint32_t me = ks_id(ks_current())->tgid;
    long r = 0;
    int q;

    if (sev_uptr != 0u) {
        if (linux_ipc_in(&ev, sev_uptr, sizeof(ev)) != 0) {
            return -VIBEOS_EFAULT;
        }
        if (ev.sigev_notify == LINUX_SIGEV_THREAD) {
            return -VIBEOS_EINVAL;   /* a netlink socket's to deliver: there is none */
        }
        if (ev.sigev_notify != LINUX_SIGEV_SIGNAL && ev.sigev_notify != LINUX_SIGEV_NONE) {
            return -VIBEOS_EINVAL;
        }
        if (ev.sigev_notify == LINUX_SIGEV_SIGNAL &&
            (ev.sigev_signo <= 0 || ev.sigev_signo > (int32_t)VIBEOS_SIG_MAX)) {
            return -VIBEOS_EINVAL;
        }
    }
    if (!(f = mq_file(fd, &r))) {
        return r;
    }
    q = mq_of(f);
    linux_ipc_lock();
    if (sev_uptr == 0u) {
        if (g_mq[q].notify_tgid == me) {
            g_mq[q].notify_tgid = 0;   /* the caller's, taken back; anybody else's stays */
        }
    } else if (g_mq[q].notify_tgid != 0u) {
        r = -VIBEOS_EBUSY;
    } else {
        g_mq[q].notify_tgid = me;
        g_mq[q].notify_how = (uint32_t)ev.sigev_notify;
        g_mq[q].notify_sig = (uint32_t)ev.sigev_signo;
        g_mq[q].notify_value = ev.sigev_value;
    }
    linux_ipc_unlock();
    vibeos_file_put(f);
    return r;
}

static long linux_sys_mq_getsetattr(uint64_t fd, uint64_t new_uptr, uint64_t old_uptr) {
    linux_mq_attr_t in, out;
    vibeos_file_t *f;
    uint32_t i;
    long r = 0;
    int q;

    if (new_uptr != 0u) {
        if (linux_ipc_in(&in, new_uptr, sizeof(in)) != 0) {
            return -VIBEOS_EFAULT;
        }
        if (in.mq_flags & ~(int64_t)VIBEOS_O_NONBLOCK) {
            return -VIBEOS_EINVAL;   /* only O_NONBLOCK is the description's to change */
        }
    }
    if (!(f = mq_file(fd, &r))) {
        return r;
    }
    q = mq_of(f);
    for (i = 0; i < sizeof(out); i++) {
        ((unsigned char *)&out)[i] = 0;
    }
    linux_ipc_lock();
    out.mq_maxmsg = g_mq[q].maxmsg;
    out.mq_msgsize = g_mq[q].msgsize;
    out.mq_curmsgs = g_mq[q].curmsgs;
    /* The description's flags in one step, the old ones from that same step:
     * F_SETFL and FIONBIO change them without this lock, and a read-modify-
     * write here lost their update or they lost this one (M-112, M-101). */
    out.mq_flags = (new_uptr != 0u
                        ? vibeos_file_change_flags(f, VIBEOS_O_NONBLOCK,
                                                   (uint32_t)in.mq_flags & VIBEOS_O_NONBLOCK)
                        : __atomic_load_n(&f->flags, __ATOMIC_ACQUIRE)) & VIBEOS_O_NONBLOCK;
    linux_ipc_unlock();
    vibeos_file_put(f);
    if (old_uptr != 0u && linux_ipc_out(old_uptr, &out, sizeof(out)) != 0) {
        return -VIBEOS_EFAULT;
    }
    return 0;
}

/* ---- /proc/sys/fs/mqueue ------------------------------------------------------ */

static int mq_set(int64_t *where, int64_t lo, int64_t hi, const char *buf, uint32_t len) {
    int64_t v;

    if (linux_ipc_parse(buf, len, &v) != 0 || v < lo || v > hi) {
        return -VIBEOS_EINVAL;
    }
    linux_ipc_lock();
    *where = v;
    linux_ipc_unlock();
    return 0;
}

#define MQ_TUNABLE(name, var, lo, hi) \
    void linux_mq_##name##_text(linux_ipc_put_t put, void *o) { \
        linux_ipc_putn(put, o, var, 0u, 10u); \
        put(o, "\n", 1u); \
    } \
    int linux_mq_##name##_write(const char *buf, uint32_t len) { \
        return mq_set(&var, lo, hi, buf, len); \
    }

MQ_TUNABLE(msg_max, g_mq_msg_max, 1, LINUX_MQ_HARD_MSGMAX)
MQ_TUNABLE(msgsize_max, g_mq_msgsize_max, 128, LINUX_MQ_MSGSIZE_CAP)
MQ_TUNABLE(msg_default, g_mq_msg_default, 1, LINUX_MQ_HARD_MSGMAX)
MQ_TUNABLE(msgsize_default, g_mq_msgsize_default, 128, LINUX_MQ_MSGSIZE_CAP)
MQ_TUNABLE(queues_max, g_mq_queues_max, 0, 0x7fffffff)

/* Forgotten, not released: see linux_ipc_shm_reset. */
void linux_mq_reset(void) {
    uint32_t i;

    for (i = 0; i < LINUX_MQ_QUEUES; i++) {
        g_mq[i].used = 0;
        g_mq[i].head = 0;
        g_mq[i].tail = 0;
    }
    g_mq_msg_max = g_mq_msg_default = 10;
    g_mq_msgsize_max = g_mq_msgsize_default = 8192;
    g_mq_queues_max = 256;
}

/* ---- the syscalls this file implements --------------------------------------- */
#define LINUX_MQ_SYSCALLS(X) \
    X(240, mq_open,         MQ_OPEN, NOPTR, linux_sys_mq_open(ARG(0), ARG(1), ARG(2), ARG(3))) \
    X(241, mq_unlink,       MQ_OPEN, NOPTR, linux_sys_mq_unlink(ARG(0))) \
    X(242, mq_timedsend,    MQ_IO,   NOPTR, linux_sys_mq_timedsend(ARG(0), ARG(1), ARG(2), ARG(3), ARG(4))) \
    X(243, mq_timedreceive, MQ_IO,   NOPTR, linux_sys_mq_timedreceive(ARG(0), ARG(1), ARG(2), ARG(3), ARG(4))) \
    X(244, mq_notify,       MQ_IO,   NOPTR, linux_sys_mq_notify(ARG(0), ARG(1))) \
    X(245, mq_getsetattr,   MQ_IO,   NOPTR, linux_sys_mq_getsetattr(ARG(0), ARG(1), ARG(2)))

LINUX_DEFINE_SYSCALLS(mq, LINUX_MQ_SYSCALLS)
