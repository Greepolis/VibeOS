/* NT personality: the process services (docs/abi/windows.md W1, W6).
 *
 * NtTerminateProcess and NtQueryInformationProcess, for the calling process and
 * since W6 for a process handle; NtCreateUserProcess and NtOpenProcess.
 *
 * A new process is a fork of its creator whose child execs the image, because
 * a fork and an exec are what the kernel underneath knows how to make. The
 * child does not return from the fork into the creator's code: it starts in
 * ntdll's entry with rcx 3 and the block its parent left it in rdx - the path,
 * the arguments, the environment - and ntdll calls NtCreateUserProcess with
 * a pointer to that address as the first argument, which this side
 * recognises and execs. The parent's copy of the block is freed before it
 * returns; the child has its own. */

#include "nt_internal.h"

#define NT_STILL_ACTIVE 0x00000103u
#define NT_PROCESS_TERMINATE 0x0001u
#define NT_PROCESS_QUERY_INFORMATION 0x0400u
#define NT_PROCESS_QUERY_LIMITED 0x1000u
#define NT_PROCESS_ALL_ACCESS 0x001FFFFFu
#define NT_SPAWN_MAGIC 0x4E57505356424956ull   /* "VIBVSPWN" */
#define NT_SPAWN_BYTES (64u * 1024u)
#define NT_SPAWN_ARGS 64u
#define NT_SPAWN_ENVS 128u
#define NT_SPAWN_ENTRY_MARK 3u   /* rcx at ntdll's entry: a new process's child */
#define NT_SPAWN_HDR 9u          /* magic, ppid, path, argv, envp, flags, three handles */
#define NT_SPAWN_VEC (8u * NT_SPAWN_HDR)
#define NT_SPAWN_INHERIT 1u
#define NT_PROCESS_CREATE_FLAGS_INHERIT_HANDLES 0x0004u
#define NT_THREAD_CREATE_SUSPENDED 0x0001u

static int nt_is_self(uint64_t h) {
    return h == VIBEOS_NT_CURRENT_PROCESS;
}

/* ---- the process type ------------------------------------------------------------------- */

/* Has it ended? A slot that is not the tenancy the object names has; a zombie
 * has, and leaves its code, the whole 32 bits a Windows parent reads. */
static int process_ended(nt_ob_t *o) {
    vibeos_task_state_t s;

    if (o->u.pr.ended) {
        return 1;
    }
    s = vibeos_task_state((uint32_t)o->u.pr.slot);
    if (ks_seq(o->u.pr.slot) != o->u.pr.seq || s == VIBEOS_TASK_FREE) {
        o->u.pr.exit_code = 0;   /* gone before anyone looked: reaped elsewhere */
        o->u.pr.ended = 1;
    } else if (s == VIBEOS_TASK_ZOMBIE) {
        const vibeos_task_t *t = ks_id(o->u.pr.slot);

        o->u.pr.exit_code = t->exit_signal ? 1u : (uint32_t)t->exit_code;
        o->u.pr.ended = 1;
    }
    return (int)o->u.pr.ended;
}

static int process_signaled(nt_ob_t *o, uint32_t tid) {
    (void)tid;
    return process_ended(o);
}

static uint32_t process_acquire(nt_ob_t *o, uint32_t tid) {
    (void)o;
    (void)tid;
    return 0;
}

static const nt_type_t g_process_type = {
    "Process",
    NT_PROCESS_ALL_ACCESS,
    0x00020410u,   /* GENERIC_READ: READ_CONTROL, VM_READ, QUERY_INFORMATION */
    0x00020BEAu,   /* GENERIC_WRITE                                            */
    0x00121000u,   /* GENERIC_EXECUTE: SYNCHRONIZE, QUERY_LIMITED_INFORMATION */
    process_signaled,
    process_acquire,
};

/* The last handle: an ended child is reaped, since no Windows parent waits
 * for one the way a Unix parent must - left, it would hold its slot until the
 * parent itself ended. */
static void process_release(vibeos_file_t *f) {
    nt_ob_t *o = nt_ob_of(f);
    uint32_t pid = 0;

    nt_ob_lock();
    if (process_ended(o) && ks_seq(o->u.pr.slot) == o->u.pr.seq) {
        pid = o->u.pr.pid;
    }
    nt_ob_unlock();
    nt_ob_release(f);
    if (pid) {
        vibeos_linux_reap(pid);
    }
}

