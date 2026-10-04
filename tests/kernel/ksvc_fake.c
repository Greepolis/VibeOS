/* The host tests' kernel services: see ksvc_fake.h. */

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vibeos/ptimer.h"
#include "vibeos/linux_exports.h"
#include "vibeos/account.h"
#include "vibeos/mbz.h"
#include "ksvc_fake.h"
#include "vibeos/filelock.h"
#include "vibeos/tty.h"
#include "vibeos/frame.h"
#include "vibeos/rmap.h"
#include "vibeos/vmspace.h"
#include "vibeos/vma.h"
#include "vibeos/mm_stats.h"
#include "vibeos/tmpfs.h"
#include "vibeos/procfs.h"
#include "vibeos/fdtable.h"
#include "vibeos/file.h"
#include "vibeos/fileops.h"
#include "vibeos/pipe.h"
/* Signal numbers and errno values: the kernel numbers signals as Linux does. */
#include "vibeos/abi_linux.h"

/* ---- state ------------------------------------------------------------------ */

typedef struct {
    vibeos_task_t id;
    vibeos_siginfo_t info[VIBEOS_NSIG];
    vibeos_procstate_t *ps;
    vibeos_image_t img;
    uint32_t seq;
    uint64_t tls;
    const char *ready_by;
    vibeos_vmspace_t as;          /* its address space (docs/abi/ L3) */
    int has_as;
} kf_task_t;

#define KF_PROCS 16u

static kf_task_t g_t[KF_SLOTS];
static vibeos_procstate_t g_ps[KF_PROCS];
static int g_cur = -1;
static uint32_t g_next_pid_v = 100;
static uint64_t g_ticks_v;
static int g_account_ready;   /* the accounting table, set up by the first kf_cpu */
static int g_nice[KF_SLOTS];  /* the scheduler's nice per slot, as the machine's policy keeps it */
static uint32_t g_illegal;

static uint8_t g_user[KF_USER_BYTES] __attribute__((aligned(4096)));
static uint64_t g_user_used;
static uint64_t g_fault_base, g_fault_len;

/* ---- an address space (docs/abi/ L3) ------------------------------------------------
 *
 * The memory handlers - mmap, munmap, mprotect, brk - could not run here: this
 * file had no page tables, and said so by failing every mapping. It has them
 * now, the real ones: kernel/mm's frame layer and address-space layer over a
 * few megabytes of static memory, exactly as their own host tests set them up.
 *
 * A mapped address is not a host pointer. Everything a handler copies to or from
 * user memory goes through vibeos_uaccess_copy, which finds the page through the
 * current task's tables as the hardware would - and takes the copy-on-write
 * fault a kernel-mode store takes. Addresses in the window below are looked up
 * that way; anything else is the flat arena kf_ualloc hands out, as before. */
#define KF_MM_LO 0x10000000ull            /* ks_heap_base                     */
#define KF_MM_HI 0x100000000ull           /* four gigabytes: no host pointer  */
#define KF_PHYS_BASE 0x40000000ull
#define KF_FRAMES 4096u
#define KF_PTE_PRESENT 1ull
#define KF_PTE_WRITE 2ull
#define KF_PTE_USER 4ull
#define KF_PTE_COW (1ull << 9)
#define KF_PTE_ADDR 0x000FFFFFFFFFF000ull

static uint8_t g_kf_ram[KF_FRAMES * 4096u] __attribute__((aligned(4096)));
static vibeos_frame_t g_kf_ftable[KF_FRAMES];
static unsigned char g_kf_rpool[KF_FRAMES * sizeof(uint32_t) + 4096u];
static vibeos_vma_t g_kf_vmas[512];

static void *kf_phys(uint64_t phys) {
    if (phys < KF_PHYS_BASE || phys >= KF_PHYS_BASE + (uint64_t)KF_FRAMES * 4096ull) {
        return 0;
    }
    return g_kf_ram + (phys - KF_PHYS_BASE);
}

static uint64_t kf_alloc_table(void) { return vibeos_frame_alloc(VIBEOS_FRAME_PAGE_TABLE); }
static void kf_free_table(uint64_t phys) { (void)vibeos_frame_put(phys); }

