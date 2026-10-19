/* System V shared memory (docs/abi/ L7 step 3), over what ipc.c shares.
 *
 * A segment is a file on /tmp that nothing names - memfd_create's kind, made
 * the same way - and an attachment is a shared mapping of it, so a store
 * through one attachment is the file's page and every other attachment sees
 * it. The segment table holds one reference to the file; each mapping holds
 * its pages, as any shared mapping of a file does (L3).
 *
 * How many attachments a segment has is not counted here. It is read off the
 * processes' regions when it is asked for (linux_shm_attached): fork copies a
 * region, exit and exec drop them, munmap can take one away - and a count kept
 * beside them would be one more field that every one of those paths has to
 * remember. The regions are what attaches.
 *
 * IPC_RMID on a segment still attached marks it SHM_DEST and makes it
 * private - its key finds nothing - and it goes when the last attachment does.
 * Nothing announces that moment, so it is noticed: every shm call, and every
 * read of /proc/sysvipc/shm, first lets go of the marked segments nobody has
 * attached (shm_reap). Until then its pages stay the file's. */

#include "linux_internal.h"
#include "vibeos/procfs.h"

typedef struct {
    vibeos_file_t *file;
    uint64_t size;
    int64_t atime, dtime;
    uint32_t cpid, lpid;
} linux_shmseg_t;

static linux_ipc_obj_t g_shm_obj[LINUX_IPC_SLOTS];
static linux_shmseg_t g_shm[LINUX_IPC_SLOTS];
static linux_ipc_ns_t g_shm_ns = {0, -1, LINUX_IPC_SLOTS, -1};
/* shmmax and shmall: Linux's defaults, ULONG_MAX less 2^24; a program may
 * lower them (LTP's shmget02 sets shmmax to 8192). shmall is in pages. */
static uint64_t g_shmmax = 0xfffffffffeffffffull;
static uint64_t g_shmall = 0xfffffffffeffffffull;

static uint32_t shm_tag(int s) {
    return g_shm_obj[s].id + 1u;
}

static uint64_t shm_pages_in_use(void) {
    uint64_t pages = 0;
    uint32_t s;

    for (s = 0; s < LINUX_IPC_SLOTS; s++) {
        if (g_shm_obj[s].used) {
            pages += (g_shm[s].size + 4095u) / 4096u;
        }
    }
    return pages;
}

/* Let go of every segment that was removed and that nobody has attached any
 * more. The count is read without the lock (it takes the processes' own),
 * and the segment's number is checked again under it: a segment the count
 * was about has not been replaced meanwhile only if its number is the same. */
static void shm_reap(void) {
    uint32_t s;

    for (s = 0; s < LINUX_IPC_SLOTS; s++) {
        vibeos_file_t *drop = 0;
        uint32_t id;
        int marked;

        linux_ipc_lock();
        marked = g_shm_obj[s].used && (g_shm_obj[s].mode & LINUX_SHM_DEST);
        id = g_shm_obj[s].id;
        linux_ipc_unlock();
        if (!marked || linux_shm_attached(id + 1u) != 0u) {
            continue;
        }
        linux_ipc_lock();
        if (g_shm_obj[s].used && g_shm_obj[s].id == id && (g_shm_obj[s].mode & LINUX_SHM_DEST)) {
            drop = g_shm[s].file;
            g_shm[s].file = 0;
            g_shm_obj[s].used = 0;
        }
        linux_ipc_unlock();
        if (drop) {
            vibeos_file_put(drop);   /* the last reference: the file goes, and its pages */
        }
    }
}

/* shmget's answer for a key that names a segment already: EEXIST under
 * IPC_EXCL, EINVAL for more than it has, the permission question, then its
 * id. One function for both places that find one - the first look, and the
 * second after the file was made outside the lock, which answered with the id
 * and asked nothing (M-113). Under the IPC lock. */
static long shm_existing(int s, uint64_t size, uint64_t flg, const vibeos_cred_t *c) {
    long r;

    if ((flg & LINUX_IPC_CREAT) && (flg & LINUX_IPC_EXCL)) {
        return -VIBEOS_EEXIST;
    }
    if (size > g_shm[s].size) {
        return -VIBEOS_EINVAL;   /* asks for more than the segment has */
    }
    r = linux_ipc_may(&g_shm_obj[s], (uint32_t)flg, c);
    return r == 0 ? (long)g_shm_obj[s].id : r;
}