static const vibeos_file_ops_t g_nt_process_ops = {
    .name = "nt-process",
    .release = process_release,
    .nt_type = &g_process_type,
};

static void process_init(nt_ob_t *o, const void *ctx) {
    int slot = *(const int *)ctx;

    o->u.pr.slot = slot;
    o->u.pr.seq = ks_seq(slot);
    o->u.pr.pid = ks_id(slot)->tgid;
    o->u.pr.exit_code = NT_STILL_ACTIVE;
    o->u.pr.ended = 0;
}

typedef struct {
    int self;
    int slot;
    uint32_t seq, pid, ppid, exit_code, ended;
    vibeos_file_t *f;
} process_view_t;

static uint32_t process_view(uint64_t h, uint32_t need, process_view_t *v) {
    int me = ks_current();
    nt_ob_t *o;
    uint32_t st;

    v->f = 0;
    v->self = nt_is_self(h);
    if (v->self) {
        v->slot = me;
        v->seq = ks_seq(me);
        v->pid = ks_id(me)->tgid;
        v->ppid = ks_id(me)->ppid;
        v->exit_code = NT_STILL_ACTIVE;
        v->ended = 0;
        return VIBEOS_STATUS_SUCCESS;
    }
    st = nt_handle_ref(h, &g_nt_process_ops, need, &v->f, 0);
    if (st != VIBEOS_STATUS_SUCCESS) {
        return st;
    }
    nt_ob_lock();
    o = nt_ob_of(v->f);
    v->ended = (uint32_t)process_ended(o);
    v->slot = o->u.pr.slot;
    v->seq = o->u.pr.seq;
    v->pid = o->u.pr.pid;
    v->exit_code = o->u.pr.exit_code;
    nt_ob_unlock();
    v->ppid = v->ended ? 0u : ks_id(v->slot)->ppid;
    return VIBEOS_STATUS_SUCCESS;
}

static void process_unview(process_view_t *v) {
    if (v->f) {
        vibeos_file_put(v->f);
    }
}

uint32_t nt_process_ps_get(uint64_t h, uint32_t need, vibeos_procstate_t **out) {
    process_view_t v;
    vibeos_procstate_t *ps = 0;
    uint32_t st = process_view(h, need, &v);

    *out = 0;
    if (st != VIBEOS_STATUS_SUCCESS) {
        return st;
    }
    /* Under the scheduler's lock the slot cannot change tenant, and a state
     * with references left cannot be freed: one more, unless it has none. */
    ks_irq_off();
    ks_lock(ks_sched_lock(), __func__);
    if (!v.ended && ks_seq(v.slot) == v.seq) {
        ps = ks_ps(v.slot);
        if (ps) {
            uint32_t r = __atomic_load_n(&ps->refs, __ATOMIC_ACQUIRE);

            while (r != 0u && !__atomic_compare_exchange_n(&ps->refs, &r, r + 1u, 0, __ATOMIC_ACQ_REL,
                                                           __ATOMIC_ACQUIRE)) {
            }
            if (r == 0u) {
                ps = 0;
            }
        }
    }
    ks_unlock(ks_sched_lock());
    ks_irq_on();
    process_unview(&v);
    if (!ps) {
        return VIBEOS_STATUS_PROCESS_IS_TERMINATING;
    }
    *out = ps;
    return VIBEOS_STATUS_SUCCESS;
}

/* NtTerminateProcess(ProcessHandle, ExitStatus). Oneself: the whole process
 * ends with the status. Another: killed, and its handles report the status
 * asked for, since the kill leaves none of its own. Windows also lets 0 mean
 * "every thread but this one"; that is refused rather than half-honoured. */
static long nt_terminate_process(uint64_t process, uint64_t status) {
    process_view_t v;
    uint32_t st;

    if (nt_is_self(process)) {
        ks_task_exit_group((uint64_t)(uint32_t)status);
        return NT_STATUS(VIBEOS_STATUS_SUCCESS);   /* not reached */
    }
    st = process_view(process, NT_PROCESS_TERMINATE, &v);
    if (st != VIBEOS_STATUS_SUCCESS) {
        return NT_STATUS(st);
    }
    if (!v.ended) {
        nt_ob_lock();
        if (!nt_ob_of(v.f)->u.pr.ended) {
            nt_ob_of(v.f)->u.pr.exit_code = (uint32_t)status;
            nt_ob_of(v.f)->u.pr.ended = 1;
        }
        nt_ob_unlock();
        if (ks_seq(v.slot) == v.seq) {
            (void)ks_signal_send(v.slot, VIBEOS_SIGKILL, 0);
        }
    }
    process_unview(&v);
    return NT_STATUS(VIBEOS_STATUS_SUCCESS);
}

