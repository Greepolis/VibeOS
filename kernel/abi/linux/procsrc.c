/* What /proc says about processes (docs/abi/ L2 step 6): the source that
 * kernel/fs/procfs.c asks, written against the kernel services like a handler.
 *
 * The filesystem formats; this gathers. Everything is copied out under the
 * locks that hold it and handed over after they are released, so the
 * filesystem never runs under the scheduler's lock or a process's.
 *
 * Another process's state is reached the way prlimit reaches it - found under
 * the scheduler's lock - and then held by a reference taken there, which exit
 * cannot pull away: it clears a task's pointer to its state under that lock
 * before it gives its own reference back. Its regions are walked under its
 * own address-space lock. Its page tables are not walked at all: they go with
 * the address space, earlier in exit, and nothing here can hold them, so the
 * resident size is the asking process's own and 0 for any other. */

#include "linux_internal.h"
#include "vibeos/procfs.h"
#include "vibeos/devfs.h"
#include "vibeos/vma.h"
#include "vibeos/random.h"

#define SRC_USER_HZ 100u

/* The name is copied bounded by the snapshot's array alone, which is safe while
 * the task's is no shorter. Said here rather than tested at every byte: with
 * both sixteen, the second bound could never fail, and code scanning said so. */
_Static_assert(sizeof(((vibeos_procfs_proc_t *)0)->comm) <= sizeof(((vibeos_task_t *)0)->comm),
               "a task's name is shorter than /proc's copy of it");

static uint64_t src_user_hz(uint64_t ticks) {
    return ticks * SRC_USER_HZ / ks_hz();
}

static uint32_t src_self(void) {
    int me = ks_current();
    return (me >= 0 && ks_id(me)->is_user) ? ks_id(me)->tgid : 0u;
}

/* The slot of the process `pid` - its leader, which is what /proc/<pid> is -
 * or -1. Under the scheduler's lock. */
static int src_leader(uint32_t pid) {
    int s = ks_task_by_tid(pid);
    vibeos_task_state_t st;

    if (s < 0) {
        return -1;
    }
    st = vibeos_task_state((uint32_t)s);
    if (st == VIBEOS_TASK_FREE || st == VIBEOS_TASK_SETUP || !ks_id(s)->is_user ||
        ks_id(s)->tgid != pid) {
        return -1;
    }
    return s;
}

/* The slot of the thread `tid` - any task of a user process, its leader
 * included - or -1. Under the scheduler's lock. */
static int src_thread(uint32_t tid) {
    int s = ks_task_by_tid(tid);
    vibeos_task_state_t st;

    if (s < 0) {
        return -1;
    }
    st = vibeos_task_state((uint32_t)s);
    if (st == VIBEOS_TASK_FREE || st == VIBEOS_TASK_SETUP || !ks_id(s)->is_user) {
        return -1;
    }
    return s;
}

/* The leader of the process `id` names, by its pid or by the id of any of its
 * threads - /proc/<tid> is the thread's process for everything a process
 * owns: its descriptors, its regions, its program. Under the scheduler's lock. */
static int src_owner(uint32_t id) {
    int s = src_thread(id);

    return s < 0 ? -1 : src_leader(ks_id(s)->tgid);
}

/* A reference to a slot's process state, if it still has one with a reference
 * left to share; under the scheduler's lock. Given back with
 * ks_procstate_put. */
static vibeos_procstate_t *src_ps_get(int slot) {
    vibeos_procstate_t *ps = ks_ps(slot);
    uint32_t r;

    if (!ps) {
        return 0;
    }
    r = __atomic_load_n(&ps->refs, __ATOMIC_ACQUIRE);
    do {
        if (r == 0u) {
            return 0;   /* being let go: as if it had none */
        }
    } while (!__atomic_compare_exchange_n(&ps->refs, &r, r + 1u, 0, __ATOMIC_ACQ_REL,
                                          __ATOMIC_ACQUIRE));
    return ps;
}

