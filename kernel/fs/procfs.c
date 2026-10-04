/* /proc. See include/vibeos/procfs.h. */

#include "vibeos/procfs.h"
#include "vibeos/abi_linux.h"

/* ---- writing a file's text ----------------------------------------------------- */

/* A window onto the text: what falls before `skip` or past `cap` is counted and
 * dropped. A file is generated straight into the reader's buffer at whatever
 * offset it asked for, and never whole on a kernel stack - which is 8 KiB here,
 * and a process's maps can be longer than that. */
typedef struct {
    char *buf;
    uint32_t cap;
    uint64_t skip;
    uint64_t len;     /* counted past the end too: the length is what was wanted */
} pf_out_t;

static void pf_char(pf_out_t *o, char c) {
    if (o->len >= o->skip && o->len - o->skip < o->cap) {
        o->buf[o->len - o->skip] = c;
    }
    o->len++;
}

static void pf_str(pf_out_t *o, const char *s) {
    while (s && *s) {
        pf_char(o, *s++);
    }
}

/* `v` in decimal, at least `width` columns, padded on the left with `pad`. */
static void pf_num(pf_out_t *o, uint64_t v, uint32_t width, char pad) {
    char d[20];
    uint32_t n = 0, i;

    do {
        d[n++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v != 0u);
    for (i = n; i < width; i++) {
        pf_char(o, pad);
    }
    while (n > 0u) {
        pf_char(o, d[--n]);
    }
}

static void pf_u(pf_out_t *o, uint64_t v) {
    pf_num(o, v, 0, ' ');
}

static void pf_i(pf_out_t *o, int64_t v) {
    if (v < 0) {
        pf_char(o, '-');
        pf_u(o, (uint64_t)(-(v + 1)) + 1u);
    } else {
        pf_u(o, (uint64_t)v);
    }
}

/* Lower-case hexadecimal, zero-padded to `width` digits. */
static void pf_x(pf_out_t *o, uint64_t v, uint32_t width) {
    char d[16];
    uint32_t n = 0, i;

    do {
        d[n++] = "0123456789abcdef"[v & 15u];
        v >>= 4;
    } while (v != 0u);
    for (i = n; i < width; i++) {
        pf_char(o, '0');
    }
    while (n > 0u) {
        pf_char(o, d[--n]);
    }
}

/* "Name:" padded to sixteen columns, the number right-aligned in eight, " kB":
 * Linux's own meminfo line, which programs read with scanf and with awk both. */
static void pf_kb(pf_out_t *o, const char *name, uint64_t kb) {
    uint32_t col = 0;

    for (; *name; name++, col++) {
        pf_char(o, *name);
    }
    pf_char(o, ':');
    for (col++; col < 16u; col++) {
        pf_char(o, ' ');
    }
    pf_num(o, kb, 8, ' ');
    pf_str(o, " kB\n");
}

/* status's "Name:\t" lines. */
static void pf_tag(pf_out_t *o, const char *name) {
    pf_str(o, name);
    pf_str(o, ":\t");
}

static void pf_tag_u(pf_out_t *o, const char *name, uint64_t v) {
    pf_tag(o, name);
    pf_u(o, v);
    pf_char(o, '\n');
}

/* ---- the files that are about the machine ------------------------------------------ */

static void pf_meminfo(const vibeos_procfs_t *pf, pf_out_t *o) {
    vibeos_procfs_mem_t m = {0, 0, 0, 0, 0, 0};

    if (pf->mem) {
        pf->mem(&m);
    }
    pf_kb(o, "MemTotal", m.total_kb);
    pf_kb(o, "MemFree", m.free_kb);
    pf_kb(o, "MemAvailable", m.available_kb);
    pf_kb(o, "Buffers", 0);
    pf_kb(o, "Cached", m.cached_kb);
    pf_kb(o, "SwapTotal", m.swap_total_kb);
    pf_kb(o, "SwapFree", m.swap_free_kb);
}

static void pf_pid_max(const vibeos_procfs_t *pf, pf_out_t *o) {
    pf_u(o, pf->pid_max);
    pf_char(o, '\n');
}

/* Linux's layout, one record per processor and a blank line after each. The
 * clock is measured, not configured; bogomips is twice it, as Linux's has been
 * on every x86 since it stopped measuring a delay loop. */
static void pf_cpuinfo(const vibeos_procfs_t *pf, pf_out_t *o) {
    const vibeos_procfs_cpu_t *c = &pf->cpu;
    uint32_t khz = pf->cpu_khz ? pf->cpu_khz() : 0u;
    uint32_t n = pf->cpus ? pf->cpus() : 1u, i;

    for (i = 0; i < n; i++) {
        pf_str(o, "processor\t: ");
        pf_u(o, i);
        pf_str(o, "\nvendor_id\t: ");
        pf_str(o, c->vendor);
        pf_str(o, "\ncpu family\t: ");
        pf_u(o, c->family);
        pf_str(o, "\nmodel\t\t: ");
        pf_u(o, c->model);
        pf_str(o, "\nmodel name\t: ");
        pf_str(o, c->model_name);
        pf_str(o, "\nstepping\t: ");
        pf_u(o, c->stepping);
        pf_str(o, "\ncpu MHz\t\t: ");
        pf_u(o, khz / 1000u);
        pf_char(o, '.');
        pf_num(o, khz % 1000u, 3, '0');
        pf_str(o, "\nphysical id\t: 0\nsiblings\t: ");
        pf_u(o, n);
        pf_str(o, "\ncore id\t\t: ");
        pf_u(o, i);
        pf_str(o, "\ncpu cores\t: ");
        pf_u(o, n);
        pf_str(o, "\napicid\t\t: ");
        pf_u(o, i);
        pf_str(o, "\ninitial apicid\t: ");
        pf_u(o, i);
        pf_str(o, "\nfpu\t\t: yes\nfpu_exception\t: yes\ncpuid level\t: ");
        pf_u(o, c->cpuid_level);
        pf_str(o, "\nwp\t\t: yes\nflags\t\t: ");
        pf_str(o, c->flags);
        pf_str(o, "\nbogomips\t: ");
        pf_u(o, (2u * (uint64_t)khz) / 1000u);
        pf_char(o, '.');
        pf_num(o, ((2u * (uint64_t)khz) % 1000u) / 10u, 2, '0');
        pf_str(o, "\nclflush size\t: 64\ncache_alignment\t: 64\naddress sizes\t: ");
        pf_u(o, c->phys_bits);
        pf_str(o, " bits physical, ");
        pf_u(o, c->virt_bits);
        pf_str(o, " bits virtual\npower management:\n\n");
    }
}

static void pf_version(const vibeos_procfs_t *pf, pf_out_t *o) {
    pf_str(o, pf->version ? pf->version : "VibeOS");
    pf_char(o, '\n');
}

/* Seconds since boot to the hundredth, then the time every core spent idle -
 * which nothing here counts, so it says none. */
static void pf_uptime(const vibeos_procfs_t *pf, pf_out_t *o) {
    uint64_t ms = pf->uptime_ms ? pf->uptime_ms() : 0u;

    pf_u(o, ms / 1000u);
    pf_char(o, '.');
    pf_num(o, (ms % 1000u) / 10u, 2, '0');
    pf_str(o, " 0.00\n");
}

/* The load averages are not computed here, so they are zero; the rest of the
 * line is true: processes running, processes in all, the newest pid. */
static void pf_loadavg(const vibeos_procfs_t *pf, pf_out_t *o) {
    uint32_t pid = 0, all = 0, running = 0, last = 0;
    vibeos_procfs_proc_t p;

    while (pf->next_pid && (pid = pf->next_pid(pid)) != 0u) {
        if (pf->proc && pf->proc(pid, &p) == 0) {
            all++;
            running += p.state == 'R' ? 1u : 0u;
            last = pid;
        }
    }
    pf_str(o, "0.00 0.00 0.00 ");
    pf_u(o, running);
    pf_char(o, '/');
    pf_u(o, all);
    pf_char(o, ' ');
    pf_u(o, last);
    pf_char(o, '\n');
}

/* Every mount, as Linux's mounts file lists them: the source, where, the type,
 * the options, two zeros. A filesystem here has no device name to give, so the
 * source is its type, as it is for Linux's own virtual ones. */
static void pf_mounts(pf_out_t *o) {
    uint32_t i, n = vibeos_fs_mount_count();

    for (i = 0; i < n; i++) {
        vibeos_fsmount_t *m = vibeos_fs_mount_at(i);
        const char *path = vibeos_fs_mount_path(i);
        const char *type = m ? vibeos_fs_type(m) : 0;

        if (!m || !path || !type) {
            continue;
        }
        if (type[0] == 'f' && type[1] == 'a' && type[2] == 't' && type[3] == 0) {
            type = "vfat";   /* Linux's name for FAT with long names */
        }
        pf_str(o, type);
        pf_char(o, ' ');
        pf_str(o, path);
        pf_char(o, ' ');
        pf_str(o, type);
        pf_str(o, vibeos_fs_writable(m) ? " rw 0 0\n" : " ro 0 0\n");
    }
}

typedef struct {
    const char *path;                         /* under /proc, no leading slash */
    void (*text)(const vibeos_procfs_t *pf, pf_out_t *o);
} pf_file_t;

static const pf_file_t g_files[] = {
    {"cpuinfo", pf_cpuinfo},
    {"loadavg", pf_loadavg},
    {"meminfo", pf_meminfo},
    {"uptime", pf_uptime},
    {"version", pf_version},
    {"sys/kernel/pid_max", pf_pid_max},
};
#define PF_FILES ((uint32_t)(sizeof(g_files) / sizeof(g_files[0])))

/* ---- the files that are about a process --------------------------------------------- */

static void pf_p_stat(const vibeos_procfs_t *pf, const vibeos_procfs_proc_t *p, pf_out_t *o) {
    uint32_t i;

    (void)pf;
    pf_u(o, p->pid);
    pf_str(o, " (");
    pf_str(o, p->comm);
    pf_str(o, ") ");
    pf_char(o, p->state);
    pf_char(o, ' ');
    pf_u(o, p->ppid);
    pf_char(o, ' ');
    pf_u(o, p->pgid);
    pf_char(o, ' ');
    pf_u(o, p->sid);
    pf_char(o, ' ');
    pf_u(o, p->tty);
    pf_char(o, ' ');
    pf_i(o, p->tty ? (int64_t)p->tty_pgid : -1);
    pf_str(o, " 0 0 0 0 0 ");          /* flags, the four fault counts */
    pf_u(o, p->utime);
    pf_char(o, ' ');
    pf_u(o, p->stime);
    pf_char(o, ' ');
    pf_u(o, p->cutime);
    pf_char(o, ' ');
    pf_u(o, p->cstime);
    pf_char(o, ' ');
    pf_i(o, 20 + (int64_t)p->nice);    /* priority, as Linux prints a normal task's */
    pf_char(o, ' ');
    pf_i(o, p->nice);
    pf_char(o, ' ');
    pf_u(o, p->threads);
    pf_str(o, " 0 ");                   /* itrealvalue, always 0 since 2.6.17 */
    pf_u(o, p->start);
    pf_char(o, ' ');
    pf_u(o, p->vsize);
    pf_char(o, ' ');
    pf_u(o, p->rss);
    pf_char(o, ' ');
    pf_u(o, p->rss_limit);
    pf_str(o, " 0 0 0 0 0 ");          /* code, stack and the saved registers */
    pf_u(o, p->sig_pending);
    pf_char(o, ' ');
    pf_u(o, p->sig_blocked);
    pf_char(o, ' ');
    pf_u(o, p->sig_ignored);
    pf_char(o, ' ');
    pf_u(o, p->sig_caught);
    pf_str(o, " 0 0 0 17");            /* wchan, nswap, cnswap; exit_signal SIGCHLD */
    for (i = 39; i <= 52u; i++) {
        pf_str(o, " 0");               /* processor to exit_code */
    }
    pf_char(o, '\n');
}

static void pf_p_statm(const vibeos_procfs_t *pf, const vibeos_procfs_proc_t *p, pf_out_t *o) {
    (void)pf;
    pf_u(o, p->vsize / 4096u);
    pf_char(o, ' ');
    pf_u(o, p->rss);
    pf_str(o, " 0 0 0 0 0\n");
}

static const char *pf_state_name(char s) {
    switch (s) {
        case 'R': return "R (running)";
        case 'T': return "T (stopped)";
        case 'Z': return "Z (zombie)";
        default: return "S (sleeping)";
    }
}

static void pf_p_status(const vibeos_procfs_t *pf, const vibeos_procfs_proc_t *p, pf_out_t *o) {
    uint32_t i;
    int root = p->uid[1] == 0u;

    (void)pf;
    pf_tag(o, "Name");
    pf_str(o, p->comm);
    pf_char(o, '\n');
    pf_tag(o, "Umask");
    pf_num(o, (p->umask >> 9) & 7u, 1, '0');
    pf_num(o, (p->umask >> 6) & 7u, 1, '0');
    pf_num(o, (p->umask >> 3) & 7u, 1, '0');
    pf_num(o, p->umask & 7u, 1, '0');
    pf_char(o, '\n');
    pf_tag(o, "State");
    pf_str(o, pf_state_name(p->state));
    pf_char(o, '\n');
    pf_tag_u(o, "Tgid", p->pid);
    pf_tag_u(o, "Ngid", 0);
    pf_tag_u(o, "Pid", p->pid);
    pf_tag_u(o, "PPid", p->ppid);
    pf_tag_u(o, "TracerPid", 0);
    pf_tag(o, "Uid");
    for (i = 0; i < 4u; i++) {
        pf_u(o, p->uid[i]);
        pf_char(o, i < 3u ? '\t' : '\n');
    }
    pf_tag(o, "Gid");
    for (i = 0; i < 4u; i++) {
        pf_u(o, p->gid[i]);
        pf_char(o, i < 3u ? '\t' : '\n');
    }
    pf_tag_u(o, "FDSize", 64);
    pf_tag(o, "Groups");
    pf_char(o, '\n');
    pf_tag(o, "VmSize");
    pf_num(o, p->vsize / 1024u, 8, ' ');
    pf_str(o, " kB\n");
    pf_tag(o, "VmRSS");
    pf_num(o, p->rss * 4u, 8, ' ');
    pf_str(o, " kB\n");
    pf_tag_u(o, "Threads", p->threads);
    pf_tag(o, "SigPnd");
    pf_x(o, p->sig_pending, 16);
    pf_str(o, "\nShdPnd:\t0000000000000000\nSigBlk:\t");
    pf_x(o, p->sig_blocked, 16);
    pf_str(o, "\nSigIgn:\t");
    pf_x(o, p->sig_ignored, 16);
    pf_str(o, "\nSigCgt:\t");
    pf_x(o, p->sig_caught, 16);
    /* Capabilities do not exist here: the superuser may do everything and
     * nobody else anything extra, which is what these masks say. */
    pf_str(o, "\nCapInh:\t0000000000000000\nCapPrm:\t");
    pf_str(o, root ? "000001ffffffffff" : "0000000000000000");
    pf_str(o, "\nCapEff:\t");
    pf_str(o, root ? "000001ffffffffff" : "0000000000000000");
    pf_str(o, "\nCapBnd:\t000001ffffffffff\nCapAmb:\t0000000000000000\nNoNewPrivs:\t0\nSeccomp:\t0\n");
}

static void pf_p_cmdline(const vibeos_procfs_t *pf, const vibeos_procfs_proc_t *p, pf_out_t *o) {
    char args[256];
    long n = pf->cmdline ? pf->cmdline(p->pid, args, (uint32_t)sizeof(args)) : 0;
    long i;

    for (i = 0; i < n && i < (long)sizeof(args); i++) {
        pf_char(o, args[i]);
    }
}

static void pf_p_comm(const vibeos_procfs_t *pf, const vibeos_procfs_proc_t *p, pf_out_t *o) {
    (void)pf;
    pf_str(o, p->comm);
    pf_char(o, '\n');
}

/* Linux's maps line: range, permissions, offset, device, inode, and the name
 * from column 73 when there is one. A region here does not remember which file
 * it came from by name, so only the heap and the stack are named. */
static void pf_p_maps(const vibeos_procfs_t *pf, const vibeos_procfs_proc_t *p, pf_out_t *o) {
    vibeos_procfs_map_t m;
    uint32_t i;

    for (i = 0; pf->map && pf->map(p->pid, i, &m) == 0; i++) {
        uint64_t at = o->len;

        pf_x(o, m.start, 8);
        pf_char(o, '-');
        pf_x(o, m.end, 8);
        pf_char(o, ' ');
        pf_char(o, (m.prot & VIBEOS_PROCFS_MAP_R) ? 'r' : '-');
        pf_char(o, (m.prot & VIBEOS_PROCFS_MAP_W) ? 'w' : '-');
        pf_char(o, (m.prot & VIBEOS_PROCFS_MAP_X) ? 'x' : '-');
        pf_char(o, (m.prot & VIBEOS_PROCFS_MAP_SHARED) ? 's' : 'p');
        pf_char(o, ' ');
        pf_x(o, m.offset, 8);
        pf_str(o, " 00:00 0 ");
        if (m.name[0]) {
            while (o->len - at < 73u) {
                pf_char(o, ' ');
            }
            m.name[sizeof(m.name) - 1u] = 0;
            pf_str(o, m.name);
        }
        pf_char(o, '\n');
    }
}

static void pf_p_mounts(const vibeos_procfs_t *pf, const vibeos_procfs_proc_t *p, pf_out_t *o) {
    (void)pf;
    (void)p;
    pf_mounts(o);
}

#define PF_E_FILE 0u
#define PF_E_LINK 1u
#define PF_E_DIR  2u

typedef struct {
    const char *name;
    uint32_t type;
    uint32_t mode;
    uint32_t link;            /* VIBEOS_PROCFS_LINK_*, for a link */
    void (*text)(const vibeos_procfs_t *pf, const vibeos_procfs_proc_t *p, pf_out_t *o);
} pf_pid_entry_t;

static const pf_pid_entry_t g_pid_files[] = {
    {"cmdline", PF_E_FILE, 0444u, 0, pf_p_cmdline},
    {"comm", PF_E_FILE, 0644u, 0, pf_p_comm},
    {"cwd", PF_E_LINK, 0777u, VIBEOS_PROCFS_LINK_CWD, 0},
    {"exe", PF_E_LINK, 0777u, VIBEOS_PROCFS_LINK_EXE, 0},
    {"fd", PF_E_DIR, 0500u, 0, 0},
    {"maps", PF_E_FILE, 0444u, 0, pf_p_maps},
    {"mounts", PF_E_FILE, 0444u, 0, pf_p_mounts},
    {"root", PF_E_LINK, 0777u, VIBEOS_PROCFS_LINK_ROOT, 0},
    {"stat", PF_E_FILE, 0444u, 0, pf_p_stat},
    {"statm", PF_E_FILE, 0444u, 0, pf_p_statm},
    {"status", PF_E_FILE, 0444u, 0, pf_p_status},
};
#define PF_PID_FILES ((uint32_t)(sizeof(g_pid_files) / sizeof(g_pid_files[0])))

/* ---- names ------------------------------------------------------------------------------ */

/* Node ids: what kind of thing, which one of its kind, and whose. The static
 * tree's directories are all 1 - nothing is ever done to one by id. */
#define PF_K_DIR     1u   /* a directory of the static tree, the root among them */
#define PF_K_FILE    2u   /* g_files[arg] */
#define PF_K_SELF    3u   /* the link "self" */
#define PF_K_MOUNTS  4u   /* the link "mounts" */
#define PF_K_PID     5u   /* a process's directory */
#define PF_K_ENTRY   6u   /* g_pid_files[arg] in it */
#define PF_K_FD      7u   /* descriptor arg's link in its fd directory */
#define PF_ID(kind, arg, pid) (((uint64_t)(pid) << 32) | ((uint64_t)(arg) << 8) | (uint64_t)(kind))

typedef struct {
    uint32_t kind;
    uint32_t arg;
    uint32_t pid;
    vibeos_procfs_proc_t p;   /* the process, when there is one */
} pf_where_t;

/* There are no directories stored anywhere in the static tree: one exists
 * because a file's path runs through it. `path` names a directory when it is
 * empty or is the start of some file's path up to a slash; this returns how
 * much of that file's path it covers, slash included (0 for the root), or -1. */
static int pf_dir_prefix(const char *path, uint32_t file) {
    const char *f = g_files[file].path;
    uint32_t i = 0;

    if (*path == 0) {
        return 0;
    }
    while (path[i] && path[i] == f[i]) {
        i++;
    }
    while (path[i] == '/') {
        path++;          /* trailing slashes */
    }
    return (path[i] == 0 && f[i] == '/') ? (int)(i + 1u) : -1;
}

static int pf_same(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == 0 && *b == 0;
}

/* Does the component at `c`, `n` bytes long, spell `name`? */
static int pf_is(const char *c, uint32_t n, const char *name) {
    uint32_t i;

    for (i = 0; i < n; i++) {
        if (name[i] != c[i]) {
            return 0;
        }
    }
    return name[n] == 0;
}

/* A component of decimal digits, as a number; 0 if it is not one. */
static uint32_t pf_number(const char *c, uint32_t n) {
    uint64_t v = 0;
    uint32_t i;

    if (n == 0u || n > 10u || (n > 1u && c[0] == '0')) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        if (c[i] < '0' || c[i] > '9') {
            return 0;
        }
        v = v * 10u + (uint64_t)(c[i] - '0');
    }
    return v > 0xffffffffull ? 0u : (uint32_t)v;
}