static long linux_sys_shmget(uint64_t key, uint64_t size, uint64_t flg) {
    vibeos_cred_t c;
    vibeos_file_t *f = 0;
    long r, err = 0;
    int s;

    linux_cred(&c);
    shm_reap();
    if (flg & LINUX_SHM_HUGETLB) {
        /* There are no huge pages to give. Linux refuses a caller outside
         * the hugetlb group with EPERM and lacks the pages for the rest. */
        linux_ipc_lock();
        s = linux_ipc_lookup(g_shm_obj, LINUX_IPC_SLOTS, (int32_t)(uint32_t)key);
        linux_ipc_unlock();
        if (s < 0 && (flg & LINUX_IPC_CREAT)) {
            return c.euid == 0u ? -VIBEOS_ENOMEM : -VIBEOS_EPERM;
        }
    }
    linux_ipc_lock();
    s = linux_ipc_lookup(g_shm_obj, LINUX_IPC_SLOTS, (int32_t)(uint32_t)key);
    if (s >= 0) {
        r = shm_existing(s, size, flg, &c);
        linux_ipc_unlock();
        return r;
    }
    if (!(flg & LINUX_IPC_CREAT) && (int32_t)(uint32_t)key != LINUX_IPC_PRIVATE) {
        linux_ipc_unlock();
        return -VIBEOS_ENOENT;
    }
    if (size < 1u || size > g_shmmax) {
        linux_ipc_unlock();
        return -VIBEOS_EINVAL;
    }
    if (shm_pages_in_use() + (size + 4095u) / 4096u > g_shmall) {
        linux_ipc_unlock();
        return -VIBEOS_ENOSPC;
    }
    linux_ipc_unlock();
    /* The file first, outside the lock: making it walks a path. */
    f = linux_hidden_file("/tmp/.shm-", &err);
    if (!f) {
        return err;
    }
    if (f->ops->truncate && (err = f->ops->truncate(f, size)) != 0) {
        vibeos_file_put(f);
        return err;
    }
    linux_ipc_lock();
    /* Looked for again: another process may have made the key meanwhile. */
    s = linux_ipc_lookup(g_shm_obj, LINUX_IPC_SLOTS, (int32_t)(uint32_t)key);
    if (s >= 0) {
        r = shm_existing(s, size, flg, &c);
    } else {
        r = linux_ipc_new(&g_shm_ns, g_shm_obj, LINUX_IPC_SLOTS, (int32_t)(uint32_t)key, flg, &c);
        if (r >= 0) {
            linux_shmseg_t *g = &g_shm[r];

            g->file = f;
            f = 0;
            g->size = size;
            g->atime = g->dtime = 0;
            g->cpid = ks_id(ks_current())->tgid;
            g->lpid = 0;
            r = (long)g_shm_obj[r].id;
        }
    }
    linux_ipc_unlock();
    if (f) {
        vibeos_file_put(f);
    }
    return r;
}

static long linux_sys_shmat(uint64_t id, uint64_t addr, uint64_t flg) {
    const int ro = (flg & LINUX_SHM_RDONLY) != 0u, exec = (flg & LINUX_SHM_EXEC) != 0u;
    vibeos_cred_t c;
    vibeos_file_t *f = 0;
    uint64_t size = 0;
    uint32_t tag = 0;
    long r = 0;
    int s;

    if (addr != 0u) {
        if (addr & (LINUX_SHM_ALIGN - 1u)) {
            if (!(flg & LINUX_SHM_RND)) {
                return -VIBEOS_EINVAL;
            }
            addr &= ~(uint64_t)(LINUX_SHM_ALIGN - 1u);
            if (addr == 0u && (flg & LINUX_SHM_REMAP)) {
                return -VIBEOS_EINVAL;
            }
        }
    } else if (flg & LINUX_SHM_REMAP) {
        return -VIBEOS_EINVAL;   /* replace what, where? */
    }
    linux_cred(&c);
    shm_reap();
    linux_ipc_lock();
    s = linux_ipc_slot(g_shm_obj, LINUX_IPC_SLOTS, id);
    if (s < 0) {
        r = s;
    } else {
        r = linux_ipc_may(&g_shm_obj[s], (ro ? 0444u : 0666u) | (exec ? 0111u : 0u), &c);
    }
    if (r == 0) {
        f = g_shm[s].file;
        vibeos_file_get(f);   /* held for the mapping, which a removal cannot pull away */
        size = g_shm[s].size;
        tag = shm_tag(s);
    }
    linux_ipc_unlock();
    if (r != 0) {
        return r;
    }
    r = linux_shm_map(f, addr, size, ro, exec, (flg & LINUX_SHM_REMAP) != 0u, tag);
    vibeos_file_put(f);
    if (r > 0) {
        linux_ipc_lock();
        if (linux_ipc_slot(g_shm_obj, LINUX_IPC_SLOTS, id) == s) {
            g_shm[s].atime = linux_ipc_now();
            g_shm[s].lpid = ks_id(ks_current())->tgid;
        }
        linux_ipc_unlock();
    }
    return r;
}

