/* The host tests' kernel services: see ksvc_fake.h. */

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ksvc_fake.h"
#include "vibeos/filelock.h"
#include "vibeos/tmpfs.h"
#include "vibeos/fdtable.h"
#include "vibeos/file.h"
#include "vibeos/fileops.h"
#include "vibeos/pipe.h"
/* Signal numbers and errno values: the kernel numbers signals as Linux does. */
#include "vibeos/abi_linux.h"

/* ---- state ------------------------------------------------------------------ */

typedef struct {
    vibeos_task_t id;
    vibeos_procstate_t *ps;
    vibeos_image_t img;
    uint32_t seq;
    uint64_t tls;
    const char *ready_by;
} kf_task_t;

#define KF_PROCS 16u

static kf_task_t g_t[KF_SLOTS];
static vibeos_procstate_t g_ps[KF_PROCS];
static int g_cur = -1;
static uint32_t g_next_pid_v = 100;
static uint64_t g_ticks_v;
static uint32_t g_illegal;

static uint8_t g_user[KF_USER_BYTES] __attribute__((aligned(4096)));
static uint64_t g_user_used;
static uint64_t g_fault_base, g_fault_len;

static jmp_buf g_escape;
static int g_in_call;
static uint64_t g_exit_code_v;
static const char *g_panic_v;
static uint32_t g_idles;
static uint64_t g_next_sp_v;

static char g_con[16384];
static uint32_t g_con_len;
static int g_lock_depth, g_lock_bad;

static vibeos_lock_t g_sched_lock_v, g_net_lock_v;
static vibeos_fsmount_t g_root;
static uint32_t g_fg_pgid;

/* ---- the filesystem --------------------------------------------------------------- */

#define KF_FILES 16u

typedef struct {
    char path[64];
    uint8_t data[512];
    uint32_t len;
    int is_dir;
    int used;
} kf_file_t;

static kf_file_t g_files[KF_FILES];

