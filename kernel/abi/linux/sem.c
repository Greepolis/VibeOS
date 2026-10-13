/* System V semaphores (docs/abi/ L7 step 2), over what ipc.c shares.
 *
 * A set is up to LINUX_SEMMSL counters, and semop applies a list of changes
 * to them all at once or not at all: every operation in the list is tried on
 * a copy, and the set changes only if none of them would have to wait. That
 * is the whole of the call's atomicity, and it is why the list is copied in
 * before the lock is taken.
 *
 * A waiter is counted on the one semaphore that stopped it - semncnt if it
 * wanted to decrease it, semzcnt if it waited for zero - as Linux counts
 * since 4.x, and uncounted when it stops waiting for any reason.
 *
 * SEM_UNDO keeps, for each process and semaphore, the sum of what the process
 * changed it by, negated; when the process's last thread leaves, each sum is
 * added back (linux_ipc_exit). A value set outright with SETVAL or SETALL
 * clears every process's sum for it, as Linux does. The table is the whole
 * machine's, keyed by process, so a fork's child starts with none and an exec
 * keeps them. */

#include "linux_internal.h"
#include "vibeos/procfs.h"

typedef struct {
    uint16_t val;
    uint16_t ncnt;       /* waiting for it to rise */
    uint16_t zcnt;       /* waiting for it to be zero */
    uint32_t pid;        /* the last process to change it */
} linux_sem_t;

typedef struct {
    uint32_t nsems;
    int64_t otime;
    linux_sem_t sem[LINUX_SEMMSL];
} linux_semset_t;

typedef struct {
    uint32_t tgid;       /* 0: a free entry */
    uint32_t id;
    uint16_t num;
    int32_t adj;
} linux_semundo_t;

#define LINUX_SEM_UNDOS 512u

static linux_ipc_obj_t g_sem_obj[LINUX_IPC_SLOTS];
static linux_semset_t g_sem[LINUX_IPC_SLOTS];
static linux_semundo_t g_undo[LINUX_SEM_UNDOS];
static linux_ipc_ns_t g_sem_ns = {0, -1, LINUX_IPC_SLOTS, -1};

static uint32_t sem_in_use(void) {
    uint32_t s, n = 0;

    for (s = 0; s < LINUX_IPC_SLOTS; s++) {
        n += g_sem_obj[s].used ? g_sem[s].nsems : 0u;
    }
    return n;
}

static long linux_sys_semget(uint64_t key, uint64_t nsems_arg, uint64_t flg) {
    const int32_t nsems = VIBEOS_ARG_INT(nsems_arg);
    vibeos_cred_t c;
    long r;
    int s;

    if (nsems < 0 || nsems > (int32_t)LINUX_SEMMSL) {
        return -VIBEOS_EINVAL;
    }
    linux_cred(&c);
    linux_ipc_lock();
    s = linux_ipc_lookup(g_sem_obj, LINUX_IPC_SLOTS, (int32_t)(uint32_t)key);
    if (s >= 0) {
        if ((flg & LINUX_IPC_CREAT) && (flg & LINUX_IPC_EXCL)) {
            r = -VIBEOS_EEXIST;
        } else if ((uint32_t)nsems > g_sem[s].nsems) {
            r = -VIBEOS_EINVAL;   /* asks for more than the set has */
        } else {
            r = linux_ipc_may(&g_sem_obj[s], (uint32_t)flg, &c);
            if (r == 0) {
                r = (long)g_sem_obj[s].id;
            }
        }
    } else if (!(flg & LINUX_IPC_CREAT) && (int32_t)(uint32_t)key != LINUX_IPC_PRIVATE) {
        r = -VIBEOS_ENOENT;
    } else if (nsems == 0) {
        r = -VIBEOS_EINVAL;   /* a new set has at least one */
    } else if (sem_in_use() + (uint32_t)nsems > LINUX_SEMMNS) {
        r = -VIBEOS_ENOSPC;
    } else {
        r = linux_ipc_new(&g_sem_ns, g_sem_obj, LINUX_IPC_SLOTS, (int32_t)(uint32_t)key, flg, &c);
        if (r >= 0) {
            linux_semset_t *set = &g_sem[r];
            uint32_t i;

            set->nsems = (uint32_t)nsems;
            set->otime = 0;
            for (i = 0; i < LINUX_SEMMSL; i++) {
                set->sem[i].val = set->sem[i].ncnt = set->sem[i].zcnt = 0;
                set->sem[i].pid = 0;
            }
            r = (long)g_sem_obj[r].id;
        }
    }
    linux_ipc_unlock();
    return r;
}