static void kf_mm_reset(void) {
    vibeos_vmspace_backend_t be;

    memset(g_kf_ftable, 0, sizeof(g_kf_ftable));
    memset(g_kf_rpool, 0, sizeof(g_kf_rpool));
    vibeos_mm_stats_reset();
    (void)vibeos_frame_init(KF_PHYS_BASE, (uint64_t)KF_FRAMES * 4096ull, g_kf_ftable, KF_FRAMES,
                            kf_phys);
    vibeos_rmap_set_base(KF_PHYS_BASE);
    (void)vibeos_rmap_init(g_kf_rpool, (uint64_t)sizeof(g_kf_rpool), KF_FRAMES);
    memset(&be, 0, sizeof(be));
    be.map_phys = kf_phys;
    be.alloc_table = kf_alloc_table;
    be.free_table = kf_free_table;
    (void)vibeos_vmspace_init(&be);
    vibeos_vma_pool_init(g_kf_vmas, (uint32_t)(sizeof(g_kf_vmas) / sizeof(g_kf_vmas[0])));
}

static int kf_mm_va(uint64_t va) { return va >= KF_MM_LO && va < KF_MM_HI; }

/* The host memory behind one mapped address of the current task, or null. A
 * store to a page that is read-only because it is shared takes the fault the
 * kernel's own store would take.
 *
 * `ring3` says who is touching it. The program reaches only a page mapped for
 * it; the kernel reaches any page that is mapped - the user bit is a limit on
 * ring 3, and ring 0 reads a PROT_NONE page without a fault. This used to
 * refuse the kernel too, which made vibeos_uaccess_copy a permission check it
 * is not: a handler that copied from a pointer nobody had judged passed here
 * and leaked on the machine (M-082). What ring 0 does not get is a store to a
 * read-only page, with CR0.WP set, as here. */
static uint8_t *kf_mm_ptr_as(uint64_t va, int write, int ring3) {
    uint64_t *e;
    uint8_t *page;

    if (g_cur < 0 || !g_t[g_cur].has_as) {
        return 0;
    }
    e = vibeos_vmspace_entry(&g_t[g_cur].as, va);
    if (!e || !(*e & KF_PTE_PRESENT) || (ring3 && !(*e & KF_PTE_USER))) {
        return 0;
    }
    if (write && !(*e & KF_PTE_WRITE)) {
        if (vibeos_vmspace_fault(&g_t[g_cur].as, va, 1) != 1) {
            return 0;
        }
        e = vibeos_vmspace_entry(&g_t[g_cur].as, va);
        if (!e || !(*e & KF_PTE_WRITE)) {
            return 0;
        }
    }
    page = (uint8_t *)kf_phys(*e & KF_PTE_ADDR);
    return page ? page + (va & 0xFFFull) : 0;
}

static int g_copy_ring3;   /* set while kf_peek and kf_poke copy: the program's access */
static uint8_t *kf_mm_ptr(uint64_t va, int write) {
    return kf_mm_ptr_as(va, write, g_copy_ring3);
}

static jmp_buf g_escape;
static int g_in_call;
static uint64_t g_exit_code_v;
static const char *g_panic_v;
static uint32_t g_idles;
static uint64_t g_next_sp_v;

static char g_con[16384];
/* The keyboard: what a test typed and nobody has read yet. */
static char g_kbd[1024];
static uint32_t g_kbd_len, g_kbd_at;
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
    linux_procstate_defaults(ps);
    ps->refs = 1u;
    ps->files_users = 1u;
    ps->brk_cur = 0x10000000ull;
    ps->mmap_cur = 0x20000000ull;
}

static void kf_pipe_lock(void) {}
static void kf_pipe_unlock(void) {}

static void *kf_fd_page(void) { return malloc(4096); }
static void kf_fd_page_free(void *p) { free(p); }

/* /tmp's pages are frames, as the kernel's are: a shared mapping of a file maps
 * the file's own page (docs/abi/ L3), so the page has to be something the
 * address-space layer can map and the frame layer can count. */