/* The next component of *p: its start and length, *p moved past it and the
 * slashes after it. */
static const char *pf_component(const char **p, uint32_t *n) {
    const char *c = *p;
    uint32_t k = 0;

    while (c[k] && c[k] != '/') {
        k++;
    }
    *n = k;
    *p = c + k;
    while (**p == '/') {
        (*p)++;
    }
    return c;
}

static int pf_parse(const vibeos_procfs_t *pf, const char *path, pf_where_t *w) {
    const char *rest, *c;
    uint32_t n, pid, i;

    while (*path == '/') {
        path++;
    }
    w->kind = PF_K_DIR;
    w->arg = 0;
    w->pid = 0;
    if (*path == 0) {
        return 0;
    }
    rest = path;
    c = pf_component(&rest, &n);
    pid = pf_number(c, n);
    if (pf_is(c, n, "self")) {
        pid = pf->self ? pf->self() : 0u;
        if (*rest == 0) {
            w->kind = PF_K_SELF;
            return pid ? 0 : -VIBEOS_ENOENT;
        }
        /* "self/..." in one piece: a walk replaces the link before it gets
         * here, but the kernel asking for itself by name need not walk. */
    }
    if (pid != 0u) {
        if (!pf->proc || pf->proc(pid, &w->p) != 0) {
            return -VIBEOS_ENOENT;
        }
        w->pid = pid;
        if (*rest == 0) {
            w->kind = PF_K_PID;
            return 0;
        }
        c = pf_component(&rest, &n);
        for (i = 0; i < PF_PID_FILES; i++) {
            if (pf_is(c, n, g_pid_files[i].name)) {
                break;
            }
        }
        if (i == PF_PID_FILES) {
            return -VIBEOS_ENOENT;
        }
        w->kind = PF_K_ENTRY;
        w->arg = i;
        if (g_pid_files[i].type == PF_E_DIR && *rest != 0) {
            uint32_t fd, got;

            c = pf_component(&rest, &n);
            fd = pf_number(c, n);
            if (fd == 0u && !pf_is(c, n, "0")) {
                return -VIBEOS_ENOENT;
            }
            if (*rest != 0) {
                return -VIBEOS_ENOTDIR;
            }
            if (!pf->next_fd || pf->next_fd(pid, fd, &got) != 0 || got != fd) {
                return -VIBEOS_ENOENT;
            }
            w->kind = PF_K_FD;
            w->arg = fd;
            return 0;
        }
        return *rest == 0 ? 0 : -VIBEOS_ENOTDIR;
    }
    if (pf_is(c, n, "mounts") && *rest == 0) {
        w->kind = PF_K_MOUNTS;
        return 0;
    }
    for (i = 0; i < PF_FILES; i++) {
        if (pf_same(path, g_files[i].path)) {
            w->kind = PF_K_FILE;
            w->arg = i;
            return 0;
        }
    }
    for (i = 0; i < PF_FILES; i++) {
        if (pf_dir_prefix(path, i) >= 0) {
            return 0;   /* a directory of the static tree */
        }
    }
    return -VIBEOS_ENOENT;
}