/* ---- undo ------------------------------------------------------------------- */

/* The entry for (tgid, id, num), made if `make`; 0 if there is none or no room. */
static linux_semundo_t *undo_of(uint32_t tgid, uint32_t id, uint16_t num, int make) {
    linux_semundo_t *free = 0;
    uint32_t i;

    for (i = 0; i < LINUX_SEM_UNDOS; i++) {
        if (g_undo[i].tgid == tgid && g_undo[i].id == id && g_undo[i].num == num) {
            return &g_undo[i];
        }
        if (!free && g_undo[i].tgid == 0u) {
            free = &g_undo[i];
        }
    }
    if (!make || !free) {
        return 0;
    }
    free->tgid = tgid;
    free->id = id;
    free->num = num;
    free->adj = 0;
    return free;
}

/* Forget every process's adjustment for semaphore `num` of set `id`, or for
 * the whole set when num is -1. */
static void undo_clear(uint32_t id, int32_t num) {
    uint32_t i;

    for (i = 0; i < LINUX_SEM_UNDOS; i++) {
        if (g_undo[i].tgid != 0u && g_undo[i].id == id && (num < 0 || g_undo[i].num == (uint16_t)num)) {
            g_undo[i].tgid = 0;
        }
    }
}

/* A process has ended: each of its adjustments is added back, the result
 * kept inside [0, SEMVMX] as Linux keeps it, and whoever waits looks again. */
void linux_ipc_exit(uint32_t tgid) {
    uint32_t i;
    int any = 0;

    if (tgid == 0u) {
        return;
    }
    linux_ipc_lock();
    for (i = 0; i < LINUX_SEM_UNDOS; i++) {
        linux_semundo_t *u = &g_undo[i];
        int s;

        if (u->tgid != tgid) {
            continue;
        }
        s = linux_ipc_slot(g_sem_obj, LINUX_IPC_SLOTS, u->id);
        if (s >= 0 && u->num < g_sem[s].nsems) {
            int32_t v = (int32_t)g_sem[s].sem[u->num].val + u->adj;

            g_sem[s].sem[u->num].val = (uint16_t)(v < 0 ? 0 : v > LINUX_SEMVMX ? LINUX_SEMVMX : v);
            g_sem[s].sem[u->num].pid = tgid;
            any = 1;
        }
        u->tgid = 0;
    }
    linux_ipc_unlock();
    if (any) {
        ks_wake_waiters();
    }
}

/* ---- semop ------------------------------------------------------------------ */

typedef struct {
    uint64_t id;
    const linux_sembuf_t *ops;
    uint32_t n;
    int alter;
    vibeos_cred_t cred;
    uint32_t tgid;
    int counted_slot;    /* where this waiter is counted, -1 for nowhere */
    uint32_t counted_num;
    int counted_zero;
} sem_op_t;

static void sem_uncount(sem_op_t *m) {
    if (m->counted_slot >= 0) {
        linux_sem_t *x = &g_sem[m->counted_slot].sem[m->counted_num];

        if (m->counted_zero) {
            x->zcnt = (uint16_t)(x->zcnt - (x->zcnt > 0u));
        } else {
            x->ncnt = (uint16_t)(x->ncnt - (x->ncnt > 0u));
        }
        m->counted_slot = -1;
    }
}

/* Try the whole list on a copy of the values. 0 if it went through and the set
 * was changed; 1 if operation *at would have to wait; a negated errno. */