/* NtOpenProcess(ProcessHandle, DesiredAccess, ObjectAttributes, ClientId): a
 * handle to the process the client id's first word names. */
static long nt_open_process(uint64_t out, uint32_t want, uint64_t cid) {
    uint64_t pid = 0;
    int slot;

    if (cid == 0u || nt_copy_in(&pid, cid, 8u) != 0) {
        return NT_STATUS(cid ? VIBEOS_STATUS_ACCESS_VIOLATION : VIBEOS_STATUS_INVALID_PARAMETER);
    }
    ks_irq_off();
    ks_lock(ks_sched_lock(), __func__);
    slot = pid ? ks_task_by_pid((uint32_t)pid) : -1;
    if (slot >= 0 && (ks_id(slot)->pid != ks_id(slot)->tgid || !ks_id(slot)->is_user)) {
        slot = -1;   /* a thread's id, or a kernel task's: not a process */
    }
    ks_unlock(ks_sched_lock());
    ks_irq_on();
    if (slot < 0) {
        return NT_STATUS(VIBEOS_STATUS_INVALID_CID);
    }
    return nt_create_common(&g_nt_process_ops, out, want, 0, 0, process_init, &slot);
}

/* ---- creating one ----------------------------------------------------------------------- */

typedef struct {
    uint8_t *b;      /* the block, built here and copied to the caller */
    uint32_t at;
    uint32_t cap;
} spawn_blk_t;

/* `n` UTF-16 units from user memory at `u`, appended as UTF-8 with a NUL: its
 * offset in the block, or -1. */
static long spawn_utf8(spawn_blk_t *k, uint64_t u, uint32_t n) {
    uint32_t start = k->at, i;

    for (i = 0; i < n; i++) {
        uint16_t c16;
        uint32_t c;

        if (nt_copy_in(&c16, u + 2u * i, 2u) != 0) {
            return -1;
        }
        c = c16;
        if (k->at + 4u >= k->cap) {
            return -1;
        }
        if (c < 0x80u) {
            k->b[k->at++] = (uint8_t)c;
        } else if (c < 0x800u) {
            k->b[k->at++] = (uint8_t)(0xC0u | (c >> 6));
            k->b[k->at++] = (uint8_t)(0x80u | (c & 0x3Fu));
        } else {
            k->b[k->at++] = (uint8_t)(0xE0u | (c >> 12));
            k->b[k->at++] = (uint8_t)(0x80u | ((c >> 6) & 0x3Fu));
            k->b[k->at++] = (uint8_t)(0x80u | (c & 0x3Fu));
        }
    }
    k->b[k->at++] = 0;
    return (long)start;
}

/* A UNICODE_STRING of the parameters, its buffer made absolute if they are not
 * normalised. 0, or -1. */
static int spawn_ustr(uint64_t params, uint32_t field, int normalised, uint64_t *buf, uint32_t *chars) {
    uint16_t len = 0;
    uint64_t b = 0;

    if (nt_copy_in(&len, params + field, 2u) != 0 || nt_copy_in(&b, params + field + 8u, 8u) != 0) {
        return -1;
    }
    *buf = (b && !normalised) ? params + b : b;
    *chars = len / 2u;
    return 0;
}

/* The command line split as a Windows program's C runtime splits it: blanks
 * between arguments, quotes around blanks, a backslash before a quote escaping
 * it. In place, in the block from `from`; the offsets of the arguments in
 * args[], their count returned. */