/* The text of a file the parse found, through `o`; -ESRCH if its process has
 * gone since. */
static int pf_generate(const vibeos_procfs_t *pf, pf_where_t *w, pf_out_t *o) {
    if (w->kind == PF_K_FILE && w->arg < PF_FILES) {
        g_files[w->arg].text(pf, o);
        return 0;
    }
    if (w->kind == PF_K_ENTRY && g_pid_files[w->arg].type == PF_E_FILE) {
        g_pid_files[w->arg].text(pf, &w->p, o);
        return 0;
    }
    return -VIBEOS_EINVAL;
}

/* ---- the operations ---------------------------------------------------------------------- */

static int pf_lookup(void *fs, const char *path, vibeos_fs_node_t *out) {
    const vibeos_procfs_t *pf = (const vibeos_procfs_t *)fs;
    pf_where_t w;
    int r = pf_parse(pf, path, &w);

    if (r != 0) {
        return r;
    }
    out->id = PF_ID(w.kind, w.arg, w.pid);
    out->size = 0;
    out->is_dir = 0;
    out->nlink = 1u;
    if (w.pid != 0u) {
        out->uid = w.p.uid[1];
        out->gid = w.p.gid[1];
    }
    switch (w.kind) {
        case PF_K_DIR:
            out->id = 1u;
            out->is_dir = 1;
            out->mode = VIBEOS_S_IFDIR | 0555u;
            out->nlink = 2u;
            break;
        case PF_K_FILE: {
            /* A machine file says how long it is, as it always has here: its
             * text is short and asked for whole. A process's says 0, as Linux's
             * do, and is read to its end. */
            pf_out_t o = {0, 0, 0, 0};
            (void)pf_generate(pf, &w, &o);
            out->size = o.len;
            out->mode = VIBEOS_S_IFREG | 0444u;
            break;
        }
        case PF_K_SELF:
        case PF_K_MOUNTS:
            out->mode = VIBEOS_S_IFLNK | 0777u;
            break;
        case PF_K_PID:
            out->is_dir = 1;
            out->mode = VIBEOS_S_IFDIR | 0555u;
            out->nlink = 2u;
            break;
        case PF_K_ENTRY: {
            const pf_pid_entry_t *e = &g_pid_files[w.arg];
            out->is_dir = e->type == PF_E_DIR;
            out->mode = e->mode | (e->type == PF_E_DIR ? VIBEOS_S_IFDIR
                                   : e->type == PF_E_LINK ? VIBEOS_S_IFLNK : VIBEOS_S_IFREG);
            out->nlink = e->type == PF_E_DIR ? 2u : 1u;
            break;
        }
        default:   /* PF_K_FD */
            out->mode = VIBEOS_S_IFLNK | 0700u;
            break;
    }
    return 0;
}