static long sem_try(int s, sem_op_t *m, uint32_t *at) {
    linux_semset_t *set = &g_sem[s];
    /* The set's size read once: the copy is filled to it here and written back
     * to it at the end, with calls between that write the undo table - and the
     * analyzer, rightly, will not assume a global they could reach is the same
     * on both sides (clang's core.uninitialized.Assign on the write-back). */
    const uint32_t nsems = set->nsems < LINUX_SEMMSL ? set->nsems : LINUX_SEMMSL;
    uint16_t val[LINUX_SEMMSL];
    uint32_t i;

    for (i = 0; i < nsems; i++) {
        val[i] = set->sem[i].val;
    }
    for (i = 0; i < m->n; i++) {
        const linux_sembuf_t *op = &m->ops[i];
        int32_t v = (int32_t)val[op->sem_num] + op->sem_op;

        if (op->sem_op == 0) {
            if (val[op->sem_num] != 0u) {
                *at = i;
                return (op->sem_flg & LINUX_IPC_NOWAIT) ? -VIBEOS_EAGAIN : 1;
            }
            continue;
        }
        if (v < 0) {
            *at = i;
            return (op->sem_flg & LINUX_IPC_NOWAIT) ? -VIBEOS_EAGAIN : 1;
        }
        if (v > LINUX_SEMVMX) {
            return -VIBEOS_ERANGE;
        }
        if (op->sem_flg & LINUX_SEM_UNDO) {
            /* The adjustment has to stay inside what a value can be too. */
            linux_semundo_t *u = undo_of(m->tgid, g_sem_obj[s].id, op->sem_num, 0);
            int32_t adj = (u ? u->adj : 0) - op->sem_op;

            if (adj < -LINUX_SEMAEM - 1 || adj > LINUX_SEMAEM) {
                return -VIBEOS_ERANGE;
            }
        }
        val[op->sem_num] = (uint16_t)v;
    }
    /* Through: the undo entries first, since making one can fail and nothing
     * may have changed when it does. */
    for (i = 0; i < m->n; i++) {
        if ((m->ops[i].sem_flg & LINUX_SEM_UNDO) && m->ops[i].sem_op != 0 &&
            !undo_of(m->tgid, g_sem_obj[s].id, m->ops[i].sem_num, 1)) {
            return -VIBEOS_ENOSPC;
        }
    }
    for (i = 0; i < m->n; i++) {
        const linux_sembuf_t *op = &m->ops[i];

        if (op->sem_op != 0 && (op->sem_flg & LINUX_SEM_UNDO)) {
            undo_of(m->tgid, g_sem_obj[s].id, op->sem_num, 1)->adj -= op->sem_op;
        }
        set->sem[op->sem_num].pid = m->tgid;   /* Linux's sempid: a wait for zero sets it too */
    }
    for (i = 0; i < nsems; i++) {
        set->sem[i].val = val[i];
    }
    set->otime = linux_ipc_now();
    return 0;
}

static long sem_look(void *ctx) {
    sem_op_t *m = (sem_op_t *)ctx;
    uint32_t at = 0;
    long r;
    int s;

    linux_ipc_lock();
    s = linux_ipc_slot(g_sem_obj, LINUX_IPC_SLOTS, m->id);
    if (s < 0) {
        m->counted_slot = -1;   /* the counts went with the set */
        linux_ipc_unlock();
        return -VIBEOS_EIDRM;
    }
    sem_uncount(m);
    r = linux_ipc_may(&g_sem_obj[s], m->alter ? 0222u : 0444u, &m->cred);
    if (r == 0) {
        r = sem_try(s, m, &at);
    }
    if (r == 1) {
        /* Counted on the semaphore that stopped it, until it looks again. */
        linux_sem_t *x = &g_sem[s].sem[m->ops[at].sem_num];

        m->counted_slot = s;
        m->counted_num = m->ops[at].sem_num;
        m->counted_zero = m->ops[at].sem_op == 0;
        if (m->counted_zero) {
            x->zcnt++;
        } else {
            x->ncnt++;
        }
        linux_ipc_unlock();
        return 0;
    }
    linux_ipc_unlock();
    if (r == 0) {
        ks_wake_waiters();   /* what it changed may let somebody else through */
        return 1;
    }
    return r;
}