static long linux_sys_shmdt(uint64_t addr) {
    long r;
    uint32_t s;

    if (addr & 0xFFFull) {
        return -VIBEOS_EINVAL;
    }
    r = linux_shm_unmap(addr);
    if (r < 0) {
        return r;
    }
    linux_ipc_lock();
    for (s = 0; s < LINUX_IPC_SLOTS; s++) {
        if (g_shm_obj[s].used && shm_tag((int)s) == (uint32_t)r) {
            g_shm[s].dtime = linux_ipc_now();
            g_shm[s].lpid = ks_id(ks_current())->tgid;
        }
    }
    linux_ipc_unlock();
    shm_reap();   /* that may have been the last attachment of a removed one */
    return 0;
}

static void shm_stat_of(int s, uint32_t nattch, linux_shmid64_ds_t *out) {
    uint8_t *raw = (uint8_t *)out;
    uint32_t i;

    for (i = 0; i < (uint32_t)sizeof(*out); i++) {
        raw[i] = 0;
    }
    linux_ipc_perm_out(&g_shm_obj[s], &out->shm_perm);
    out->shm_segsz = g_shm[s].size;
    out->shm_atime = g_shm[s].atime;
    out->shm_dtime = g_shm[s].dtime;
    out->shm_ctime = g_shm_obj[s].ctime;
    out->shm_cpid = (int32_t)g_shm[s].cpid;
    out->shm_lpid = (int32_t)g_shm[s].lpid;
    out->shm_nattch = nattch;
}

/* The slot a number (or, for the *_STAT forms, an index) names, and how many
 * attach it - counted before the lock is taken, and good only if the slot
 * still holds that segment once it is. Returns the slot, under the lock. */
static int shm_find_counted(uint64_t id, int by_index, uint32_t *nattch) {
    uint32_t tag;
    int s, tries;

    for (tries = 0; tries < 4; tries++) {
        linux_ipc_lock();
        s = by_index ? linux_ipc_index(g_shm_obj, LINUX_IPC_SLOTS, id)
                     : linux_ipc_slot(g_shm_obj, LINUX_IPC_SLOTS, id);
        tag = s >= 0 ? shm_tag(s) : 0u;
        linux_ipc_unlock();
        if (s < 0) {
            linux_ipc_lock();
            return s;
        }
        *nattch = linux_shm_attached(tag);
        linux_ipc_lock();
        if (g_shm_obj[s].used && shm_tag(s) == tag) {
            return s;
        }
        linux_ipc_unlock();
    }
    linux_ipc_lock();
    return -VIBEOS_EINVAL;
}