/* Linux numbers a set's bits from signal 1; this kernel by signal. */
static uint64_t src_sigset(uint64_t kernel_set) {
    return kernel_set >> 1;
}

/* /proc/<pid>, or /proc/<tid> and task/<tid>: what a process owns from its
 * leader, and what a thread has of its own - state, name, signals - from the
 * thread asked about, which for /proc/<pid> is the leader itself. */
static int src_proc(uint32_t pid, vibeos_procfs_proc_t *out) {
    vibeos_procstate_t *ps;
    const vibeos_task_t *t, *th;
    int s, ts, me = ks_current();
    uint32_t i, threads = 0;
    int running = 0, stopped = 0, nice;

    for (i = 0; i < sizeof(*out); i++) {
        ((unsigned char *)out)[i] = 0;
    }
    ks_lock(ks_sched_lock(), __func__);
    ts = src_thread(pid);
    s = ts >= 0 ? src_leader(ks_id(ts)->tgid) : -1;
    if (s < 0) {
        ks_unlock(ks_sched_lock());
        return -VIBEOS_ENOENT;
    }
    t = ks_id(s);
    th = ks_id(ts);
    out->pid = pid;
    out->tgid = t->tgid;
    out->ppid = t->ppid;
    out->pgid = t->pgid;
    out->sid = t->sid;
    for (i = 0; i + 1u < sizeof(out->comm) && th->comm[i]; i++) {
        out->comm[i] = th->comm[i];
    }
    out->start = src_user_hz(th->start_tick);
    out->sig_pending = src_sigset(th->sig_pending);
    out->sig_blocked = src_sigset(th->sig_blocked);
    for (i = 0; i < ks_slots(); i++) {
        vibeos_task_state_t st = vibeos_task_state(i);

        if (st == VIBEOS_TASK_FREE || st == VIBEOS_TASK_SETUP || st == VIBEOS_TASK_ZOMBIE ||
            !ks_id((int)i)->is_user || ks_id((int)i)->tgid != t->tgid) {
            continue;
        }
        threads++;
    }
    /* The state is the thread's asked about - the leader's, for /proc/<pid>,
     * as Linux gives it. It used to be "running if any thread is", so a
     * process whose main thread waited in a futex while another thread
     * watched for that, by reading this file, was running for as long as it
     * looked - LTP's futex_wait03 waited out its timeout (L2 step 6's run). */
    stopped = th->signal_stopped != 0u;
    running = vibeos_task_state((uint32_t)ts) != VIBEOS_TASK_ZOMBIE && !th->sleeping;
    out->threads = threads;
    if (threads == 0u) {
        out->state = 'Z';
    } else if (stopped) {
        out->state = 'T';
    } else {
        out->state = running ? 'R' : 'S';
    }
    /* The thread's own reference: they share one state, and a leader that has
     * ended has let go of its pointer to it while its threads have not. */
    ps = src_ps_get(ts);
    ks_unlock(ks_sched_lock());

    nice = ks_task_nice(ts);
    out->nice = nice;
    /* A thread's own time for <tid>/, the process's for <pid>/, as Linux. */
    out->utime = src_user_hz(pid != t->tgid ? linux_cpu_of_thread(ts) : linux_cpu_of_process(t->tgid));
    out->tty = VIBEOS_DEV_CONSOLE;   /* the one terminal, every process's */
    out->tty_pgid = ks_foreground_pgid();
    out->rss_limit = ~0ull;
    if (!ps) {
        return 0;   /* ended: what a zombie still has */
    }
    out->cutime = src_user_hz(__atomic_load_n(&ps->cpu_children, __ATOMIC_RELAXED));
    out->rss_limit = ps->rlim_cur[VIBEOS_RLIM_RSS];
    ks_lock(&ps->files_lock, __func__);
    out->uid[0] = ps->cred.uid;
    out->uid[1] = ps->cred.euid;
    out->uid[2] = ps->cred.suid;
    out->uid[3] = ps->cred.fsuid;
    out->gid[0] = ps->cred.gid;
    out->gid[1] = ps->cred.egid;
    out->gid[2] = ps->cred.sgid;
    out->gid[3] = ps->cred.fsgid;
    out->umask = ps->umask;
    ks_unlock(&ps->files_lock);
    for (i = 1; i < VIBEOS_NSIG; i++) {
        if (ps->sig_handler[i] == SIG_IGN_ADDR) {
            out->sig_ignored |= 1ull << (i - 1u);
        } else if (ps->sig_handler[i] != SIG_DFL_ADDR) {
            out->sig_caught |= 1ull << (i - 1u);
        }
    }
    ks_mm_lock(ps);
    {
        const vibeos_vma_t *v;
        uint32_t n = 0;

        for (v = ps->vmas.head; v && n < 4096u; v = v->next, n++) {
            out->vsize += v->len;
        }
    }
    /* Resident pages, for the asking process only: its own page tables are
     * the ones nothing can take away while it asks. */
    if (me >= 0 && ks_id(me)->tgid == out->tgid) {
        vibeos_vmspace_t vm = ks_vm(me);
        const vibeos_vma_t *v;
        uint32_t n = 0;

        for (v = ps->vmas.head; v && n < 4096u; v = v->next, n++) {
            uint64_t va;
            for (va = v->base; va < v->base + v->len && out->rss < (1ull << 20); va += 4096u) {
                out->rss += vibeos_vmspace_resident(&vm, va) == 1 ? 1u : 0u;
            }
        }
    }
    ks_mm_unlock(ps);
    ks_procstate_put(ps);
    return 0;
}