static long linux_semtimedop(uint64_t id, uint64_t sops, uint64_t nsops, uint64_t tsp) {
    linux_sembuf_t *ops;
    sem_op_t m;
    int64_t ticks = -1;
    uint32_t i;
    long r;
    int s;

    if ((int64_t)(int32_t)(uint32_t)id < 0 || nsops < 1u) {
        return -VIBEOS_EINVAL;
    }
    if (nsops > LINUX_SEMOPM) {
        return -VIBEOS_E2BIG;
    }
    if (tsp != 0u) {
        linux_timespec_t ts;

        if (linux_ipc_in(&ts, tsp, sizeof(ts)) != 0) {
            return -VIBEOS_EFAULT;
        }
        if ((ticks = linux_ticks_of(&ts)) < 0) {
            return -VIBEOS_EINVAL;
        }
    }
    ops = (linux_sembuf_t *)ks_page_alloc();   /* SEMOPM of them fit in one */
    if (!ops) {
        return -VIBEOS_ENOMEM;
    }
    if (linux_ipc_in(ops, sops, (uint32_t)(nsops * sizeof(linux_sembuf_t))) != 0) {
        ks_page_free(ops, "semop list");
        return -VIBEOS_EFAULT;
    }
    m.id = id;
    m.ops = ops;
    m.n = (uint32_t)nsops;
    m.alter = 0;
    m.tgid = ks_id(ks_current())->tgid;
    m.counted_slot = -1;
    linux_cred(&m.cred);
    for (i = 0; i < m.n; i++) {
        m.alter |= ops[i].sem_op != 0;
    }
    /* Linux's order: the number, then each semaphore's index against the set
     * (EFBIG), then the permission - all before the first attempt. */
    linux_ipc_lock();
    s = linux_ipc_slot(g_sem_obj, LINUX_IPC_SLOTS, id);
    r = s < 0 ? s : 0;
    for (i = 0; r == 0 && i < m.n; i++) {
        if (ops[i].sem_num >= g_sem[s].nsems) {
            r = -VIBEOS_EFBIG;
        }
    }
    if (r == 0) {
        r = linux_ipc_may(&g_sem_obj[s], m.alter ? 0222u : 0444u, &m.cred);
    }
    linux_ipc_unlock();
    if (r == 0) {
        r = linux_wait_ready(sem_look, &m, ticks, 0);
        if (m.counted_slot >= 0) {
            linux_ipc_lock();
            if (linux_ipc_slot(g_sem_obj, LINUX_IPC_SLOTS, id) == m.counted_slot) {
                sem_uncount(&m);
            }
            linux_ipc_unlock();
        }
        /* Done is 1; a time that ran out with the operation still waiting
         * is EAGAIN, as Linux answers semtimedop. */
        r = r == 1 ? 0 : r == 0 ? -VIBEOS_EAGAIN : r;
    }
    ks_page_free(ops, "semop list");
    return r;
}

static long linux_sys_semop(uint64_t id, uint64_t sops, uint64_t nsops) {
    return linux_semtimedop(id, sops, nsops, 0);
}

static long linux_sys_semtimedop(uint64_t id, uint64_t sops, uint64_t nsops, uint64_t tsp) {
    return linux_semtimedop(id, sops, nsops, tsp);
}

/* ---- semctl ----------------------------------------------------------------- */

static void sem_stat_of(int s, linux_semid64_ds_t *out) {
    uint8_t *raw = (uint8_t *)out;
    uint32_t i;

    for (i = 0; i < (uint32_t)sizeof(*out); i++) {
        raw[i] = 0;
    }
    linux_ipc_perm_out(&g_sem_obj[s], &out->sem_perm);
    out->sem_otime = g_sem[s].otime;
    out->sem_ctime = g_sem_obj[s].ctime;
    out->sem_nsems = g_sem[s].nsems;
}