static long linux_sys_shmctl(uint64_t id, uint64_t cmd_arg, uint64_t buf) {
    const int32_t cmd = VIBEOS_ARG_INT(cmd_arg);
    vibeos_cred_t c;
    linux_shmid64_ds_t ds;
    uint32_t nattch = 0;
    long r = 0;
    int s;

    linux_cred(&c);
    shm_reap();
    switch (cmd) {
        case LINUX_IPC_INFO: {
            linux_shminfo64_t si;
            uint8_t *raw = (uint8_t *)&si;
            uint32_t i;

            for (i = 0; i < (uint32_t)sizeof(si); i++) {
                raw[i] = 0;
            }
            linux_ipc_lock();
            si.shmmax = g_shmmax;
            si.shmmin = 1u;
            si.shmmni = g_shm_ns.max < LINUX_IPC_SLOTS ? g_shm_ns.max : LINUX_IPC_SLOTS;
            si.shmseg = si.shmmni;
            si.shmall = g_shmall;
            r = linux_ipc_max_index(g_shm_obj, LINUX_IPC_SLOTS);
            linux_ipc_unlock();
            return linux_ipc_out(buf, &si, sizeof(si)) != 0 ? -VIBEOS_EFAULT : r;
        }
        case LINUX_SHM_INFO: {
            linux_shm_info_t si;
            uint8_t *raw = (uint8_t *)&si;
            uint32_t i;

            for (i = 0; i < (uint32_t)sizeof(si); i++) {
                raw[i] = 0;
            }
            linux_ipc_lock();
            si.used_ids = (int32_t)linux_ipc_live(g_shm_obj, LINUX_IPC_SLOTS);
            si.shm_tot = shm_pages_in_use();
            /* Resident and swapped are the file's to say, and tmpfs does not
             * count them apart; 0, as /proc/sysvipc/shm says too. */
            r = linux_ipc_max_index(g_shm_obj, LINUX_IPC_SLOTS);
            linux_ipc_unlock();
            return linux_ipc_out(buf, &si, sizeof(si)) != 0 ? -VIBEOS_EFAULT : r;
        }
        case LINUX_IPC_STAT:
        case LINUX_SHM_STAT:
        case LINUX_SHM_STAT_ANY:
            s = shm_find_counted(id, cmd != LINUX_IPC_STAT, &nattch);
            if (s >= 0 && cmd != LINUX_SHM_STAT_ANY) {
                r = linux_ipc_may(&g_shm_obj[s], 0444u, &c);
            }
            if (s >= 0 && r == 0) {
                shm_stat_of(s, nattch, &ds);
                r = cmd == LINUX_IPC_STAT ? 0 : (long)g_shm_obj[s].id;
            }
            linux_ipc_unlock();
            if (s < 0) {
                return s;
            }
            return (r >= 0 && linux_ipc_out(buf, &ds, sizeof(ds)) != 0) ? -VIBEOS_EFAULT : r;
        case LINUX_IPC_SET:
            if (linux_ipc_in(&ds, buf, sizeof(ds)) != 0) {
                return -VIBEOS_EFAULT;
            }
            linux_ipc_lock();
            s = linux_ipc_slot(g_shm_obj, LINUX_IPC_SLOTS, id);
            if (s >= 0 && (r = linux_ipc_owner(&g_shm_obj[s], &c)) == 0) {
                linux_ipc_set(&g_shm_obj[s], &ds.shm_perm);
            }
            linux_ipc_unlock();
            return s < 0 ? s : r;
        case LINUX_IPC_RMID:
            linux_ipc_lock();
            s = linux_ipc_slot(g_shm_obj, LINUX_IPC_SLOTS, id);
            if (s >= 0 && (r = linux_ipc_owner(&g_shm_obj[s], &c)) == 0) {
                g_shm_obj[s].mode |= LINUX_SHM_DEST;
                g_shm_obj[s].key = LINUX_IPC_PRIVATE;   /* nobody finds it by its key again */
            }
            linux_ipc_unlock();
            if (s >= 0 && r == 0) {
                shm_reap();   /* at once, when nobody has it attached */
            }
            return s < 0 ? s : r;
        case LINUX_SHM_LOCK:
        case LINUX_SHM_UNLOCK:
            /* The pages are a file's, and a file's pages are never paged
             * out here; what is left is the mark and who may set it - the
             * owner or the superuser, as Linux allows without CAP_IPC_LOCK. */
            linux_ipc_lock();
            s = linux_ipc_slot(g_shm_obj, LINUX_IPC_SLOTS, id);
            if (s >= 0 && (r = linux_ipc_owner(&g_shm_obj[s], &c)) == 0) {
                if (cmd == LINUX_SHM_LOCK) {
                    g_shm_obj[s].mode |= LINUX_SHM_LOCKED;
                } else {
                    g_shm_obj[s].mode &= ~(uint32_t)LINUX_SHM_LOCKED;
                }
            }
            linux_ipc_unlock();
            return s < 0 ? s : r;
        default:
            return -VIBEOS_EINVAL;
    }
}

/* ---- what /proc shows -------------------------------------------------------- */