static uint64_t kf_phys_of(void *page) {
    return KF_PHYS_BASE + (uint64_t)((uint8_t *)page - g_kf_ram);
}
static void *kf_tmpfs_page(void) {
    uint64_t phys = vibeos_frame_alloc(VIBEOS_FRAME_ALLOCATED);
    return phys ? kf_phys(phys) : 0;
}
static void kf_tmpfs_page_free(void *p) { (void)vibeos_frame_put(kf_phys_of(p)); }
static void kf_tmpfs_page_hold(void *p) { vibeos_frame_get(kf_phys_of(p)); }

/* /tmp is tmpfs here as in the kernel (docs/abi/ L1): the root above stores
 * whole files, as FAT does, so the handlers' real write path - at an offset,
 * with modes and links - needs the filesystem that has one. */
static vibeos_tmpfs_t g_tmpfs;
static vibeos_fsmount_t g_tmpfs_mnt;
static vibeos_fsmount_t g_procfs_mnt;
static vibeos_procfs_t g_procfs;

static void kf_procfs_mem(vibeos_procfs_mem_t *out) {
    out->total_kb = vibeos_frame_total() * 4ull;
    out->free_kb = vibeos_frame_free_count() * 4ull;
    out->available_kb = out->free_kb;
    out->cached_kb = 0;
    out->swap_total_kb = 0;
    out->swap_free_kb = 0;
}
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
    /* Before the frames are forgotten: its pages are frames, and giving them
     * back to a table that has been wiped would be giving them to nobody. */
    if (g_tmpfs_live) {
        vibeos_tmpfs_destroy(&g_tmpfs);
    }
    kf_mm_reset();
    vibeos_file_reset();
    vibeos_flk_set_lock(kf_pipe_lock, kf_pipe_unlock);
    vibeos_flk_reset();
    vibeos_ptimer_set_lock(kf_pipe_lock, kf_pipe_unlock);
    vibeos_ptimer_reset();
    memset(g_nice, 0, sizeof(g_nice));
    g_account_ready = 0;
    vibeos_tty_reset();
    g_kbd_len = g_kbd_at = 0;
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
    (void)vibeos_tmpfs_init(&g_tmpfs, 2048, kf_tmpfs_page, kf_tmpfs_page_free, kf_pipe_lock,
                            kf_pipe_unlock);
    vibeos_tmpfs_set_page_hold(&g_tmpfs, kf_tmpfs_page_hold);
    g_tmpfs_live = 1;
    kf_share_page(-1, 0, 0);
    (void)vibeos_fs_mount(&g_tmpfs_mnt, vibeos_tmpfs_ops(), &g_tmpfs, "tmpfs");
    (void)vibeos_fs_attach("/tmp", &g_tmpfs_mnt);
    /* /proc, as in the kernel (docs/abi/ L3 step 3). */
    g_procfs.mem = kf_procfs_mem;
    g_procfs.pid_max = 4194304u;
    vibeos_fs_unmount(&g_procfs_mnt);
    (void)vibeos_fs_mount(&g_procfs_mnt, vibeos_procfs_ops(), &g_procfs, "proc");
    (void)vibeos_fs_attach("/proc", &g_procfs_mnt);
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
    g_t[i].has_as = vibeos_vmspace_create(&g_t[i].as) == 0;
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

static struct ks_regs g_last_frame;
const struct ks_regs *kf_last_frame(void) { return &g_last_frame; }