static uint32_t src_next_pid(uint32_t after) {
    uint32_t i, best = 0;

    ks_lock(ks_sched_lock(), __func__);
    for (i = 0; i < ks_slots(); i++) {
        vibeos_task_state_t st = vibeos_task_state(i);
        const vibeos_task_t *t = ks_id((int)i);

        if (st == VIBEOS_TASK_FREE || st == VIBEOS_TASK_SETUP || !t->is_user || t->pid != t->tgid) {
            continue;
        }
        if (t->pid > after && (best == 0u || t->pid < best)) {
            best = t->pid;
        }
    }
    ks_unlock(ks_sched_lock());
    return best;
}

/* task/: the threads of process `pid`, by id. */
static uint32_t src_next_tid(uint32_t pid, uint32_t after) {
    uint32_t i, best = 0;

    ks_lock(ks_sched_lock(), __func__);
    for (i = 0; i < ks_slots(); i++) {
        vibeos_task_state_t st = vibeos_task_state(i);
        const vibeos_task_t *t = ks_id((int)i);

        if (st == VIBEOS_TASK_FREE || st == VIBEOS_TASK_SETUP || st == VIBEOS_TASK_ZOMBIE ||
            !t->is_user || t->tgid != pid) {
            continue;
        }
        if (t->pid > after && (best == 0u || t->pid < best)) {
            best = t->pid;
        }
    }
    ks_unlock(ks_sched_lock());
    return best;
}

static long src_cmdline(uint32_t pid, char *buf, uint32_t cap) {
    const vibeos_image_t *img;
    uint32_t n = 0, i;
    int s;

    ks_lock(ks_sched_lock(), __func__);
    s = src_owner(pid);
    if (s >= 0 && vibeos_task_state((uint32_t)s) != VIBEOS_TASK_ZOMBIE) {
        img = ks_image(s);
        n = img->cmdline_len < (uint32_t)sizeof(img->cmdline) ? img->cmdline_len
                                                               : (uint32_t)sizeof(img->cmdline);
        n = n < cap ? n : cap;
        for (i = 0; i < n; i++) {
            buf[i] = img->cmdline[i];
        }
    }
    ks_unlock(ks_sched_lock());
    return s < 0 ? -VIBEOS_ENOENT : (long)n;
}

static long src_copy(char *buf, uint32_t cap, const char *s) {
    uint32_t n = 0;

    while (n < cap && s[n]) {   /* the bound before the byte it guards */
        buf[n] = s[n];
        n++;
    }
    return (long)n;
}