static long linux_sys_semctl(uint64_t id, uint64_t num_arg, uint64_t cmd_arg, uint64_t arg) {
    const int32_t num = VIBEOS_ARG_INT(num_arg), cmd = VIBEOS_ARG_INT(cmd_arg);
    uint16_t *vals = 0;
    vibeos_cred_t c;
    linux_semid64_ds_t ds;
    long r = 0;
    int s;

    linux_cred(&c);
    switch (cmd) {
        case LINUX_IPC_INFO:
        case LINUX_SEM_INFO: {
            linux_seminfo_t si;

            si.semmni = (int32_t)(g_sem_ns.max < LINUX_IPC_SLOTS ? g_sem_ns.max : LINUX_IPC_SLOTS);
            si.semmns = LINUX_SEMMNS;
            si.semmsl = LINUX_SEMMSL;
            si.semopm = LINUX_SEMOPM;
            si.semvmx = LINUX_SEMVMX;
            si.semmnu = LINUX_SEMMNS;
            si.semmap = LINUX_SEMMNS;
            si.semume = LINUX_SEMOPM;
            linux_ipc_lock();
            if (cmd == LINUX_SEM_INFO) {
                /* SEM_INFO reports use where IPC_INFO reports limits. */
                si.semusz = (int32_t)linux_ipc_live(g_sem_obj, LINUX_IPC_SLOTS);
                si.semaem = (int32_t)sem_in_use();
            } else {
                si.semusz = (int32_t)sizeof(linux_semundo_t);
                si.semaem = LINUX_SEMAEM;
            }
            r = linux_ipc_max_index(g_sem_obj, LINUX_IPC_SLOTS);
            linux_ipc_unlock();
            return linux_ipc_out(arg, &si, sizeof(si)) != 0 ? -VIBEOS_EFAULT : r;
        }
        case LINUX_IPC_STAT:
        case LINUX_SEM_STAT:
        case LINUX_SEM_STAT_ANY:
            linux_ipc_lock();
            s = cmd == LINUX_IPC_STAT ? linux_ipc_slot(g_sem_obj, LINUX_IPC_SLOTS, id)
                                      : linux_ipc_index(g_sem_obj, LINUX_IPC_SLOTS, id);
            if (s >= 0 && cmd != LINUX_SEM_STAT_ANY) {
                r = linux_ipc_may(&g_sem_obj[s], 0444u, &c);
            }
            if (s >= 0 && r == 0) {
                sem_stat_of(s, &ds);
                r = cmd == LINUX_IPC_STAT ? 0 : (long)g_sem_obj[s].id;
            }
            linux_ipc_unlock();
            if (s < 0) {
                return s;
            }
            return (r >= 0 && linux_ipc_out(arg, &ds, sizeof(ds)) != 0) ? -VIBEOS_EFAULT : r;
        case LINUX_IPC_SET:
            if (linux_ipc_in(&ds, arg, sizeof(ds)) != 0) {
                return -VIBEOS_EFAULT;
            }
            linux_ipc_lock();
            s = linux_ipc_slot(g_sem_obj, LINUX_IPC_SLOTS, id);
            if (s >= 0 && (r = linux_ipc_owner(&g_sem_obj[s], &c)) == 0) {
                linux_ipc_set(&g_sem_obj[s], &ds.sem_perm);
            }
            linux_ipc_unlock();
            return s < 0 ? s : r;
        case LINUX_IPC_RMID:
            linux_ipc_lock();
            s = linux_ipc_slot(g_sem_obj, LINUX_IPC_SLOTS, id);
            if (s >= 0 && (r = linux_ipc_owner(&g_sem_obj[s], &c)) == 0) {
                undo_clear(g_sem_obj[s].id, -1);
                g_sem_obj[s].used = 0;
            }
            linux_ipc_unlock();
            if (s >= 0 && r == 0) {
                ks_wake_waiters();   /* every waiter on it: EIDRM */
            }
            return s < 0 ? s : r;
        case LINUX_GETVAL:
        case LINUX_GETPID:
        case LINUX_GETNCNT:
        case LINUX_GETZCNT:
            linux_ipc_lock();
            s = linux_ipc_slot(g_sem_obj, LINUX_IPC_SLOTS, id);
            if (s >= 0) {
                r = linux_ipc_may(&g_sem_obj[s], 0444u, &c);
            }
            if (s >= 0 && r == 0 && (num < 0 || (uint32_t)num >= g_sem[s].nsems)) {
                r = -VIBEOS_EINVAL;
            }
            if (s >= 0 && r == 0) {
                const linux_sem_t *x = &g_sem[s].sem[num];

                r = cmd == LINUX_GETVAL ? x->val : cmd == LINUX_GETPID ? (long)x->pid
                    : cmd == LINUX_GETNCNT ? x->ncnt : x->zcnt;
            }
            linux_ipc_unlock();
            return s < 0 ? s : r;
        case LINUX_SETVAL: {
            const int32_t v = VIBEOS_ARG_INT(arg);   /* the union's int */

            if (v < 0 || v > LINUX_SEMVMX) {
                return -VIBEOS_ERANGE;
            }
            linux_ipc_lock();
            s = linux_ipc_slot(g_sem_obj, LINUX_IPC_SLOTS, id);
            if (s >= 0 && (num < 0 || (uint32_t)num >= g_sem[s].nsems)) {
                r = -VIBEOS_EINVAL;
            }
            if (s >= 0 && r == 0) {
                r = linux_ipc_may(&g_sem_obj[s], 0222u, &c);
            }
            if (s >= 0 && r == 0) {
                g_sem[s].sem[num].val = (uint16_t)v;
                g_sem[s].sem[num].pid = ks_id(ks_current())->tgid;
                undo_clear(g_sem_obj[s].id, num);
                g_sem_obj[s].ctime = linux_ipc_now();
            }
            linux_ipc_unlock();
            if (s >= 0 && r == 0) {
                ks_wake_waiters();
            }
            return s < 0 ? s : r;
        }
        case LINUX_GETALL:
        case LINUX_SETALL: {
            uint32_t n = 0, i;

            linux_ipc_lock();
            s = linux_ipc_slot(g_sem_obj, LINUX_IPC_SLOTS, id);
            if (s >= 0) {
                r = linux_ipc_may(&g_sem_obj[s], cmd == LINUX_GETALL ? 0444u : 0222u, &c);
                n = g_sem[s].nsems;
            }
            linux_ipc_unlock();
            if (s < 0 || r != 0) {
                return s < 0 ? s : r;
            }
            vals = (uint16_t *)ks_page_alloc();   /* SEMMSL of them fit in one */
            if (!vals) {
                return -VIBEOS_ENOMEM;
            }
            if (cmd == LINUX_SETALL) {
                if (linux_ipc_in(vals, arg, n * 2u) != 0) {
                    ks_page_free(vals, "semctl values");
                    return -VIBEOS_EFAULT;
                }
                for (i = 0; i < n; i++) {
                    if (vals[i] > LINUX_SEMVMX) {
                        ks_page_free(vals, "semctl values");
                        return -VIBEOS_ERANGE;
                    }
                }
            }
            linux_ipc_lock();
            /* Looked for again: the set may have gone, or been replaced by
             * one of another size, while the values were being copied. */
            if (linux_ipc_slot(g_sem_obj, LINUX_IPC_SLOTS, id) != s || g_sem[s].nsems != n) {
                r = -VIBEOS_EIDRM;
            } else if (cmd == LINUX_GETALL) {
                for (i = 0; i < n; i++) {
                    vals[i] = g_sem[s].sem[i].val;
                }
            } else {
                for (i = 0; i < n; i++) {
                    g_sem[s].sem[i].val = vals[i];
                    g_sem[s].sem[i].pid = ks_id(ks_current())->tgid;
                }
                undo_clear(g_sem_obj[s].id, -1);
                g_sem_obj[s].ctime = linux_ipc_now();
            }
            linux_ipc_unlock();
            if (r == 0 && cmd == LINUX_GETALL && linux_ipc_out(arg, vals, n * 2u) != 0) {
                r = -VIBEOS_EFAULT;
            }
            ks_page_free(vals, "semctl values");
            if (r == 0 && cmd == LINUX_SETALL) {
                ks_wake_waiters();
            }
            return r;
        }
        default:
            return -VIBEOS_EINVAL;
    }
}