static long pf_read_at(void *fs, const vibeos_fs_node_t *node, uint64_t off, void *buf, uint32_t len) {
    const vibeos_procfs_t *pf = (const vibeos_procfs_t *)fs;
    pf_where_t w;
    pf_out_t o;

    w.kind = (uint32_t)(node->id & 0xffu);
    w.arg = (uint32_t)((node->id >> 8) & 0xffffffu);
    w.pid = (uint32_t)(node->id >> 32);
    if (w.kind == PF_K_DIR || w.kind == PF_K_PID ||
        (w.kind == PF_K_ENTRY && w.arg < PF_PID_FILES && g_pid_files[w.arg].type == PF_E_DIR)) {
        return -VIBEOS_EISDIR;
    }
    if (w.kind == PF_K_ENTRY) {
        if (w.arg >= PF_PID_FILES || !pf->proc || pf->proc(w.pid, &w.p) != 0) {
            return -VIBEOS_ESRCH;   /* the process ended after the file was opened */
        }
    } else if (w.kind != PF_K_FILE || w.arg >= PF_FILES) {
        return -VIBEOS_EINVAL;
    }
    o.buf = (char *)buf;
    o.cap = len;
    o.skip = off;
    o.len = 0;
    if (pf_generate(pf, &w, &o) != 0) {
        return -VIBEOS_EINVAL;
    }
    if (o.len <= off) {
        return 0;
    }
    return (long)(o.len - off < len ? o.len - off : len);
}