/* What a descriptor is, as its /proc/<pid>/fd link reads: the path it was
 * opened by, or Linux's spelling of a thing with no name. */
static long src_fd_name(const vibeos_file_t *f, char *buf, uint32_t cap) {
    char tmp[48];
    const char *kind = 0;
    uint64_t id = 0;
    uint32_t n = 0, k;

    if (f->path[0]) {
        return src_copy(buf, cap, f->path);
    }
    if (f->ops == &vibeos_fops_pipe) {
        kind = "pipe:[";
        id = (uint64_t)(uint32_t)f->pipe;
    } else if (f->ops == &vibeos_fops_socket) {
        kind = "socket:[";
        id = (uint64_t)(uint32_t)f->sock;
    } else if (f->ops == &vibeos_fops_unix) {
        kind = "socket:[";
        id = 0x30000u + (uint64_t)(uint32_t)f->ux;   /* what fstat says its inode is */
    } else if (f->ops == &vibeos_fops_console) {
        return src_copy(buf, cap, "/dev/console");
    } else {
        return src_copy(buf, cap, f->ops == &vibeos_fops_pidfd ? "anon_inode:[pidfd]"
                                  : f->ops == &vibeos_fops_eventfd ? "anon_inode:[eventfd]"
                                  : f->ops == &vibeos_fops_timerfd ? "anon_inode:[timerfd]"
                                  : f->ops == &linux_fops_signalfd ? "anon_inode:[signalfd]"
                                  : f->ops == &linux_fops_epoll ? "anon_inode:[eventpoll]"
                                  : f->ops == &linux_fops_inotify ? "anon_inode:inotify"
                                  : "anon_inode:[file]");
    }
    for (k = 0; kind[k]; k++) {
        tmp[n++] = kind[k];
    }
    {
        char d[20];
        uint32_t m = 0;
        do {
            d[m++] = (char)('0' + id % 10u);
            id /= 10u;
        } while (id != 0u);
        while (m > 0u) {
            tmp[n++] = d[--m];
        }
    }
    tmp[n++] = ']';
    tmp[n] = 0;
    return src_copy(buf, cap, tmp);
}

static long src_link(uint32_t pid, uint32_t which, uint32_t fd, char *buf, uint32_t cap) {
    vibeos_procstate_t *ps;
    long r = -VIBEOS_ENOENT;
    int s;

    ks_lock(ks_sched_lock(), __func__);
    s = src_owner(pid);
    if (s >= 0 && which == VIBEOS_PROCFS_LINK_EXE) {
        const char *exe = ks_image(s)->exe_path;
        char path[VIBEOS_PATH_MAX];
        uint32_t i;

        for (i = 0; i + 1u < (uint32_t)sizeof(path) && exe[i]; i++) {
            path[i] = exe[i];
        }
        path[i] = 0;
        ks_unlock(ks_sched_lock());
        return i ? src_copy(buf, cap, path) : -VIBEOS_ENOENT;
    }
    ps = s >= 0 ? src_ps_get(s) : 0;
    ks_unlock(ks_sched_lock());
    if (!ps) {
        return -VIBEOS_ENOENT;
    }
    ks_lock(&ps->files_lock, __func__);
    if (which == VIBEOS_PROCFS_LINK_CWD) {
        r = src_copy(buf, cap, ps->cwd[0] ? ps->cwd : "/");
    } else if (which == VIBEOS_PROCFS_LINK_ROOT) {
        r = src_copy(buf, cap, ps->root[0] ? ps->root : "/");
    } else if (which == VIBEOS_PROCFS_LINK_FD) {
        const vibeos_file_t *f = vibeos_fdtable_get(&ps->files, fd);
        r = f ? src_fd_name(f, buf, cap) : -VIBEOS_ENOENT;
    }
    ks_unlock(&ps->files_lock);
    ks_procstate_put(ps);
    return r;
}