static uint32_t spawn_split(spawn_blk_t *k, uint32_t from, uint32_t *args, uint32_t max) {
    uint32_t n = 0, r = from, w;

    for (;;) {
        int quoted = 0;

        while (k->b[r] == ' ' || k->b[r] == '\t') {
            r++;
        }
        if (!k->b[r] || n == max) {
            return n;
        }
        args[n++] = w = r;
        while (k->b[r] && (quoted || (k->b[r] != ' ' && k->b[r] != '\t'))) {
            if (k->b[r] == '\\' && k->b[r + 1u] == '"') {
                k->b[w++] = '"';
                r += 2u;
            } else if (k->b[r] == '"') {
                quoted = !quoted;
                r++;
            } else {
                k->b[w++] = k->b[r++];
            }
        }
        if (k->b[r]) {
            r++;
        }
        k->b[w] = 0;
    }
}

/* The child's half: the block its parent left at *ProcessHandle, if this is a
 * child of the call - its parent's pid at the block's second word - and the
 * exec. 1 if it was (and the exec failed), 0 if this call is a parent's. */
/* The child's table before its exec: the standard handles the parent named
 * moved onto descriptors 0, 1 and 2, where the exec's PEB says they are; and,
 * unless the parent asked for its handles to be inherited, every other
 * descriptor marked to close at the exec. A handle made with OBJ_INHERIT is
 * kept only when the parent asked: Windows needs both. */
static void spawn_table(const uint64_t *hdr) {
    vibeos_procstate_t *ps = ks_ps(ks_current());
    vibeos_file_t *put[3] = { 0, 0, 0 };
    uint32_t i;
    int top;

    if (!ps) {
        return;
    }
    ks_lock(&ps->files_lock, __func__);
    for (i = 0; i < 3u; i++) {
        uint64_t h = hdr[6u + i];
        vibeos_file_t *f;

        if (h == 0u || (h & 3u) != 0u || h / 4u - 1u == i) {
            continue;   /* none, or already where it belongs */
        }
        f = vibeos_fdtable_get(&ps->files, h / 4u - 1u);
        if (f) {
            vibeos_file_get(f);
            if (vibeos_fdtable_install_at(&ps->files, i, f, 0u, &put[i]) != 0) {
                vibeos_file_put(f);
            }
        }
    }
    top = vibeos_fdtable_highest(&ps->files);
    for (i = 0; (int)i <= top; i++) {
        if (i < 3u) {
            (void)vibeos_fdtable_set_flags(&ps->files, i, 0u);   /* the standard handles cross */
        } else if (!(hdr[5] & NT_SPAWN_INHERIT) && vibeos_fdtable_get(&ps->files, i)) {
            (void)vibeos_fdtable_set_flags(&ps->files, i, VIBEOS_FD_CLOEXEC);
        }
    }
    ks_unlock(&ps->files_lock);
    for (i = 0; i < 3u; i++) {
        if (put[i]) {
            vibeos_file_put(put[i]);   /* outside the lock: a release does I/O */
        }
    }
}

static int spawn_child(ks_regs_t *frame, uint64_t handle_u, long *result) {
    uint64_t base = 0, hdr[NT_SPAWN_HDR];
    int me = ks_current();

    if (nt_copy_in(&base, handle_u, 8u) != 0 || base == 0u || (base & 0xFFFu) != 0u ||
        nt_copy_in(hdr, base, sizeof(hdr)) != 0 || hdr[0] != NT_SPAWN_MAGIC ||
        hdr[1] != (uint64_t)ks_id(me)->ppid || hdr[1] == (uint64_t)ks_id(me)->tgid) {
        return 0;
    }
    hdr[0] = 0;   /* used: a later call of this process's own is a parent's */
    (void)nt_copy_out(base, hdr, 8u);
    spawn_table(hdr);
    *result = vibeos_linux_execve(frame, hdr[2], hdr[3], hdr[4]);
    /* The parent has its handle already; a child whose image cannot run ends
     * with the status the parent would have been refused with. Run, the
     * frame is the new image's and the call returns into it. */
    if (*result < 0) {
        ks_task_exit_group(nt_status_of_errno(*result));
    }
    return 1;
}

/* NtCreateUserProcess(ProcessHandle, ThreadHandle, ProcessDesiredAccess,
 * ThreadDesiredAccess, ProcessObjectAttributes, ThreadObjectAttributes,
 * ProcessFlags, ThreadFlags, ProcessParameters, CreateInfo, AttributeList).
 * The image named by the parameters' ImagePathName, its arguments from their
 * CommandLine and its environment from theirs, its standard handles from
 * theirs; the creator's other handles cross only with
 * PROCESS_CREATE_FLAGS_INHERIT_HANDLES, and then those made inheritable. An
 * image that is not there is refused here, before anything is made. A
 * suspended first thread is stopped as NtSuspendThread stops one, before it
 * reaches the image, and NtResumeThread lets it go. */