/* ---- what /proc shows -------------------------------------------------------- */

void linux_ipc_proc_sem(linux_ipc_put_t put, void *o) {
    uint32_t s;

    linux_ipc_puts(put, o, "       key      semid perms      nsems   uid   gid  cuid  cgid      otime      ctime\n");
    linux_ipc_lock();
    for (s = 0; s < LINUX_IPC_SLOTS; s++) {
        const linux_ipc_obj_t *ob = &g_sem_obj[s];

        if (!ob->used) {
            continue;
        }
        linux_ipc_putn(put, o, ob->key, 10u, 10u);
        linux_ipc_putn(put, o, ob->id, 11u, 10u);
        linux_ipc_puts(put, o, "  ");
        linux_ipc_putn(put, o, ob->mode & 0777u, 4u, 8u);
        linux_ipc_putn(put, o, g_sem[s].nsems, 11u, 10u);
        linux_ipc_putn(put, o, ob->uid, 6u, 10u);
        linux_ipc_putn(put, o, ob->gid, 6u, 10u);
        linux_ipc_putn(put, o, ob->cuid, 6u, 10u);
        linux_ipc_putn(put, o, ob->cgid, 6u, 10u);
        linux_ipc_putn(put, o, g_sem[s].otime, 11u, 10u);
        linux_ipc_putn(put, o, ob->ctime, 11u, 10u);
        put(o, "\n", 1u);
    }
    linux_ipc_unlock();
}