static long pf_readlink(void *fs, const char *path, char *buf, uint32_t cap) {
    const vibeos_procfs_t *pf = (const vibeos_procfs_t *)fs;
    pf_where_t w;
    pf_out_t o = {buf, cap, 0, 0};
    int r = pf_parse(pf, path, &w);

    if (r != 0) {
        return r;
    }
    switch (w.kind) {
        case PF_K_SELF:
            pf_u(&o, pf->self ? pf->self() : 0u);
            break;
        case PF_K_MOUNTS:
            pf_str(&o, "self/mounts");
            break;
        case PF_K_ENTRY:
            if (g_pid_files[w.arg].type != PF_E_LINK || !pf->link) {
                return -VIBEOS_EINVAL;
            }
            return pf->link(w.pid, g_pid_files[w.arg].link, 0, buf, cap);
        case PF_K_FD:
            return pf->link ? pf->link(w.pid, VIBEOS_PROCFS_LINK_FD, w.arg, buf, cap) : -VIBEOS_EINVAL;
        default:
            return -VIBEOS_EINVAL;
    }
    return (long)(o.len < cap ? o.len : cap);
}

static void pf_name(char *name, uint32_t cap, const char *src, uint32_t n) {
    uint32_t k;

    for (k = 0; k + 1u < cap && k < n; k++) {
        name[k] = src[k];
    }
    name[k] = 0;
}