static long nt_create_user_process(ks_regs_t *frame, uint64_t ph, uint64_t th, uint32_t paccess,
                                   uint32_t taccess, uint32_t pflags, uint32_t tflags, uint64_t params) {
    static uint8_t g_blk[NT_SPAWN_BYTES];   /* under g_spawn_lock */
    static vibeos_lock_t g_spawn_lock;
    spawn_blk_t k;
    nt_name_t name;
    char path[VIBEOS_PATH_MAX];
    uint64_t buf, base = 0, vec[NT_SPAWN_ARGS + NT_SPAWN_ENVS + 2u], hdr[NT_SPAWN_HDR], env = 0, h = 0;
    uint32_t chars, flags = 0, args[NT_SPAWN_ARGS], envs = 0, nargs, i, st;
    long cmd, r, pid;
    int normalised, slot = -1;
    const vibeos_image_t *self = ks_image(ks_current());

    if (spawn_child(frame, ph, &r)) {
        return r;   /* the exec result: the new image is entered */
    }
    if (params == 0u || !self || !self->entry) {
        return NT_STATUS(VIBEOS_STATUS_INVALID_PARAMETER);
    }
    if (nt_copy_in(&flags, params + VIBEOS_RUPP_FLAGS, 4u) != 0) {
        return NT_STATUS(VIBEOS_STATUS_ACCESS_VIOLATION);
    }
    normalised = (flags & VIBEOS_RUPP_FLAG_NORMALIZED) != 0u;
    /* The image: an NT path, or a DOS one, which RtlCreateProcessParametersEx
     * is given by most callers and passes on as it was given. */
    if (spawn_ustr(params, VIBEOS_RUPP_IMAGE_PATH, normalised, &buf, &chars) != 0 || chars == 0u) {
        return NT_STATUS(VIBEOS_STATUS_INVALID_PARAMETER);
    }
    name.len = 0;
    name.insensitive = 1;
    {
        uint16_t c0 = 0;

        (void)nt_copy_in(&c0, buf, 2u);
        if (c0 != '\\') {
            static const char p[] = "\\??\\";
            for (i = 0; i < 4u; i++) {
                name.chars[name.len++] = (uint16_t)(uint8_t)p[i];
            }
        }
    }
    if (name.len + chars > NT_NAME_MAX) {
        return NT_STATUS(VIBEOS_STATUS_OBJECT_NAME_INVALID);
    }
    if (nt_copy_in(&name.chars[name.len], buf, 2u * chars) != 0) {
        return NT_STATUS(VIBEOS_STATUS_ACCESS_VIOLATION);
    }
    name.len += chars;
    st = nt_vfs_path(&name, path, sizeof(path));
    if (st != VIBEOS_STATUS_SUCCESS) {
        return NT_STATUS(st);
    }
    st = nt_image_present(path);
    if (st != VIBEOS_STATUS_SUCCESS) {
        return NT_STATUS(st);
    }
    (void)nt_copy_in(&env, params + VIBEOS_RUPP_ENVIRONMENT, 8u);
    for (i = 0; i < 3u; i++) {
        hdr[6u + i] = 0;
        (void)nt_copy_in(&hdr[6u + i], params + VIBEOS_RUPP_STD_INPUT + 8u * i, 8u);
    }
    hdr[5] = (pflags & NT_PROCESS_CREATE_FLAGS_INHERIT_HANDLES) ? NT_SPAWN_INHERIT : 0u;

    ks_lock_preemptible(&g_spawn_lock);
    k.b = g_blk;
    k.cap = sizeof(g_blk);
    k.at = NT_SPAWN_VEC + 8u * (NT_SPAWN_ARGS + NT_SPAWN_ENVS + 2u);   /* header and vectors first */
    for (i = 0; path[i] && k.at + 1u < k.cap; i++) {
        k.b[k.at++] = (uint8_t)path[i];
    }
    k.b[k.at++] = 0;
    hdr[2] = k.at - i - 1u;
    cmd = -1;
    if (spawn_ustr(params, VIBEOS_RUPP_COMMAND_LINE, normalised, &buf, &chars) == 0) {
        cmd = spawn_utf8(&k, buf, chars);
    }
    nargs = cmd >= 0 ? spawn_split(&k, (uint32_t)cmd, args, NT_SPAWN_ARGS) : 0u;
    if (nargs == 0u) {
        args[0] = (uint32_t)hdr[2];   /* no command line: the image's path is argv[0] */
        nargs = 1;
    }
    /* The environment: NAME=value strings, each ended by a NUL, the block by
     * an empty one. */
    if (env) {
        uint64_t at = env;

        while (envs < NT_SPAWN_ENVS) {
            uint32_t n = 0;
            uint16_t c = 1;
            long o;

            while (n < 4096u && nt_copy_in(&c, at + 2u * n, 2u) == 0 && c) {
                n++;
            }
            if (n == 0u || c != 0u || (o = spawn_utf8(&k, at, n)) < 0) {
                break;
            }
            vec[NT_SPAWN_ARGS + 1u + envs++] = (uint64_t)o;
            at += 2u * (n + 1u);
        }
    }
    st = nt_alloc_rw(NT_SPAWN_BYTES, &base);
    if (st != VIBEOS_STATUS_SUCCESS) {
        ks_unlock_preemptible(&g_spawn_lock);
        return NT_STATUS(st);
    }
    hdr[0] = NT_SPAWN_MAGIC;
    hdr[1] = ks_id(ks_current())->tgid;
    hdr[2] += base;
    hdr[3] = base + NT_SPAWN_VEC;                              /* argv */
    hdr[4] = base + NT_SPAWN_VEC + 8u * (NT_SPAWN_ARGS + 1u);  /* envp */
    for (i = 0; i < NT_SPAWN_ARGS + 1u; i++) {
        vec[i] = i < nargs ? base + args[i] : 0u;
    }
    for (i = 0; i < envs; i++) {
        vec[NT_SPAWN_ARGS + 1u + i] += base;
    }
    vec[NT_SPAWN_ARGS + 1u + envs] = 0;
    for (i = 0; i < NT_SPAWN_HDR; i++) {
        *(uint64_t *)(void *)&k.b[8u * i] = hdr[i];
    }
    for (i = 0; i < NT_SPAWN_ARGS + NT_SPAWN_ENVS + 2u; i++) {
        *(uint64_t *)(void *)&k.b[NT_SPAWN_VEC + 8u * i] = (i <= NT_SPAWN_ARGS + envs + 1u) ? vec[i] : 0u;
    }
    if (nt_copy_out(base, k.b, k.at) != 0) {
        ks_unlock_preemptible(&g_spawn_lock);
        nt_free_alloc(base, NT_SPAWN_BYTES);
        return NT_STATUS(VIBEOS_STATUS_ACCESS_VIOLATION);
    }
    ks_unlock_preemptible(&g_spawn_lock);
    /* The child starts in ntdll, on a stack below the parent's own - its copy
     * of it, which nothing else in the child will use again. */
    pid = vibeos_linux_fork_win64(frame, self->entry, ((ks_regs_sp(frame) - 4096u) & ~0xFull) - 8u,
                                  NT_SPAWN_ENTRY_MARK, base, 0, (tflags & NT_THREAD_CREATE_SUSPENDED) != 0u);
    nt_free_alloc(base, NT_SPAWN_BYTES);
    if (pid < 0) {
        (void)nt_copy_out(ph, &h, 8u);
        return NT_STATUS(nt_status_of_errno(pid));
    }
    ks_irq_off();
    ks_lock(ks_sched_lock(), __func__);
    slot = ks_task_by_pid((uint32_t)pid);
    ks_unlock(ks_sched_lock());
    ks_irq_on();
    (void)nt_copy_out(ph, &h, 8u);
    if (slot < 0) {
        return NT_STATUS(VIBEOS_STATUS_PROCESS_IS_TERMINATING);
    }
    if (tflags & NT_THREAD_CREATE_SUSPENDED) {
        /* The fork left it stopped before it could run - sent here, after it
         * was ready, a fast child reached its end first. This is the count
         * NtResumeThread reads, set before this call returns, so no resume
         * can come first. */
        ks_id(slot)->nt_suspend = 1u;
    }
    r = nt_create_common(&g_nt_process_ops, ph, paccess ? paccess : NT_PROCESS_ALL_ACCESS, 0, 0,
                         process_init, &slot);
    if (r != NT_STATUS(VIBEOS_STATUS_SUCCESS)) {
        return r;
    }
    if (th) {
        st = nt_thread_handle_for(slot, taccess ? taccess : NT_PROCESS_ALL_ACCESS, th);
        if (st != VIBEOS_STATUS_SUCCESS) {
            return NT_STATUS(st);
        }
    }
    return NT_STATUS(VIBEOS_STATUS_SUCCESS);
}