/* /proc/sys/kernel/sem: SEMMSL, SEMMNS, SEMOPM and SEMMNI, tab-separated.
 * Read-only here: the sizes are what this kernel stores. */
void linux_ipc_proc_semlimits(linux_ipc_put_t put, void *o) {
    linux_ipc_putn(put, o, LINUX_SEMMSL, 0u, 10u);
    put(o, "\t", 1u);
    linux_ipc_putn(put, o, LINUX_SEMMNS, 0u, 10u);
    put(o, "\t", 1u);
    linux_ipc_putn(put, o, LINUX_SEMOPM, 0u, 10u);
    put(o, "\t", 1u);
    linux_ipc_putn(put, o, g_sem_ns.max < LINUX_IPC_SLOTS ? g_sem_ns.max : LINUX_IPC_SLOTS, 0u, 10u);
    put(o, "\n", 1u);
}

void linux_ipc_sem_reset(void) {
    uint32_t i;

    for (i = 0; i < LINUX_IPC_SLOTS; i++) {
        g_sem_obj[i].used = 0;
    }
    for (i = 0; i < LINUX_SEM_UNDOS; i++) {
        g_undo[i].tgid = 0;
    }
    g_sem_ns.seq = 0;
    g_sem_ns.next_id = -1;
    g_sem_ns.max = LINUX_IPC_SLOTS;
    g_sem_ns.last_idx = -1;
}

/* ---- the syscalls this file implements --------------------------------------- */
#define LINUX_SEM_SYSCALLS(X) \
    X(64,  semget,     IPC_GET,  NOPTR, linux_sys_semget(ARG(0), ARG(1), ARG(2))) \
    X(65,  semop,      IPC_SEND, NOPTR, linux_sys_semop(ARG(0), ARG(1), ARG(2))) \
    X(66,  semctl,     IPC_CTL,  NOPTR, linux_sys_semctl(ARG(0), ARG(1), ARG(2), ARG(3))) \
    X(220, semtimedop, IPC_SEND, NOPTR, linux_sys_semtimedop(ARG(0), ARG(1), ARG(2), ARG(3)))

LINUX_DEFINE_SYSCALLS(sem, LINUX_SEM_SYSCALLS)

linux_ipc_ns_t *linux_ipc_sem_ns(void) {
    return &g_sem_ns;
}