void linux_ipc_proc_shm(linux_ipc_put_t put, void *o) {
    uint32_t s;

    shm_reap();
    linux_ipc_puts(put, o, "       key      shmid perms                  size  cpid  lpid nattch   uid   gid"
                           "  cuid  cgid      atime      dtime      ctime                   rss"
                           "                  swap\n");
    for (s = 0; s < LINUX_IPC_SLOTS; s++) {
        linux_ipc_obj_t ob;
        linux_shmseg_t g;
        uint32_t nattch;

        linux_ipc_lock();
        ob = g_shm_obj[s];
        g = g_shm[s];
        linux_ipc_unlock();
        if (!ob.used) {
            continue;
        }
        nattch = linux_shm_attached(ob.id + 1u);
        linux_ipc_putn(put, o, ob.key, 10u, 10u);
        linux_ipc_putn(put, o, ob.id, 11u, 10u);
        linux_ipc_puts(put, o, "  ");
        linux_ipc_putn(put, o, ob.mode, 4u, 8u);
        linux_ipc_putn(put, o, (int64_t)g.size, 22u, 10u);
        linux_ipc_putn(put, o, g.cpid, 6u, 10u);
        linux_ipc_putn(put, o, g.lpid, 6u, 10u);
        linux_ipc_puts(put, o, " ");
        linux_ipc_putn(put, o, nattch, 6u, 10u);
        linux_ipc_putn(put, o, ob.uid, 6u, 10u);
        linux_ipc_putn(put, o, ob.gid, 6u, 10u);
        linux_ipc_putn(put, o, ob.cuid, 6u, 10u);
        linux_ipc_putn(put, o, ob.cgid, 6u, 10u);
        linux_ipc_putn(put, o, g.atime, 11u, 10u);
        linux_ipc_putn(put, o, g.dtime, 11u, 10u);
        linux_ipc_putn(put, o, ob.ctime, 11u, 10u);
        linux_ipc_putn(put, o, 0, 22u, 10u);
        linux_ipc_putn(put, o, 0, 22u, 10u);
        put(o, "\n", 1u);
    }
}

/* The tunables a program writes: the largest segment, the pages there may be
 * in all, and how many segments. */
static int shm_set_u64(uint64_t *where, const char *buf, uint32_t len) {
    uint64_t v = 0;
    uint32_t i = 0;
    int digits = 0;

    while (i < len && (buf[i] == ' ' || buf[i] == '\t')) {
        i++;
    }
    while (i < len && buf[i] >= '0' && buf[i] <= '9') {
        uint64_t d = (uint64_t)(buf[i++] - '0');

        if (v > (~0ull - d) / 10u) {
            return -VIBEOS_EINVAL;
        }
        v = v * 10u + d;
        digits = 1;
    }
    while (i < len && (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\n')) {
        i++;
    }
    if (!digits || i != len) {
        return -VIBEOS_EINVAL;
    }
    linux_ipc_lock();
    *where = v;
    linux_ipc_unlock();
    return 0;
}

void linux_ipc_shmmax_text(linux_ipc_put_t put, void *o) {
    linux_ipc_putn(put, o, (int64_t)g_shmmax, 0u, 10u);   /* below 2^63: shown as it is */
    put(o, "\n", 1u);
}

int linux_ipc_shmmax_write(const char *buf, uint32_t len) {
    return shm_set_u64(&g_shmmax, buf, len);
}

void linux_ipc_shmall_text(linux_ipc_put_t put, void *o) {
    linux_ipc_putn(put, o, (int64_t)g_shmall, 0u, 10u);
    put(o, "\n", 1u);
}

int linux_ipc_shmall_write(const char *buf, uint32_t len) {
    return shm_set_u64(&g_shmall, buf, len);
}

linux_ipc_ns_t *linux_ipc_shm_ns(void) {
    return &g_shm_ns;
}

/* Forgotten, not released: this runs at boot, before there is anything, and
 * for the host tests' fresh kernel, whose file table has been emptied under
 * it - a put there would be a put of something that no longer exists. */
void linux_ipc_shm_reset(void) {
    uint32_t i;

    for (i = 0; i < LINUX_IPC_SLOTS; i++) {
        g_shm_obj[i].used = 0;
        g_shm[i].file = 0;
    }
    g_shm_ns.seq = 0;
    g_shm_ns.next_id = -1;
    g_shm_ns.max = LINUX_IPC_SLOTS;
    g_shm_ns.last_idx = -1;
    g_shmmax = g_shmall = 0xfffffffffeffffffull;
}

/* ---- the syscalls this file implements --------------------------------------- */
#define LINUX_SHM_SYSCALLS(X) \
    X(29,  shmget, IPC_GET,  NOPTR, linux_sys_shmget(ARG(0), ARG(1), ARG(2))) \
    X(30,  shmat,  SHM_MAP,  NOPTR, linux_sys_shmat(ARG(0), ARG(1), ARG(2))) \
    X(31,  shmctl, IPC_CTL,  NOPTR, linux_sys_shmctl(ARG(0), ARG(1), ARG(2))) \
    X(67,  shmdt,  SHM_MAP,  NOPTR, linux_sys_shmdt(ARG(0)))

LINUX_DEFINE_SYSCALLS(shm, LINUX_SHM_SYSCALLS)