/* NtQueryInformationProcess(Process, Class, Buffer, Length, ReturnLength).
 * ProcessBasicInformation only, in W1; any other class is
 * STATUS_INVALID_INFO_CLASS, which a program asking about a class it does not
 * need takes in its stride. A length that is not the structure's is
 * STATUS_INFO_LENGTH_MISMATCH, with nothing written - Windows' rule for this
 * class. */
static long nt_query_information_process(uint64_t process, uint64_t cls, uint64_t buf,
                                         uint64_t len, uint64_t ret_len) {
    vibeos_nt_process_basic_t pbi;
    uint32_t got = (uint32_t)sizeof(pbi);
    process_view_t v;
    uint32_t i, st;

    if ((uint32_t)cls != VIBEOS_NT_PROCESS_BASIC_INFORMATION) {
        return NT_STATUS(VIBEOS_STATUS_INVALID_INFO_CLASS);
    }
    if ((uint32_t)len != sizeof(pbi)) {
        return NT_STATUS(VIBEOS_STATUS_INFO_LENGTH_MISMATCH);
    }
    st = process_view(process, NT_PROCESS_QUERY_INFORMATION, &v);
    if (st != VIBEOS_STATUS_SUCCESS) {
        return NT_STATUS(st);
    }
    for (i = 0; i < sizeof(pbi); i++) {
        ((uint8_t *)&pbi)[i] = 0;
    }
    pbi.exit_status = v.exit_code;   /* STATUS_PENDING while it runs */
    /* Another process's PEB is at the address every process's is. */
    pbi.peb = ks_image(ks_current())->start_arg;
    pbi.affinity_mask = ks_cpus_online_mask();
    pbi.base_priority = 8u;     /* NORMAL_PRIORITY_CLASS's base */
    pbi.unique_process_id = v.pid;
    pbi.inherited_from_pid = v.ppid;
    process_unview(&v);
    /* The buffer was judged by the row; the copy is fault-safe for the sibling
     * that unmaps it in between. */
    if (nt_copy_out(buf, &pbi, sizeof(pbi)) != 0) {
        return NT_STATUS(VIBEOS_STATUS_ACCESS_VIOLATION);
    }
    if (ret_len != 0u &&
        nt_copy_out(ret_len, &got, sizeof(got)) != 0) {
        return NT_STATUS(VIBEOS_STATUS_ACCESS_VIOLATION);
    }
    return NT_STATUS(VIBEOS_STATUS_SUCCESS);
}

#define NT_PROCESS_SERVICES(X) \
    X(VIBEOS_NT_NR_NtTerminateProcess, NtTerminateProcess, EXIT_GROUP, NOPTR, \
      nt_terminate_process(ARG(0), NT_ARG32(1))) \
    X(VIBEOS_NT_NR_NtQueryInformationProcess, NtQueryInformationProcess, IDENTITY_GET, \
      PTRS(NT_OUT_BUF(2, 3), OUT_OPT(4, 4)), \
      nt_query_information_process(ARG(0), NT_ARG32(1), ARG(2), NT_ARG32(3), ARG(4))) \
    X(VIBEOS_NT_NR_NtOpenProcess, NtOpenProcess, IDENTITY_GET, PTRS(OUT(0, 8)), \
      nt_open_process(ARG(0), NT_ARG32(1), ARG(3))) \
    X(VIBEOS_NT_NR_NtCreateUserProcess, NtCreateUserProcess, EXEC, PTRS(OUT(0, 8)), \
      nt_create_user_process(FRAME, ARG(0), ARG(1), NT_ARG32(2), NT_ARG32(3), NT_ARG32(6), NT_ARG32(7), \
                             ARG(8)))

NT_DEFINE_SERVICES(process, NT_PROCESS_SERVICES)