long kf_call(kf_entry_t entry, uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2,
             uint64_t a3, uint64_t a4, uint64_t a5, kf_outcome_t *out) {
    uint64_t a[6];
    volatile long r = 0;
    int how;

    a[0] = a0; a[1] = a1; a[2] = a2; a[3] = a3; a[4] = a4; a[5] = a5;
    memset(&g_last_frame, 0, sizeof(g_last_frame));
    g_last_frame.ret = nr;
    g_last_frame.sp = g_next_sp_v;
    g_next_sp_v = 0;
    g_lock_depth = 0;
    g_idles = 0;
    g_in_call = 1;
    how = setjmp(g_escape);
    if (how == 0) {
        r = entry(&g_last_frame, nr, a);
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

/* A timer's expiry, as the machine's hw_ptimer_fire delivers it: to the
 * process's leader, or to the thread named, and not again while the last one
 * is pending. */

static int kf_ptimer_fire(const vibeos_ptimer_fire_t *f) {
    int i, slot = -1;
    vibeos_siginfo_t why;

    for (i = 0; i < KF_SLOTS; i++) {
        if (!g_t[i].id.is_user || g_t[i].id.tgid != f->tgid ||
            vibeos_task_state((uint32_t)i) == VIBEOS_TASK_FREE) {
            continue;
        }
        if (f->to_thread ? g_t[i].id.pid == f->tid : (slot < 0 || g_t[i].id.pid == f->tgid)) {
            slot = i;
        }
    }
    if (slot < 0) {
        vibeos_mbz_hit(VIBEOS_MBZ_PTIMER_ORPHAN, f->tgid);
        return 0;
    }
    if (f->id >= 0 && (g_t[slot].id.sig_pending & (1ull << f->signo)) &&
        g_t[slot].info[f->signo].from == VIBEOS_SIG_FROM_TIMER && g_t[slot].info[f->signo].code == f->id) {
        g_t[slot].info[f->signo].status += 1 + (int32_t)f->overrun;
        return 1 + g_t[slot].info[f->signo].status;
    }
    memset(&why, 0, sizeof(why));
    why.from = f->id < 0 ? VIBEOS_SIG_FROM_KERNEL : VIBEOS_SIG_FROM_TIMER;
    why.code = f->id;
    why.status = (int32_t)f->overrun;
    why.addr = f->value;
    (void)ks_signal_send(slot, f->signo, &why);
    return 0;
}

/* The current task runs for `ticks`, in user mode or in the kernel: the clock
 * moves, the due timers fire, and the task is charged the CPU time - what the
 * machine's tick does on a core that is running it. A wait (ks_idle) charges
 * nothing, as a core that has given its task away charges it nothing. */
void kf_cpu(uint32_t ticks, int user) {
    uint32_t i;

    if (!g_account_ready) {
        (void)vibeos_account_init(KF_SLOTS, 1u);
        g_account_ready = 1;
    }
    for (i = 0; i < ticks; i++) {
        g_ticks_v++;
        vibeos_ptimer_tick(g_ticks_v, kf_ptimer_fire);
        if (g_cur >= 0 && g_t[g_cur].id.is_user) {
            vibeos_account_tick(0u, g_cur, 0);
            vibeos_ptimer_charge(g_t[g_cur].id.tgid, g_t[g_cur].id.pid, user, kf_ptimer_fire);
        }
    }
}

/* A wait. The clock moves one tick, so a wait with a deadline reaches it; one
 * with no deadline and nobody to wake it is reported rather than spun forever.
 * The due timers fire, as the machine's clock owner fires them. */
void ks_idle(void) {
    g_ticks_v++;
    vibeos_ptimer_tick(g_ticks_v, kf_ptimer_fire);
    if (++g_idles > 5000u) {
        kf_escape(KF_BLOCKED);
    }
}
void ks_block_point(void) { kf_escape(KF_BLOCKED); }
void ks_wait_tick(void) { ks_idle(); }
void ks_wake_waiters(void) {}
int ks_signal_interrupts(int slot) {
    return (g_t[slot].id.sig_pending & ~g_t[slot].id.sig_blocked) != 0u;
}
int ks_signal_raise(int slot, uint32_t sig) {
    return ks_signal_send(slot, sig, 0);
}
/* The scheduler's nice, per slot, as the machine's policy keeps it. */
int ks_task_nice(int slot) { return g_nice[slot]; }
int ks_task_set_nice(int slot, int nice) {
    if (nice < -20 || nice > 19) {
        return -1;
    }
    g_nice[slot] = nice;
    return 0;
}
/* As the machine: a pending signal keeps its first reason, and one that is
 * ignored and not blocked is dropped. */
int ks_signal_send(int slot, uint32_t sig, const vibeos_siginfo_t *info) {
    /* As the machine: a task with no process state left is exiting, and
     * takes no signal. */
    if (sig == 0u || sig > VIBEOS_SIG_MAX || !g_t[slot].ps) {
        return -1;
    }
    if (sig != VIBEOS_SIGKILL && sig != VIBEOS_SIGSTOP && g_t[slot].ps &&
        g_t[slot].ps->sig_handler[sig] == SIG_IGN_ADDR &&
        (g_t[slot].id.sig_blocked & (1ull << sig)) == 0u) {
        return 0;
    }
    if ((g_t[slot].id.sig_pending & (1ull << sig)) == 0u) {
        memset(&g_t[slot].info[sig], 0, sizeof(g_t[slot].info[sig]));
        if (info) {
            g_t[slot].info[sig] = *info;
        }
        g_t[slot].id.sig_pending |= 1ull << sig;
    }
    return 0;
}
int ks_signal_take(int slot, uint32_t sig, vibeos_siginfo_t *out) {
    if (sig == 0u || sig > VIBEOS_SIG_MAX || (g_t[slot].id.sig_pending & (1ull << sig)) == 0u) {
        return 0;
    }
    if (out) {
        *out = g_t[slot].info[sig];
    }
    g_t[slot].id.sig_pending &= ~(1ull << sig);
    return 1;
}
const vibeos_siginfo_t *kf_siginfo(int slot, uint32_t sig) { return &g_t[slot].info[sig]; }
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
    if (kf_mm_va(base)) {
        /* A mapped range: every page present and reachable from ring 3, and
         * for a store writable or copy-on-write - which a store resolves. */
        uint64_t va, end = base + (len == 0u ? 1u : len);
        if (g_cur < 0 || !g_t[g_cur].has_as || end < base || end > KF_MM_HI) {
            return 0;
        }
        for (va = base & ~0xFFFull; va < end; va += 4096ull) {
            uint64_t *e = vibeos_vmspace_entry(&g_t[g_cur].as, va);
            if (!e || !(*e & KF_PTE_PRESENT) || !(*e & KF_PTE_USER) ||
                (write && !(*e & (KF_PTE_WRITE | KF_PTE_COW)))) {
                return 0;
            }
        }
        return 1;
    }
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
    uint64_t d = (uint64_t)(uintptr_t)dst, s = (uint64_t)(uintptr_t)src, done = 0;

    if (kf_faults(dst, len) || kf_faults(src, len)) {
        return -1;
    }
    if (!kf_mm_va(d) && !kf_mm_va(s)) {
        memmove(dst, src, (size_t)len);
        return 0;
    }
    /* One side is a mapped address: a page at a time, through the tables. */
    while (done < len) {
        uint64_t n = len - done, room;
        uint8_t *dp = kf_mm_va(d + done) ? kf_mm_ptr(d + done, 1) : (uint8_t *)(uintptr_t)(d + done);
        const uint8_t *sp = kf_mm_va(s + done) ? kf_mm_ptr(s + done, 0)
                                               : (const uint8_t *)(uintptr_t)(s + done);
        if (!dp || !sp) {
            return -1;
        }
        if (kf_mm_va(d + done) && (room = 4096ull - ((d + done) & 0xFFFull)) < n) {
            n = room;
        }
        if (kf_mm_va(s + done) && (room = 4096ull - ((s + done) & 0xFFFull)) < n) {
            n = room;
        }
        memmove(dp, sp, (size_t)n);
        done += n;
    }
    return 0;
}

/* What the program itself can read and write: ring 3's view. */
int kf_peek(uint64_t va, void *out, uint64_t n) {
    int r;
    g_copy_ring3 = 1;
    r = vibeos_uaccess_copy(out, (const void *)(uintptr_t)va, n);
    g_copy_ring3 = 0;
    return r;
}

int kf_poke(uint64_t va, const void *in, uint64_t n) {
    int r;
    g_copy_ring3 = 1;
    r = vibeos_uaccess_copy((void *)(uintptr_t)va, in, n);
    g_copy_ring3 = 0;
    return r;
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
/* What a sibling thread does the instant the address-space lock is released:
 * the test's to supply, fired once. A fork that reads the parent's state after
 * letting go of the lock reads what this left (M-078). */
static void (*g_mm_unlock_hook)(vibeos_procstate_t *ps);
void kf_on_mm_unlock(void (*fn)(vibeos_procstate_t *ps)) { g_mm_unlock_hook = fn; }
void ks_mm_unlock(vibeos_procstate_t *ps) {
    void (*fn)(vibeos_procstate_t *) = g_mm_unlock_hook;

    ps->mm_busy = 0u;
    if (fn) {
        g_mm_unlock_hook = 0;
        fn(ps);
    }
}
vibeos_vmspace_t ks_vm(int slot) {
    vibeos_vmspace_t v;
    memset(&v, 0, sizeof(v));
    return g_t[slot].has_as ? g_t[slot].as : v;
}
/* One fresh zeroed page at `va`, as the architecture maps one: the frame is
 * allocated, mapped - the mapping takes its own reference - and the
 * allocation's reference given back. */
int ks_map_anon(int slot, uint64_t va, vibeos_prot_t prot) {
    uint64_t phys;

    if (!g_t[slot].has_as || (phys = vibeos_frame_alloc(VIBEOS_FRAME_ALLOCATED)) == 0u) {
        return -1;
    }
    memset(kf_phys(phys), 0, 4096u);
    if (vibeos_vmspace_map(&g_t[slot].as, va, phys, prot) != 0) {
        (void)vibeos_frame_put(phys);
        return -1;
    }
    (void)vibeos_frame_put(phys);
    return 0;
}
int ks_map_page(int slot, uint64_t va, void *page, vibeos_prot_t prot) {
    return g_t[slot].has_as && vibeos_vmspace_map(&g_t[slot].as, va, kf_phys_of(page), prot) == 0
               ? 0 : -1;
}
void ks_page_unhold(void *page) { (void)vibeos_frame_put(kf_phys_of(page)); }
int ks_map_user_pages(int slot, uint64_t va, uint64_t pages) {
    uint64_t i;
    for (i = 0; i < pages; i++) {
        if (ks_map_anon(slot, va + i * 4096ull,
                        (vibeos_prot_t)(VIBEOS_PROT_READ | VIBEOS_PROT_WRITE | VIBEOS_PROT_USER)) != 0) {
            return -1;
        }
    }
    return 0;
}
/* Where a program may put a mapping it names the address of. */
int ks_user_fixed_ok(uint64_t base, uint64_t len) {
    return base >= KF_MM_LO && len <= KF_MM_HI - KF_MM_LO && base <= KF_MM_HI - len;
}
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
/* Pages a test says are shared (kf_share_page): the fake has no page tables,
 * so this is the whole of what ks_pageinfo knows - enough for a shared futex,
 * which asks which frame a word is on and nothing else. */
static struct {
    int slot;
    uint64_t page;
    uint64_t frame;
} g_shared_pages[8];
static uint32_t g_shared_count;

void kf_share_page(int slot, uint64_t va, uint64_t frame) {
    if (slot < 0) {
        g_shared_count = 0;
        return;
    }
    if (g_shared_count < 8u) {
        g_shared_pages[g_shared_count].slot = slot;
        g_shared_pages[g_shared_count].page = va & ~0xfffull;
        g_shared_pages[g_shared_count].frame = frame;
        g_shared_count++;
    }
}

    if (ps && ps->refs) {
        /* As the architecture's: the last reference closes a table nobody left. */
        if (ps->refs == 1u) {
            vibeos_fdtable_destroy(&ps->files);
    for (i = 0; i < g_shared_count; i++) {
        if (g_shared_pages[i].slot == slot && g_shared_pages[i].page == (va & ~0xfffull)) {
            out->frame = g_shared_pages[i].frame;
            out->flags = VIBEOS_PAGE_PRESENT | VIBEOS_PAGE_WRITE | VIBEOS_PAGE_USER | VIBEOS_PAGE_SHARED;
            out->owners = 2u;
        }
    }
        }
        ps->refs--;
    }
}
int ks_fork_aspace(int child, int parent) {
    if (!g_t[parent].has_as || vibeos_vmspace_create(&g_t[child].as) != 0) {
        return -1;
    }
    g_t[child].has_as = 1;
    if (vibeos_vmspace_clone_cow(&g_t[child].as, &g_t[parent].as) != 0) {
        (void)vibeos_vmspace_destroy(&g_t[child].as);
        g_t[child].has_as = 0;
        return -1;
    }
    return 0;
}
void ks_drop_aspace(int slot) {
    if (g_t[slot].has_as) {
        (void)vibeos_vmspace_destroy(&g_t[slot].as);
        g_t[slot].has_as = 0;
    }
}
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
void ks_regs_restart(ks_regs_t *frame, uint64_t nr) {
    frame->ret = nr;
    frame->ip -= 2u;
}

/* The fake's registers by the machine's names: ip, sp, ret and the first three
 * arguments are the ones the handlers and the tests look at; the rest is
 * carried so that a frame round trip can be checked whole. */
void ks_regs_get(const ks_regs_t *frame, vibeos_uregs_t *out) {
    memset(out, 0, sizeof(*out));
    out->rip = frame->ip;
    out->rsp = frame->sp;
    out->rax = frame->ret;
    out->rdi = frame->arg0;
    out->rsi = frame->arg1;
    out->rdx = frame->arg2;
    out->rflags = frame->flags;
    out->r8 = frame->other[0];  out->r9 = frame->other[1];
    out->r10 = frame->other[2]; out->r11 = frame->other[3];
    out->r12 = frame->other[4]; out->r13 = frame->other[5];
    out->r14 = frame->other[6]; out->r15 = frame->other[7];
    out->rbp = frame->other[8]; out->rbx = frame->other[9];
    out->cs = 0x33;
    out->ss = 0x2b;
}
/* Refuses what the machine refuses: a resume address outside user memory. The
 * fake's user memory is its arena and the pages it maps; anything below 0x10000
 * is code the tests pretend to run, and is accepted. */
int ks_regs_set(ks_regs_t *frame, const vibeos_uregs_t *in) {
    if (in->rip >> 47) {
        return -1;
    }
    frame->ip = in->rip;
    frame->sp = in->rsp;
    frame->ret = in->rax;
    frame->arg0 = in->rdi;
    frame->arg1 = in->rsi;
    frame->arg2 = in->rdx;
    frame->flags = (in->rflags & 0xCD5u) | 0x202u;
    frame->other[0] = in->r8;  frame->other[1] = in->r9;
    frame->other[2] = in->r10; frame->other[3] = in->r11;
    frame->other[4] = in->r12; frame->other[5] = in->r13;
    frame->other[6] = in->r14; frame->other[7] = in->r15;
    frame->other[8] = in->rbp; frame->other[9] = in->rbx;
    return 0;
}
unsigned char g_kf_fpu[512];
uint64_t ks_fpu_size(void) { return 512u; }
int ks_fpu_save(uint64_t uaddr) {
    return vibeos_uaccess_copy((void *)(uintptr_t)uaddr, g_kf_fpu, sizeof(g_kf_fpu)) == 0 ? 0 : -1;
}
int ks_fpu_restore(uint64_t uaddr) {
    unsigned char area[512];
    if (vibeos_uaccess_copy(area, (const void *)(uintptr_t)uaddr, sizeof(area)) != 0) {
        return -1;
    }
    area[26] = 0;   /* MXCSR's reserved half, as the machine clears it */
    area[27] = 0;
    memcpy(g_kf_fpu, area, sizeof(area));
    return 0;
}
void ks_regs_enter_handler(ks_regs_t *frame, uint64_t handler, uint64_t sp,
                           uint64_t a0, uint64_t a1, uint64_t a2) {
    frame->ip = handler;
    frame->sp = sp;
    frame->arg0 = a0;
    frame->arg1 = a1;
    frame->arg2 = a2;
    frame->ret = 0;
    frame->flags &= ~((1ull << 10) | (1ull << 8));
}
uint64_t ks_tls_get(int slot) { return g_t[slot].tls; }
void ks_tls_set(int slot, uint64_t base) { g_t[slot].tls = base; }

vibeos_inet_t *ks_net(void) { return g_net_is_up ? &g_net_v : 0; }
vibeos_lock_t *ks_net_lock(void) { return &g_net_lock_v; }
void kf_type(const char *s) {
    while (*s && g_kbd_len < sizeof(g_kbd)) {
        g_kbd[g_kbd_len++] = *s++;
    }
}

int ks_console_getc(void) {
    return g_kbd_at < g_kbd_len ? (int)(unsigned char)g_kbd[g_kbd_at++] : -1;
}
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