static int src_next_fd(uint32_t pid, uint32_t from, uint32_t *fd) {
    vibeos_procstate_t *ps;
    int s, top, r = -1;
    uint32_t i;

    ks_lock(ks_sched_lock(), __func__);
    s = src_owner(pid);
    ps = s >= 0 ? src_ps_get(s) : 0;
    ks_unlock(ks_sched_lock());
    if (!ps) {
        return -1;
    }
    ks_lock(&ps->files_lock, __func__);
    top = vibeos_fdtable_highest(&ps->files);
    for (i = from; top >= 0 && i <= (uint32_t)top; i++) {
        if (vibeos_fdtable_get(&ps->files, i)) {
            *fd = i;
            r = 0;
            break;
        }
    }
    ks_unlock(&ps->files_lock);
    ks_procstate_put(ps);
    return r;
}

static int src_map(uint32_t pid, uint32_t index, vibeos_procfs_map_t *out) {
    vibeos_procstate_t *ps;
    const vibeos_vma_t *v;
    uint64_t sp = 0;
    uint32_t n = 0, i;
    int s, r = -1;

    ks_lock(ks_sched_lock(), __func__);
    s = src_owner(pid);
    if (s >= 0) {
        sp = ks_image(s)->user_sp;
    }
    ps = s >= 0 ? src_ps_get(s) : 0;
    ks_unlock(ks_sched_lock());
    if (!ps) {
        return -1;
    }
    for (i = 0; i < sizeof(*out); i++) {
        ((unsigned char *)out)[i] = 0;
    }
    ks_mm_lock(ps);
    for (v = ps->vmas.head; v && n < 4096u; v = v->next, n++) {
        const char *name = 0;

        if (n != index) {
            continue;
        }
        out->start = v->base;
        out->end = v->base + v->len;
        out->offset = v->backing_offset;
        out->prot = ((v->prot & VIBEOS_PROT_READ) ? VIBEOS_PROCFS_MAP_R : 0u) |
                    ((v->prot & VIBEOS_PROT_WRITE) ? VIBEOS_PROCFS_MAP_W : 0u) |
                    ((v->prot & VIBEOS_PROT_EXEC) ? VIBEOS_PROCFS_MAP_X : 0u) |
                    ((v->prot & VIBEOS_PROT_SHARED) ? VIBEOS_PROCFS_MAP_SHARED : 0u);
        if (v->backing == VIBEOS_BACKING_ANON && ps->brk_cur > v->base && ps->brk_cur <= out->end) {
            name = "[heap]";
        } else if (sp > v->base && sp <= out->end) {
            name = "[stack]";
        }
        for (i = 0; name && name[i] && i + 1u < sizeof(out->name); i++) {
            out->name[i] = name[i];
        }
        r = 0;
        break;
    }
    ks_mm_unlock(ps);
    ks_procstate_put(ps);
    return r;
}

/* Linux since 5.18 says 256 - a full pool - once the generator is seeded, and
 * what has been credited before then. One function for the ioctl and the file,
 * which LTP's ioctl07 compares. */
uint32_t linux_entropy_avail(void) {
    uint64_t c;

    if (vibeos_random_ready()) {
        return 256u;
    }
    c = vibeos_random_credited();
    return c > 255u ? 255u : (uint32_t)c;
}

static uint64_t src_uptime_ms(void) {
    return ks_ticks() * 1000u / ks_hz();
}

void linux_procfs_bind(vibeos_procfs_t *pf) {
    pf->self = src_self;
    pf->proc = src_proc;
    pf->next_pid = src_next_pid;
    pf->next_tid = src_next_tid;
    pf->cmdline = src_cmdline;
    pf->link = src_link;
    pf->next_fd = src_next_fd;
    pf->map = src_map;
    pf->uptime_ms = src_uptime_ms;
    pf->entropy_avail = linux_entropy_avail;
    linux_inotify_limits(&pf->inotify_queued, &pf->inotify_instances, &pf->inotify_watches);
    pf->version = "Linux version " VIBEOS_LINUX_RELEASE " (vibeos@vibeos) (gcc) #1 SMP PREEMPT";
}