/* The `index`th entry of a static directory: the next component of every
 * file's path that runs through it, each once - the first file that has a
 * component names it, and a later file with the same one is passed over.
 * 0, or -1 past the last. */
static int pf_static_entry(const vibeos_procfs_t *pf, const char *path, uint32_t index, char *name,
                           uint32_t cap, uint64_t *out_size, int *out_is_dir) {
    uint32_t i, seen = 0;

    for (i = 0; i < PF_FILES; i++) {
        int at = pf_dir_prefix(path, i);
        const char *c;
        uint32_t len = 0, j, k;
        int dup = 0;

        if (at < 0) {
            continue;
        }
        c = g_files[i].path + at;
        while (c[len] && c[len] != '/') {
            len++;
        }
        for (j = 0; j < i && !dup; j++) {
            int at2 = pf_dir_prefix(path, j);
            if (at2 == at) {
                const char *d = g_files[j].path + at2;
                for (k = 0; k < len && d[k] == c[k]; k++) {
                }
                dup = k == len && (d[len] == 0 || d[len] == '/');
            }
        }
        if (dup || seen++ != index) {
            continue;
        }
        pf_name(name, cap, c, len);
        if (out_is_dir) {
            *out_is_dir = c[len] == '/';
        }
        if (out_size) {
            pf_out_t o = {0, 0, 0, 0};
            if (c[len] != '/') {
                g_files[i].text(pf, &o);
            }
            *out_size = o.len;
        }
        return 0;
    }
    return -1;
}