static int kf_find(const char *path) {
    uint32_t i;
    const char *p = path[0] == '/' ? path + 1 : path;

    for (i = 0; i < KF_FILES; i++) {
        const char *q = g_files[i].path[0] == '/' ? g_files[i].path + 1 : g_files[i].path;
        if (g_files[i].used && strcmp(p, q) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static int kf_fs_lookup(void *fs, const char *path, vibeos_fs_node_t *out) {
    int i;
    (void)fs;
    if (path[0] == 0 || (path[0] == '/' && path[1] == 0)) {
        out->id = 0;          /* the volume's root, however it is spelled */
        out->size = 0;
        out->is_dir = 1;
        return 0;
    }
    i = kf_find(path);
    if (i < 0) {
        return -1;
    }
    out->id = (uint64_t)i + 1u;
    out->size = g_files[i].len;
    out->is_dir = g_files[i].is_dir;
    return 0;
}

static long kf_fs_read_at(void *fs, const vibeos_fs_node_t *node, uint64_t offset,
                          void *buf, uint32_t len) {
    const kf_file_t *f;
    (void)fs;
    if (node->id == 0u || node->id > KF_FILES) {
        return -1;
    }
    f = &g_files[node->id - 1u];
    if (offset >= f->len) {
        return 0;
    }
    if (len > f->len - offset) {
        len = (uint32_t)(f->len - offset);
    }
    memcpy(buf, f->data + offset, len);
    return (long)len;
}

static long kf_fs_write_file(void *fs, const char *path, const void *buf, uint32_t len) {
    (void)fs;
    kf_fs_add(path, buf, len, 0);
    return (long)len;
}

/* Entries of `path` are the files whose path is `path` + "/" + a name with no
 * further slash - enough for one level of directories. */
static int kf_fs_list(void *fs, const char *path, uint32_t index, char *name,
                      uint32_t name_cap, uint64_t *out_size, int *out_is_dir) {
    uint32_t i, seen = 0, plen;
    const char *p = path[0] == '/' ? path + 1 : path;
    (void)fs;

    plen = (uint32_t)strlen(p);
    for (i = 0; i < KF_FILES; i++) {
        const char *q = g_files[i].path[0] == '/' ? g_files[i].path + 1 : g_files[i].path;
        const char *rest;
        if (!g_files[i].used) {
            continue;
        }
        if (plen && (strncmp(q, p, plen) != 0 || q[plen] != '/')) {
            continue;
        }
        rest = plen ? q + plen + 1 : q;
        if (!*rest || strchr(rest, '/')) {
            continue;
        }
        if (seen++ == index) {
            strncpy(name, rest, name_cap - 1u);
            name[name_cap - 1u] = 0;
            *out_size = g_files[i].len;
            *out_is_dir = g_files[i].is_dir;
            return 0;
        }
    }
    return -1;
}

static int kf_fs_unlink(void *fs, const char *path) {
    int i = kf_find(path);
    (void)fs;
    if (i < 0) {
        return -1;
    }
    g_files[i].used = 0;
    return 0;
}

static int kf_fs_mkdir(void *fs, const char *path) {
    (void)fs;
    kf_fs_add(path, 0, 0, 1);
    return 0;
}

static int kf_fs_create(void *fs, const char *path, uint32_t mode, vibeos_fs_node_t *out);
static long kf_fs_write_at(void *fs, const vibeos_fs_node_t *node, uint64_t offset,
                           const void *buf, uint32_t len);
static int kf_fs_truncate(void *fs, const vibeos_fs_node_t *node, uint64_t size);

static const vibeos_fs_ops_t g_kf_fs_ops = {
    .lookup = kf_fs_lookup,
    .read_at = kf_fs_read_at,
    .write_file = kf_fs_write_file,
    .list = kf_fs_list,
    .unlink = kf_fs_unlink,
    .mkdir = kf_fs_mkdir,
    .write_at = kf_fs_write_at,
    .truncate = kf_fs_truncate,
    .create = kf_fs_create,
};

void kf_fs_add(const char *path, const void *data, uint32_t len, int is_dir) {
    int i = kf_find(path);
    uint32_t k;

    if (i < 0) {
        for (k = 0; k < KF_FILES; k++) {
            if (!g_files[k].used) {
                i = (int)k;
                break;
            }
        }
    }
    if (i < 0) {
        return;
    }
    memset(&g_files[i], 0, sizeof(g_files[i]));
    strncpy(g_files[i].path, path, sizeof(g_files[i].path) - 1u);
    if (len > sizeof(g_files[i].data)) {
        len = sizeof(g_files[i].data);
    }
    if (data && len) {
        memcpy(g_files[i].data, data, len);
    }
    g_files[i].len = len;
    g_files[i].is_dir = is_dir;
    g_files[i].used = 1;
}

/* The root writes in place, as every filesystem the kernel mounts to write
 * does since docs/abi/ L1 step 4. A file here is 512 bytes at most; past that
 * is the volume being full. */
static kf_file_t *kf_node_file(const vibeos_fs_node_t *node) {
    if (node->id == 0u || node->id > KF_FILES || !g_files[node->id - 1u].used) {
        return 0;
    }
    return &g_files[node->id - 1u];
}

static int kf_fs_create(void *fs, const char *path, uint32_t mode, vibeos_fs_node_t *out) {
    int i;
    (void)fs;
    (void)mode;
    if (kf_find(path) >= 0) {
        return -VIBEOS_EEXIST;
    }
    kf_fs_add(path, 0, 0, 0);
    i = kf_find(path);
    if (i < 0) {
        return -VIBEOS_ENOSPC;
    }
    out->id = (uint64_t)i + 1u;
    out->size = 0;
    out->is_dir = 0;
    return 0;
}

static long kf_fs_write_at(void *fs, const vibeos_fs_node_t *node, uint64_t offset,
                           const void *buf, uint32_t len) {
    kf_file_t *f = kf_node_file(node);
    (void)fs;
    if (!f) {
        return -VIBEOS_ENOENT;
    }
    if (len == 0u) {
        return 0;
    }
    if (offset >= sizeof(f->data)) {
        return -VIBEOS_ENOSPC;
    }
    if (len > sizeof(f->data) - offset) {
        len = (uint32_t)(sizeof(f->data) - offset);
    }
    if (offset > f->len) {
        memset(f->data + f->len, 0, (size_t)(offset - f->len));
    }
    memcpy(f->data + offset, buf, len);
    if (offset + len > f->len) {
        f->len = (uint32_t)(offset + len);
    }
    return (long)len;
}

static int kf_fs_truncate(void *fs, const vibeos_fs_node_t *node, uint64_t size) {
    kf_file_t *f = kf_node_file(node);
    (void)fs;
    if (!f) {
        return -VIBEOS_ENOENT;
    }
    if (size > sizeof(f->data)) {
        return -VIBEOS_EFBIG;
    }
    if (size > f->len) {
        memset(f->data + f->len, 0, (size_t)(size - f->len));
    }
    f->len = (uint32_t)size;
    return 0;
}

/* ---- the network ------------------------------------------------------------------- */

static vibeos_inet_t g_net_v;
static int g_net_is_up;
static uint32_t g_tx, g_udp_tx, g_udp_last_len;
static const uint8_t g_mac[6] = {0x52, 0x54, 0, 0x12, 0x34, 0x56};
static const uint8_t g_peer_mac[6] = {0x52, 0x54, 0, 0xAA, 0xBB, 0xCC};

static int kf_tx(void *ctx, const void *frame, uint32_t len) {
    const uint8_t *f = (const uint8_t *)frame;
    (void)ctx;
    g_tx++;
    if (len >= 14u + 20u + 8u && f[12] == 0x08 && f[13] == 0x00 && f[14 + 9] == 17u) {
        uint32_t ihl = (uint32_t)(f[14] & 0x0Fu) * 4u;
        const uint8_t *u = f + 14 + ihl;
        g_udp_tx++;
        g_udp_last_len = (((uint32_t)u[4] << 8) | u[5]) - 8u;
    }
    return 0;
}

void kf_net_up(uint32_t ip, uint32_t peer_ip) {
    memset(&g_net_v, 0, sizeof(g_net_v));
    (void)vibeos_inet_init(&g_net_v, g_mac, kf_tx, 0);
    vibeos_inet_set_addr(&g_net_v, ip, 0xFFFFFF00u, peer_ip, peer_ip);
    g_net_v.arp[0].ip = peer_ip;
    memcpy(g_net_v.arp[0].mac, g_peer_mac, 6);
    g_net_v.arp[0].valid = 1;
    g_net_v.arp[0].expires_ms = ~0ull;
    g_net_is_up = 1;
    g_tx = g_udp_tx = g_udp_last_len = 0;
}

static void kf_wr16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void kf_wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

/* An IPv4 datagram with a correct header checksum and no UDP checksum, which
 * IPv4 permits. */
void kf_net_deliver_udp(uint32_t src_ip, uint16_t sport, uint16_t dport,
                        const void *payload, uint32_t len) {
    uint8_t f[14 + 20 + 8 + 256];
    uint32_t sum = 0, i;

    if (len > 256u) {
        return;
    }
    memset(f, 0, sizeof(f));
    memcpy(f, g_mac, 6);
    memcpy(f + 6, g_peer_mac, 6);
    kf_wr16(f + 12, 0x0800);
    f[14] = 0x45;
    kf_wr16(f + 16, 20u + 8u + len);
    f[22] = 64;
    f[23] = 17;
    kf_wr32(f + 26, src_ip);
    kf_wr32(f + 30, g_net_v.ip);
    for (i = 0; i < 20u; i += 2u) {
        sum += ((uint32_t)f[14 + i] << 8) | f[15 + i];
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFFu) + (sum >> 16);
    }
    kf_wr16(f + 24, (uint16_t)~sum);
    kf_wr16(f + 34, sport);
    kf_wr16(f + 36, dport);
    kf_wr16(f + 38, 8u + len);
    memcpy(f + 42, payload, len);
    (void)vibeos_inet_input(&g_net_v, f, 42u + len);
}

uint32_t kf_net_tx_count(void) { return g_tx; }
uint32_t kf_net_udp_sent(uint32_t *last_payload_len) {
    if (last_payload_len) {
        *last_payload_len = g_udp_last_len;
    }
    return g_udp_tx;
}

/* ---- setup ------------------------------------------------------------------------ */

static void kf_procstate_init(vibeos_procstate_t *ps) {
    memset(ps, 0, sizeof(*ps));
    vibeos_fdtable_init(&ps->files);
    ps->cwd[0] = '/';
    ps->root[0] = '/';
    ps->umask = 022u;
    ps->refs = 1u;
    ps->files_users = 1u;
    ps->brk_cur = 0x10000000ull;
}

static void kf_pipe_lock(void) {}
static void kf_pipe_unlock(void) {}

static void *kf_fd_page(void) { return malloc(4096); }
static void kf_fd_page_free(void *p) { free(p); }

/* /tmp is tmpfs here as in the kernel (docs/abi/ L1): the root above stores
 * whole files, as FAT does, so the handlers' real write path - at an offset,
 * with modes and links - needs the filesystem that has one. */
static vibeos_tmpfs_t g_tmpfs;
static vibeos_fsmount_t g_tmpfs_mnt;
static int g_tmpfs_live;

void kf_reset(void) {
    uint32_t i;

    memset(g_t, 0, sizeof(g_t));
    /* The previous test's tables hold pages and descriptions: release them before
     * forgetting the process states, or every reset leaks what they held. */
    vibeos_fdtable_set_pages(kf_fd_page, kf_fd_page_free);
    for (i = 0; i < KF_PROCS; i++) {
        vibeos_fdtable_destroy(&g_ps[i].files);
    }
    vibeos_file_reset();
    vibeos_flk_set_lock(kf_pipe_lock, kf_pipe_unlock);
    vibeos_flk_reset();
    vibeos_pipe_reset();
    memset(g_ps, 0, sizeof(g_ps));
    memset(g_user, 0, sizeof(g_user));
    memset(g_files, 0, sizeof(g_files));
    (void)vibeos_task_table_init(KF_SLOTS);
    for (i = 0; i < KF_SLOTS; i++) {
        vibeos_task_identity_reset(&g_t[i].id);
    }
    g_cur = -1;
    g_next_pid_v = 100;
    g_ticks_v = 0;
    g_illegal = 0;
    g_user_used = 0;
    g_fault_base = g_fault_len = 0;
    g_con_len = 0;
    g_con[0] = 0;
    g_lock_depth = g_lock_bad = 0;
    g_panic_v = 0;
    g_exit_code_v = 0;
    g_net_is_up = 0;
    g_fg_pgid = 0;
    vibeos_pipe_set_lock(kf_pipe_lock, kf_pipe_unlock);
    vibeos_fs_unmount(&g_root);
    (void)vibeos_fs_mount(&g_root, &g_kf_fs_ops, 0, "kf");
    /* Paths go through the mount table since docs/abi/ A4, as in the kernel. */
    vibeos_fs_set_lock(kf_pipe_lock, kf_pipe_unlock);
    vibeos_fs_detach_all();
    (void)vibeos_fs_attach("/", &g_root);
    if (g_tmpfs_live) {
        vibeos_tmpfs_destroy(&g_tmpfs);
    }
    (void)vibeos_tmpfs_init(&g_tmpfs, 4096, kf_fd_page, kf_fd_page_free, kf_pipe_lock,
                            kf_pipe_unlock);
    g_tmpfs_live = 1;
    (void)vibeos_fs_mount(&g_tmpfs_mnt, vibeos_tmpfs_ops(), &g_tmpfs, "tmpfs");
    (void)vibeos_fs_attach("/tmp", &g_tmpfs_mnt);
    /* The personality registers its own tables: see the tests' fresh(). */
}

int kf_spawn(uint32_t pid, uint32_t sid) {
    uint32_t i, p;

    for (i = 0; i < KF_SLOTS; i++) {
        if (vibeos_task_state(i) == VIBEOS_TASK_FREE) {
            break;
        }
    }
    for (p = 0; p < KF_PROCS; p++) {
        if (g_ps[p].refs == 0u) {
            break;
        }
    }
    if (i == KF_SLOTS || p == KF_PROCS) {
        return -1;
    }
    (void)vibeos_task_transition(i, VIBEOS_TASK_SETUP, "kf_spawn");
    (void)vibeos_task_transition(i, VIBEOS_TASK_READY, "kf_spawn");
    vibeos_task_identity_reset(&g_t[i].id);
    g_t[i].id.pid = pid;
    g_t[i].id.tgid = pid;
    g_t[i].id.ppid = 1;
    g_t[i].id.pgid = pid;
    g_t[i].id.sid = sid;
    g_t[i].id.is_user = 1;
    kf_procstate_init(&g_ps[p]);
    (void)vibeos_files_std_console(&g_ps[p].files);
    g_t[i].ps = &g_ps[p];
    g_t[i].seq++;
    return (int)i;
}

void kf_set_current(int slot) {
    if (g_cur >= 0 && vibeos_task_state((uint32_t)g_cur) == VIBEOS_TASK_RUNNING) {
        (void)vibeos_task_transition((uint32_t)g_cur, VIBEOS_TASK_READY, "kf_switch");
    }
    g_cur = slot;
    if (slot >= 0 && vibeos_task_state((uint32_t)slot) == VIBEOS_TASK_READY) {
        (void)vibeos_task_transition((uint32_t)slot, VIBEOS_TASK_RUNNING, "kf_switch");
    }
}

uint64_t kf_ualloc(uint64_t len) {
    uint64_t at = (g_user_used + 15u) & ~15ull;
    if (at + len > KF_USER_BYTES) {
        return 0;
    }
    g_user_used = at + len;
    return (uint64_t)(uintptr_t)&g_user[at];
}

void *kf_uptr(uint64_t uaddr) { return (void *)(uintptr_t)uaddr; }

void kf_next_sp(uint64_t sp) { g_next_sp_v = sp; }

void kf_fault(uint64_t uaddr, uint64_t len) {
    g_fault_base = uaddr;
    g_fault_len = len;
}

long kf_call(kf_entry_t entry, uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2,
             uint64_t a3, uint64_t a4, uint64_t a5, kf_outcome_t *out) {
    static struct ks_regs frame;
    uint64_t a[6];
    volatile long r = 0;
    int how;

    a[0] = a0; a[1] = a1; a[2] = a2; a[3] = a3; a[4] = a4; a[5] = a5;
    memset(&frame, 0, sizeof(frame));
    frame.ret = nr;
    frame.sp = g_next_sp_v;
    g_next_sp_v = 0;
    g_lock_depth = 0;
    g_idles = 0;
    g_in_call = 1;
    how = setjmp(g_escape);
    if (how == 0) {
        r = entry(&frame, nr, a);
    }
    g_in_call = 0;
    if (out) {
        *out = (kf_outcome_t)how;
    }
    /* A call that escaped left its task wherever it was; put it back on the
     * core so the next call starts from a running task. */
    if (g_cur >= 0) {
        vibeos_task_state_t st = vibeos_task_state((uint32_t)g_cur);
        if (st == VIBEOS_TASK_BLOCKED) {
            (void)vibeos_task_transition((uint32_t)g_cur, VIBEOS_TASK_READY, "kf_call");
            st = VIBEOS_TASK_READY;
        }
        if (st == VIBEOS_TASK_READY) {
            (void)vibeos_task_transition((uint32_t)g_cur, VIBEOS_TASK_RUNNING, "kf_call");
        }
    }
    if (g_lock_depth != 0) {
        g_lock_bad++;
    }
    return r;
}

static void kf_escape(kf_outcome_t how) {
    if (g_in_call) {
        longjmp(g_escape, (int)how);
    }
    fprintf(stderr, "ksvc_fake: escape outside a call (%d)\n", (int)how);
}

const char *kf_console(void) { return g_con; }
int kf_lock_imbalance(void) { return g_lock_bad; }
uint64_t kf_exit_code(void) { return g_exit_code_v; }
const char *kf_panic_reason(void) { return g_panic_v; }
uint32_t kf_illegal_transitions(void) { return g_illegal; }

/* ---- the services ---------------------------------------------------------------------- */

int ks_current(void) { return g_cur; }
uint32_t ks_slots(void) { return KF_SLOTS; }
vibeos_task_t *ks_id(int slot) { return &g_t[slot].id; }
vibeos_procstate_t *ks_ps(int slot) { return g_t[slot].ps; }
void ks_set_ps(int slot, vibeos_procstate_t *ps) { g_t[slot].ps = ps; }
vibeos_image_t *ks_image(int slot) { return &g_t[slot].img; }
uint32_t ks_seq(int slot) { return g_t[slot].seq; }
const vibeos_abi_t *ks_abi(int slot) { (void)slot; return vibeos_abi_linux(); }
uint64_t ks_cr3(int slot) { (void)slot; return 0; }
void ks_mark_ready(int slot, const char *where) { g_t[slot].ready_by = where; }
void ks_mark_aspace(int slot, const char *where) { (void)slot; (void)where; }

vibeos_lock_t *ks_sched_lock(void) { return &g_sched_lock_v; }
int ks_set_state(int slot, vibeos_task_state_t to, const char *why) {
    if (vibeos_task_transition((uint32_t)slot, to, why) != 0) {
        g_illegal++;
        return -1;
    }
    return 0;
}

int ks_task_by_pid(uint32_t pid) {
    uint32_t i;
    for (i = 0; i < KF_SLOTS; i++) {
        vibeos_task_state_t st = vibeos_task_state(i);
        if (st != VIBEOS_TASK_FREE && st != VIBEOS_TASK_SETUP && g_t[i].id.pid == pid &&
            g_t[i].id.tgid == pid) {
            return (int)i;
        }
    }
    return -1;
}

int ks_task_by_tid(uint32_t tid) {
    uint32_t i;
    for (i = 0; i < KF_SLOTS; i++) {
        vibeos_task_state_t st = vibeos_task_state(i);
        if (st != VIBEOS_TASK_FREE && st != VIBEOS_TASK_SETUP && g_t[i].id.pid == tid) {
            return (int)i;
        }
    }
    return -1;
}

int ks_task_alloc_for_user(const char *what) {
    uint32_t i;
    (void)what;
    for (i = 0; i < KF_SLOTS; i++) {
        if (vibeos_task_state(i) == VIBEOS_TASK_FREE) {
            (void)vibeos_task_transition(i, VIBEOS_TASK_SETUP, "kf_alloc");
            vibeos_task_identity_reset(&g_t[i].id);
            g_t[i].ps = 0;
            g_t[i].seq++;
            return (int)i;
        }
    }
    return -1;
}

void ks_task_release(int slot) { (void)vibeos_task_transition((uint32_t)slot, VIBEOS_TASK_FREE, "kf_release"); }
uint32_t ks_next_pid(void) { return g_next_pid_v++; }
void ks_task_exit(uint64_t code) { g_exit_code_v = code; kf_escape(KF_EXITED); }
void ks_task_exit_group(uint64_t code) { g_exit_code_v = code; kf_escape(KF_EXITED); }

void ks_lock(vibeos_lock_t *l, const char *fn) { (void)fn; l->locked++; g_lock_depth++; }
void ks_unlock(vibeos_lock_t *l) {
    if (l->locked <= 0) {
        g_lock_bad++;
        return;
    }
    l->locked--;
    g_lock_depth--;
}
void ks_lock_preemptible(vibeos_lock_t *l) { ks_lock(l, "preemptible"); }
void ks_unlock_preemptible(vibeos_lock_t *l) { ks_unlock(l); }
void ks_irq_off(void) {}
void ks_irq_on(void) {}

/* A wait. The clock moves one tick, so a wait with a deadline reaches it; one
 * with no deadline and nobody to wake it is reported rather than spun forever. */
void ks_idle(void) {
    g_ticks_v++;
    if (++g_idles > 5000u) {
        kf_escape(KF_BLOCKED);
    }
}
void ks_block_point(void) { kf_escape(KF_BLOCKED); }
void ks_wake_waiters(void) {}
int ks_signal_interrupts(int slot) {
    return (g_t[slot].id.sig_pending & ~g_t[slot].id.sig_blocked) != 0u;
}
int ks_signal_raise(int slot, uint32_t sig) {
    if (sig == 0u || sig > VIBEOS_SIG_MAX) {
        return -1;
    }
    g_t[slot].id.sig_pending |= 1ull << sig;
    return 0;
}
int ks_signal_default_kills(uint32_t sig) {
    return !(sig == VIBEOS_SIGCHLD || sig == VIBEOS_SIGCONT || sig == VIBEOS_SIGWINCH);
}

uint64_t ks_ticks(void) { return g_ticks_v; }
uint32_t ks_hz(void) { return 100u; }

static int kf_in_user(uint64_t base, uint64_t len) {
    uint64_t lo = (uint64_t)(uintptr_t)g_user;
    return base >= lo && len <= KF_USER_BYTES && base - lo <= KF_USER_BYTES - len;
}

int ks_user_ok(uint64_t base, uint64_t len, int write) {
    (void)write;
    return kf_in_user(base, len == 0u ? 1u : len);
}
int ks_user_addr_ok(uint64_t va) { return va == 0u ? 0 : kf_in_user(va, 1u); }
int ks_user_range_why(uint64_t va, uint64_t len, int write, uint32_t *why) {
    int ok = ks_user_ok(va, len, write);
    *why = ok ? 0u : 3u;
    return ok;
}
const char *ks_user_range_why_name(uint32_t why) { return why ? "outside_user_arena" : "ok"; }

static int kf_faults(const void *p, uint64_t n) {
    uint64_t a = (uint64_t)(uintptr_t)p;
    return g_fault_len && a < g_fault_base + g_fault_len && a + n > g_fault_base;
}

int vibeos_uaccess_copy(void *dst, const void *src, uint64_t len) {
    if (kf_faults(dst, len) || kf_faults(src, len)) {
        return -1;
    }
    memmove(dst, src, (size_t)len);
    return 0;
}

int ks_copy_user_string(uint64_t uptr, char *dst, int max) {
    int i;
    for (i = 0; i < max - 1; i++) {
        if (!ks_user_ok(uptr + (uint64_t)i, 1, 0) ||
            vibeos_uaccess_copy(&dst[i], (const void *)(uintptr_t)(uptr + (uint64_t)i), 1u) != 0) {
            return -1;
        }
        if (dst[i] == 0) {
            return 0;
        }
    }
    dst[max - 1] = 0;
    return 0;
}

void ks_mm_lock(vibeos_procstate_t *ps) { ps->mm_busy = 1u; }
void ks_mm_unlock(vibeos_procstate_t *ps) { ps->mm_busy = 0u; }
vibeos_vmspace_t ks_vm(int slot) {
    vibeos_vmspace_t v;
    (void)slot;
    memset(&v, 0, sizeof(v));
    return v;
}
/* No page tables here: a handler that reaches these is a test that needs the
 * memory manager's own harness (vmspace_tests.c), and says so by failing. */
int ks_map_anon(int slot, uint64_t va, vibeos_prot_t prot) { (void)slot; (void)va; (void)prot; return -1; }
int ks_map_user_pages(int slot, uint64_t va, uint64_t pages) { (void)slot; (void)va; (void)pages; return -1; }
void ks_tlb_drain(void) {}
void ks_tlb_flush_page(uint64_t va) { (void)va; }
void ks_pageinfo(int slot, uint64_t va, vibeos_pageinfo_t *out) {
    (void)slot; (void)va;
    memset(out, 0, sizeof(*out));
}
void *ks_page_alloc(void) { return malloc(4096); }
void ks_page_free(void *page, const char *why) { (void)why; free(page); }
uint64_t ks_heap_base(void) { return 0x10000000ull; }
uint64_t ks_mmap_base(void) { return 0x20000000ull; }
uint64_t ks_stack_bytes(void) { return 16384ull; }

vibeos_procstate_t *ks_procstate_new(void) {
    uint32_t p;
    for (p = 0; p < KF_PROCS; p++) {
        if (g_ps[p].refs == 0u) {
            kf_procstate_init(&g_ps[p]);
            g_ps[p].files_users = 1u;
            return &g_ps[p];
        }
    }
    return 0;
}
void ks_procstate_put(vibeos_procstate_t *ps) {
    if (ps && ps->refs) {
        /* As the architecture's: the last reference closes a table nobody left. */
        if (ps->refs == 1u) {
            vibeos_fdtable_destroy(&ps->files);
        }
        ps->refs--;
    }
}
int ks_fork_aspace(int child, int parent) { (void)child; (void)parent; return 0; }
void ks_drop_aspace(int slot) { (void)slot; }
int ks_alloc_kstack(int slot) { (void)slot; return 0; }
void ks_fork_regs(int child, int parent, const ks_regs_t *frame) {
    (void)frame;
    g_t[child].img = g_t[parent].img;
    g_t[child].tls = g_t[parent].tls;
}
void ks_thread_regs(int child, int parent, const ks_regs_t *frame, uint64_t stack, uint64_t tls) {
    (void)frame; (void)stack;
    g_t[child].img = g_t[parent].img;
    g_t[child].tls = tls;
}

static uint8_t g_exec_buf[4096];
uint8_t *ks_exec_buffer(uint32_t *cap) { *cap = sizeof(g_exec_buf); return g_exec_buf; }
long ks_read_file_cached(const char *path, void *buf, uint32_t cap, uint32_t *out_id) {
    vibeos_fs_node_t n;
    *out_id = 0;
    if (kf_fs_lookup(0, path, &n) != 0) {
        return -1;
    }
    return kf_fs_read_at(0, &n, 0, buf, cap);
}
int ks_exec_refuse(vibeos_exec_fail_t why, const char *path, const char *detail) {
    (void)why; (void)path; (void)detail;
    return -VIBEOS_ENOENT;
}
int ks_image_create(vibeos_image_t *img, vibeos_procstate_t *ps,
                    const unsigned char *elf, uint64_t len, uint64_t staged,
                    const char *const *argv, const char *const *envp,
                    const char *path, uint32_t file_id) {
    (void)img; (void)ps; (void)elf; (void)len; (void)staged; (void)argv; (void)envp;
    (void)path; (void)file_id;
    return -1;   /* no loader here: execve is tested up to the image */
}
void ks_image_drop(vibeos_image_t *img, const char *why) { (void)img; (void)why; }
void ks_exec_switch(int slot, const vibeos_image_t *img) { g_t[slot].img = *img; }
void ks_exec_regs(int slot, ks_regs_t *frame, uint64_t entry, uint64_t sp) {
    g_t[slot].tls = 0;
    memset(frame, 0, sizeof(*frame));
    frame->ip = entry;
    frame->sp = sp;
}

uint64_t ks_regs_sp(const ks_regs_t *frame) { return frame->sp; }
uint64_t ks_regs_ret(const ks_regs_t *frame) { return frame->ret; }

typedef struct {
    uint64_t magic, blocked;
    struct ks_regs regs;
} kf_sigframe_t;

uint64_t ks_sigframe_size(void) { return sizeof(kf_sigframe_t); }
int ks_sigframe_push(const ks_regs_t *frame, uint64_t sp, uint64_t blocked, uint64_t restorer) {
    kf_sigframe_t kf;
    kf.magic = 0x4B46534947ull;
    kf.blocked = blocked;
    kf.regs = *frame;
    if (vibeos_uaccess_copy((void *)(uintptr_t)(sp + 8u), &kf, sizeof(kf)) != 0 ||
        vibeos_uaccess_copy((void *)(uintptr_t)sp, &restorer, 8u) != 0) {
        return -1;
    }
    return 0;
}
int ks_sigframe_pop(ks_regs_t *frame, uint64_t base, uint64_t *blocked) {
    kf_sigframe_t kf;
    if (vibeos_uaccess_copy(&kf, (const void *)(uintptr_t)base, sizeof(kf)) != 0 ||
        kf.magic != 0x4B46534947ull) {
        return -1;
    }
    *blocked = kf.blocked;
    *frame = kf.regs;
    return 0;
}
void ks_regs_enter_handler(ks_regs_t *frame, uint64_t handler, uint64_t sp, uint32_t sig) {
    frame->ip = handler;
    frame->sp = sp;
    frame->arg0 = sig;
    frame->ret = 0;
}
uint64_t ks_tls_get(int slot) { return g_t[slot].tls; }
void ks_tls_set(int slot, uint64_t base) { g_t[slot].tls = base; }

vibeos_inet_t *ks_net(void) { return g_net_is_up ? &g_net_v : 0; }
vibeos_lock_t *ks_net_lock(void) { return &g_net_lock_v; }
int ks_console_getc(void) { return -1; }
void ks_console_echo(char c) { (void)c; }
uint32_t ks_foreground_pgid(void) { return g_fg_pgid; }
void ks_set_foreground_pgid(uint32_t pgid) { g_fg_pgid = pgid; }

void ks_con_lock(void) {}
void ks_con_unlock(void) {}
void ks_con_puts(const char *s) {
    while (*s && g_con_len + 1u < sizeof(g_con)) {
        g_con[g_con_len++] = *s++;
    }
    g_con[g_con_len] = 0;
}
void ks_con_putc(char c) {
    char s[2];
    s[0] = c;
    s[1] = 0;
    ks_con_puts(s);
}
void ks_con_hex(uint64_t v) {
    char s[17];
    int i;
    for (i = 15; i >= 0; i--) {
        s[i] = "0123456789abcdef"[v & 0xFu];
        v >>= 4;
    }
    s[16] = 0;
    ks_con_puts(s);
}
void ks_log(vibeos_log_level_t level, uint32_t code, uint64_t a0, uint64_t a1, const char *msg) {
    (void)level; (void)code; (void)a0; (void)a1; (void)msg;
}
void ks_panic(const char *why) { g_panic_v = why; kf_escape(KF_PANICKED); }
uint32_t ks_cpu_id(void) { return 0; }
uint64_t ks_cr3_now(void) { return 0; }

void ks_con_cow_faults(uint64_t page) { (void)page; }