static int pf_list(void *fs, const char *path, uint32_t index, char *name, uint32_t cap,
                   uint64_t *out_size, int *out_is_dir) {
    const vibeos_procfs_t *pf = (const vibeos_procfs_t *)fs;
    pf_where_t w;
    uint32_t n = 0, k;
    int r = pf_parse(pf, path, &w);

    if (r != 0) {
        return r;
    }
    if (cap == 0u) {
        return -1;
    }
    if (out_size) {
        *out_size = 0;
    }
    if (out_is_dir) {
        *out_is_dir = 0;
    }
    while (*path == '/') {
        path++;
    }
    if (w.kind == PF_K_DIR) {
        if (pf_static_entry(pf, path, index, name, cap, out_size, out_is_dir) == 0) {
            return 0;
        }
        if (*path != 0) {
            return -1;   /* only the root has more than the static tree */
        }
        while (pf_static_entry(pf, path, n, name, cap, 0, 0) == 0) {
            n++;
        }
        if (index == n) {
            pf_name(name, cap, "self", 4);
            return 0;
        }
        if (index == n + 1u) {
            pf_name(name, cap, "mounts", 6);
            return 0;
        }
        {
            uint32_t pid = 0;
            pf_out_t o = {name, cap - 1u, 0, 0};

            for (k = n + 2u; k <= index; k++) {
                pid = pf->next_pid ? pf->next_pid(pid) : 0u;
                if (pid == 0u) {
                    return -1;
                }
            }
            pf_u(&o, pid);
            name[o.len < cap - 1u ? o.len : cap - 1u] = 0;
            if (out_is_dir) {
                *out_is_dir = 1;
            }
            return 0;
        }
    }
    if (w.kind == PF_K_PID) {
        if (index >= PF_PID_FILES) {
            return -1;
        }
        k = 0;
        while (g_pid_files[index].name[k]) {
            k++;
        }
        pf_name(name, cap, g_pid_files[index].name, k);
        if (out_is_dir) {
            *out_is_dir = g_pid_files[index].type == PF_E_DIR;
        }
        return 0;
    }
    if (w.kind == PF_K_ENTRY && g_pid_files[w.arg].type == PF_E_DIR) {
        uint32_t fd = 0, at = 0;
        pf_out_t o = {name, cap - 1u, 0, 0};

        for (k = 0; k <= index; k++) {
            if (!pf->next_fd || pf->next_fd(w.pid, at, &fd) != 0) {
                return -1;
            }
            at = fd + 1u;
        }
        pf_u(&o, fd);
        name[o.len < cap - 1u ? o.len : cap - 1u] = 0;
        return 0;
    }
    return -VIBEOS_ENOTDIR;
}

static int pf_statfs(void *fs, vibeos_fs_statfs_t *out) {
    (void)fs;
    out->magic = 0x9fa0u;   /* PROC_SUPER_MAGIC */
    out->block_size = 4096u;
    out->name_max = 255u;
    out->read_only = 1;
    return 0;
}

/* No lock in this file, because there is nothing to hold one over: it keeps no
 * state. The only data it defines is the tables above and the operations below,
 * all constant; every number it prints is asked of the mount's owner at the
 * moment of the read. */
static const vibeos_fs_ops_t g_procfs_ops = {
    .lookup = pf_lookup,
    .read_at = pf_read_at,
    .list = pf_list,
    .readlink = pf_readlink,
    .statfs = pf_statfs,
};

const vibeos_fs_ops_t *vibeos_procfs_ops(void) {
    return &g_procfs_ops;
}
