/* SPDX-License-Identifier: GPL-3.0-or-later */
/* NT personality: registry keys (docs/abi/windows.md W8 step 3).
 *
 * The Nt*Key services over the registry's core (registry/reg.c). A key handle
 * is a description of its own type, "Key", holding the core's key - index and
 * tenancy - in `nt_key`; not an object of the object manager's table, which
 * is sized for events and threads, not for the hundreds of keys a program
 * opens. Its last reference gives the key back.
 *
 * Every service runs under one lock of the registry's own (g_io), which also
 * guards the staging buffer every argument and answer passes through: a
 * value's data can be a megabyte and a name 16,383 units, too much for a
 * kernel stack, and copying a program's memory never happens under the core's
 * lock. The lock is held across a flush's file writes, which the debounce
 * below keeps rare.
 *
 * The registry starts at the first call, not at boot: the personality is
 * started before the filesystem is mounted. Its hives are files on the boot
 * volume, \Windows\System32\config\MACHINE.HIV and USER.HIV; a missing or
 * unreadable hive is replaced by its .BAK, and with neither the registry
 * starts with the default content programs read. A change is written behind:
 * at the next change or key closed a second or more after the last save, and
 * at NtFlushKey - never more than once a second, whatever a program does. A
 * change made in the last second before nothing else touches the registry
 * waits for the next call (WO-088). Each save writes .NEW,
 * moves the hive to .BAK and .NEW into its place, so that a crash at any
 * point leaves one good image. */

#include "nt_internal.h"
#include "vibeos/ntreg.h"
#include "vibeos/vfs.h"

#define KEY_QUERY_VALUE        0x0001u
#define KEY_SET_VALUE          0x0002u
#define KEY_CREATE_SUB_KEY     0x0004u
#define KEY_ENUMERATE_SUB_KEYS 0x0008u
#define KEY_ALL_ACCESS         0x000F003Fu
#define KEY_READ               0x00020019u
#define KEY_WRITE              0x00020006u

#define REG_OPTION_VOLATILE    0x1u
#define REG_OPTION_CREATE_LINK 0x2u
#define REG_OPTION_BACKUP      0x4u
#define REG_OPTION_OPEN_LINK   0x8u

/* KEY_INFORMATION_CLASS and KEY_VALUE_INFORMATION_CLASS (wdm.h). */
#define KEY_BASIC        0u
#define KEY_NODE         1u
#define KEY_FULL         2u
#define KEY_NAME         3u
#define KEY_CACHED       4u
#define VALUE_BASIC      0u
#define VALUE_FULL       1u
#define VALUE_PARTIAL    2u
#define VALUE_PARTIAL64  3u

static const nt_type_t g_key_type = {
    "Key", KEY_ALL_ACCESS, KEY_READ, KEY_WRITE, KEY_READ, 0, 0,
};

static void key_release(vibeos_file_t *f);

const vibeos_file_ops_t g_nt_key_ops = {
    .name = "nt-key",
    .release = key_release,
    .nt_type = &g_key_type,
};

/* ---- the registry's lock, its staging and its start --------------------------------------- */

#define STAGE_BYTES (VIBEOS_REG_DATA_MAX + 65536u)   /* data, a header and a long name */
#define PATH_UNITS  32767u                            /* Windows' longest key path */

static vibeos_lock_t g_io;      /* the services, the staging buffer, the hive files */
static vibeos_lock_t g_core;    /* the core's own, under g_io */
static uint8_t g_stage[STAGE_BYTES];
static uint16_t g_path[PATH_UNITS + 1u];
static uint16_t g_vname[VIBEOS_REG_VALUE_NAME_MAX + 1u];
static int g_started;
static uint64_t g_saved;        /* vibeos_reg_changes() at the last save */
static uint64_t g_saved_tick;
static int g_pending;           /* a change not yet written */
static int g_no_volume;         /* the volume refused a save: memory only, said once */

/* Pages the registry gave back, kept for its next value rather than returned
 * to the frame layer. A large value is a chain of pages, and setting one again
 * frees the old chain and takes a new one: a megabyte is 256 frees, and the
 * frame layer's audit walks every process's page tables on every eighth free
 * (hw_free_page_why), with the caller's interrupts masked. Written while a
 * boot went quiet under ntdll:reg, as its suspected cause - which a count of
 * the frees disproved: none reached the frame layer, and the test was in a
 * loop of its own (see reg_defaults). Kept as what it is, a cheaper registry.
 * Up to two megabytes; past that, given back. Under the core's lock, which is
 * the only caller's. */
#define REG_PAGE_CACHE 512u
static void *g_page_cache[REG_PAGE_CACHE];
static uint32_t g_page_cached;

static void *reg_page_alloc(void) {
    if (g_page_cached != 0u) {
        return g_page_cache[--g_page_cached];
    }
    return ks_page_alloc();
}

static void reg_page_free(void *p) {
    if (g_page_cached < REG_PAGE_CACHE) {
        g_page_cache[g_page_cached++] = p;
        return;
    }
    ks_page_free(p, "registry");
}
static void reg_lock(void) { ks_lock_preemptible(&g_core); }
static void reg_unlock(void) { ks_unlock_preemptible(&g_core); }
static uint64_t reg_now(void) { return nt_ticks_to_filetime(ks_ticks()); }

/* Which task holds g_io, or -1. The last reference to a key's description can
 * be put with the lock held - a handle install that fails (a full table), a
 * handle closed because its number could not be copied out, a sibling's close
 * landing between key_ref's reference and its put - and the release then
 * took g_io again: a self-deadlock any program reached with 1024 key handles
 * (v8 review, H-041). The release asks this instead of locking blindly. */
static int g_io_owner = -1;

static void io_lock(void) {
    ks_lock_preemptible(&g_io);
    __atomic_store_n(&g_io_owner, ks_current(), __ATOMIC_RELAXED);
}

static void io_unlock(void) {
    __atomic_store_n(&g_io_owner, -1, __ATOMIC_RELAXED);
    ks_unlock_preemptible(&g_io);
}

/* Does the calling task already hold g_io? Only it can have stored its own
 * number there, so a match cannot be another task's. */
static int io_held(void) {
    int me = ks_current();

    return me >= 0 && __atomic_load_n(&g_io_owner, __ATOMIC_RELAXED) == me;
}

static const char *const g_hive_dirs[] = { "/windows", "/windows/system32", "/windows/system32/config" };
static const char *const g_hive_file[2] = { "/windows/system32/config/MACHINE.HIV",
                                            "/windows/system32/config/USER.HIV" };
static const char g_hive_root[2][8] = { "MACHINE", "USER" };

static uint32_t ascii_units(const char *s, uint16_t *out) {
    uint32_t i;

    for (i = 0; s[i]; i++) {
        out[i] = (uint16_t)(unsigned char)s[i];
    }
    return i;
}

/* The key named by an ASCII path under \Registry, referenced, or a status. */
static uint32_t reg_open_ascii(const char *path, int create, vibeos_reg_key_t *out) {
    vibeos_reg_key_t root = vibeos_reg_root();
    uint16_t p[160];
    uint32_t n = ascii_units(path, p), st, disp, at = 0;

    if (!create) {
        st = vibeos_reg_open(root, p, n, out);
        vibeos_reg_unref(root);
        return st;
    }
    /* Each component in turn: create makes only the last of a path. */
    st = VIBEOS_STATUS_SUCCESS;
    *out = root;
    while (st == VIBEOS_STATUS_SUCCESS && at < n) {
        uint32_t end = at;
        vibeos_reg_key_t next;

        while (end < n && p[end] != '\\') {
            end++;
        }
        st = vibeos_reg_create(*out, p + at, end - at, 0u, &next, &disp);
        vibeos_reg_unref(*out);
        if (st == VIBEOS_STATUS_SUCCESS) {
            *out = next;
        }
        at = end + 1u;
    }
    return st;
}

static void set_sz(vibeos_reg_key_t k, const char *name, const char *value) {
    uint16_t n[64], v[64];
    uint32_t nn = ascii_units(name, n), vn = ascii_units(value, v);

    v[vn++] = 0;
    (void)vibeos_reg_set(k, n, nn, VIBEOS_REG_SZ, v, vn * 2u);
}

static void set_dword(vibeos_reg_key_t k, const char *name, uint32_t value) {
    uint16_t n[64];
    uint32_t nn = ascii_units(name, n);

    (void)vibeos_reg_set(k, n, nn, VIBEOS_REG_DWORD, &value, 4u);
}

/* What a fresh machine has: the version programs read - agreeing with
 * kernel32's GetVersionEx (Windows 10, build 19045) - and the keys the
 * predefined handles name. */
static void reg_defaults(void) {
    static const char *const keys[] = {
        "MACHINE\\SOFTWARE", "MACHINE\\SOFTWARE\\Classes", "MACHINE\\SOFTWARE\\Microsoft",
        /* Where Windows x64 keeps 32-bit programs' view. There are no 32-bit
         * programs here (WoW64 is not planned) and nothing is redirected, but
         * a 64-bit program may name these keys and finds them. */
        "MACHINE\\SOFTWARE\\Wow6432Node", "MACHINE\\SOFTWARE\\Classes\\Wow6432Node",
        /* And the 32-bit view's Classes, which Windows x64 always has: without
         * it ntdll:reg's test_redirection reads a key count out of a buffer a
         * failed query left as it was, and enumerated for minutes. */
        "MACHINE\\SOFTWARE\\Wow6432Node\\Classes",
        "MACHINE\\SOFTWARE\\Microsoft\\Windows NT",
        "MACHINE\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
        "MACHINE\\SYSTEM", "MACHINE\\SYSTEM\\CurrentControlSet",
        "MACHINE\\SYSTEM\\CurrentControlSet\\Control", "USER\\.DEFAULT",
        "USER\\.DEFAULT\\Software", "USER\\.DEFAULT\\Environment",
    };
    vibeos_reg_key_t k;
    uint32_t i;

    for (i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if (reg_open_ascii(keys[i], 1, &k) == VIBEOS_STATUS_SUCCESS) {
            vibeos_reg_unref(k);
        }
    }
    if (reg_open_ascii("MACHINE\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0, &k) ==
        VIBEOS_STATUS_SUCCESS) {
        set_sz(k, "ProductName", "VibeOS");
        set_sz(k, "CurrentVersion", "6.3");
        set_sz(k, "CurrentBuild", "19045");
        set_sz(k, "CurrentBuildNumber", "19045");
        set_sz(k, "CurrentType", "Multiprocessor Free");
        set_sz(k, "InstallationType", "Client");
        set_dword(k, "CurrentMajorVersionNumber", 10u);
        set_dword(k, "CurrentMinorVersionNumber", 0u);
        vibeos_reg_unref(k);
    }
}

/* ---- hives on the volume ---------------------------------------------------------------- */

static uint32_t g_stage_len;

static int stage_sink(void *ctx, const void *buf, uint32_t len) {
    (void)ctx;
    if (len > STAGE_BYTES - g_stage_len) {
        return -1;   /* a hive larger than the staging buffer is not saved half */
    }
    {
        const uint8_t *b = (const uint8_t *)buf;
        uint32_t i;

        for (i = 0; i < len; i++) {
            g_stage[g_stage_len + i] = b[i];
        }
    }
    g_stage_len += len;
    return 0;
}

/* `path` with `suffix`, into `out`. */
static void with_suffix(const char *path, const char *suffix, char *out, uint32_t cap) {
    uint32_t i = 0, j = 0;

    while (path[i] && i + 1u < cap) {
        out[i] = path[i];
        i++;
    }
    while (suffix[j] && i + 1u < cap) {
        out[i++] = suffix[j++];
    }
    out[i] = 0;
}

/* Load hive `h`, from its file, else its backup, else the copy a save on a
 * volume without rename left beside it: 0 if one loaded. */
static int hive_load(uint32_t h) {
    char bak[96], nw[96];
    const char *paths[3];
    vibeos_reg_key_t k;
    uint32_t i;

    with_suffix(g_hive_file[h], ".BAK", bak, sizeof(bak));
    with_suffix(g_hive_file[h], ".NEW", nw, sizeof(nw));
    paths[0] = g_hive_file[h];
    paths[1] = bak;
    paths[2] = nw;
    if (reg_open_ascii(g_hive_root[h], 0, &k) != VIBEOS_STATUS_SUCCESS) {
        return -1;
    }
    for (i = 0; i < 3u; i++) {
        vibeos_fsmount_t *m;
        const char *tail;
        long n;

        if (vibeos_fs_resolve(paths[i], &m, &tail) != 0) {
            continue;
        }
        n = vibeos_fs_read_file(m, tail, g_stage, STAGE_BYTES);
        if (n > 0 && vibeos_reg_load(k, g_stage, (uint64_t)n) == VIBEOS_STATUS_SUCCESS) {
            vibeos_reg_unref(k);
            return 0;
        }
        if (n > 0) {
            ks_log(VIBEOS_LOG_WARN, 90u, h, i, "registry: a hive did not check; trying its backup");
        }
    }
    vibeos_reg_unref(k);
    return -1;
}

/* Save hive `h`: .NEW written, the hive moved to .BAK, .NEW into its place.
 * A volume that cannot rename gets the hive written in place instead, and
 * keeps .NEW whole beside it - which the load reads if the hive is not. */
static int hive_save(uint32_t h) {
    char nw[96], bak[96];
    vibeos_fsmount_t *m;
    const char *tail, *t_new, *t_bak;
    vibeos_reg_key_t k;
    uint32_t i;

    if (reg_open_ascii(g_hive_root[h], 0, &k) != VIBEOS_STATUS_SUCCESS) {
        return -1;
    }
    g_stage_len = 0;
    i = vibeos_reg_save(k, stage_sink, 0);
    vibeos_reg_unref(k);
    if (i != VIBEOS_STATUS_SUCCESS || vibeos_fs_resolve(g_hive_file[h], &m, &tail) != 0) {
        return -1;
    }
    for (i = 0; i < sizeof(g_hive_dirs) / sizeof(g_hive_dirs[0]); i++) {
        vibeos_fsmount_t *dm;
        const char *dt;

        if (vibeos_fs_resolve(g_hive_dirs[i], &dm, &dt) == 0) {
            (void)vibeos_fs_mkdir(dm, dt);   /* there already, or now */
        }
    }
    with_suffix(g_hive_file[h], ".NEW", nw, sizeof(nw));
    with_suffix(g_hive_file[h], ".BAK", bak, sizeof(bak));
    if (vibeos_fs_resolve(nw, &m, &t_new) != 0 || vibeos_fs_resolve(bak, &m, &t_bak) != 0 ||
        vibeos_fs_write_file(m, t_new, g_stage, g_stage_len) != (long)g_stage_len) {
        return -1;
    }
    (void)vibeos_fs_unlink(m, t_bak);
    (void)vibeos_fs_rename(m, tail, t_bak, 0u);   /* absent the first time */
    if (vibeos_fs_rename(m, t_new, tail, 0u) != 0 &&
        vibeos_fs_write_file(m, tail, g_stage, g_stage_len) != (long)g_stage_len) {
        return -1;
    }
    return 0;
}

static void flush_now(void) {
    uint64_t changes = vibeos_reg_changes();
    uint32_t h;
    int ok = 1;

    for (h = 0; h < 2u; h++) {
        if (hive_save(h) != 0) {
            ok = 0;
        }
    }
    g_saved_tick = ks_ticks();
    if (ok) {
        g_saved = changes;
        g_pending = 0;
    } else if (!g_no_volume) {
        g_no_volume = 1;
        ks_log(VIBEOS_LOG_WARN, 91u, 0, 0, "registry: the hives could not be written; kept in memory");
    }
}

/* After a call that may have changed something: written now if the last
 * save was a second or more ago, otherwise left for later. */
static void flush_behind(void) {
    if (vibeos_reg_changes() == g_saved) {
        return;
    }
    if (ks_ticks() - g_saved_tick >= (uint64_t)ks_hz()) {
        flush_now();
    } else {
        g_pending = 1;
    }
}

/* Under g_io: the registry, started once. */
static void reg_start(void) {
    vibeos_reg_env_t env;
    uint32_t h;
    int loaded = 0;

    if (g_started) {
        return;
    }
    env.page_alloc = reg_page_alloc;
    env.page_free = reg_page_free;
    env.lock = reg_lock;
    env.unlock = reg_unlock;
    env.now = reg_now;
    env.quota = 16u * 1024u * 1024u;
    vibeos_reg_init(&env);
    for (h = 0; h < 2u; h++) {
        if (hive_load(h) == 0) {
            loaded++;
        }
    }
    /* What every machine has, made where it is missing - on a first start,
     * and after a hive was lost with its backup. */
    reg_defaults();
    g_started = 1;
    g_saved = loaded == 2 ? vibeos_reg_changes() : 0u;
    g_saved_tick = 0;
    flush_behind();
}

/* ---- handles ------------------------------------------------------------------------------ */

static vibeos_reg_key_t key_of(const vibeos_file_t *f) {
    vibeos_reg_key_t k;

    k.idx = (uint32_t)f->nt_key;
    k.gen = (uint32_t)(f->nt_key >> 32);
    return k;
}

static void key_release(vibeos_file_t *f) {
    const int held = io_held();

    if (!held) {
        io_lock();
    }
    vibeos_reg_unref(key_of(f));
    /* The same second's limit as every other write-behind. A save on every
     * close of a key that changed something rewrites the whole hive each time
     * a program opens, writes and closes in a loop - a file write per close.
     * (Suspected for a quiet boot under ntdll:reg and disproved; it is a limit
     * worth having anyway.) */
    if (g_pending) {
        flush_behind();
    }
    if (!held) {
        io_unlock();
    }
}

/* A handle for `k`, whose reference it takes over; written to `out_u`. */
static uint32_t key_handle(vibeos_reg_key_t k, uint32_t want, uint32_t attributes, uint64_t out_u) {
    vibeos_file_t *f = vibeos_file_alloc(&g_nt_key_ops, 0);
    uint64_t h = 0;
    uint32_t st;

    if (!f) {
        vibeos_reg_unref(k);
        return VIBEOS_STATUS_INSUFFICIENT_RESOURCES;
    }
    f->nt_key = (uint64_t)k.idx | ((uint64_t)k.gen << 32);
    st = nt_handle_install(f, nt_map_access(&g_nt_key_ops, want), attributes, &h);
    if (st != VIBEOS_STATUS_SUCCESS) {
        return st;   /* the install released the description, and with it the key */
    }
    if (nt_copy_out(out_u, &h, sizeof(h)) != 0) {
        (void)nt_close_handle(h);
        return VIBEOS_STATUS_ACCESS_VIOLATION;
    }
    return VIBEOS_STATUS_SUCCESS;
}

/* The key a handle names, with `need` granted; referenced. */
static uint32_t key_ref(uint64_t h, uint32_t need, vibeos_reg_key_t *k) {
    vibeos_file_t *f;
    uint32_t st = nt_handle_ref(h, &g_nt_key_ops, need, &f, 0);

    if (st != VIBEOS_STATUS_SUCCESS) {
        return st;
    }
    *k = key_of(f);
    vibeos_reg_ref(*k);
    vibeos_file_put(f);
    return VIBEOS_STATUS_SUCCESS;
}

/* A UNICODE_STRING of the program's, into `out` (`cap` units). */
static uint32_t read_ustring(uint64_t us, uint16_t *out, uint32_t cap, uint32_t *n) {
    uint8_t s[16];
    uint64_t buf;
    uint32_t len;

    if (us == 0u || nt_copy_in(s, us, sizeof(s)) != 0) {
        return VIBEOS_STATUS_ACCESS_VIOLATION;
    }
    len = (uint32_t)s[0] | ((uint32_t)s[1] << 8);
    buf = 0;
    for (*n = 0; *n < 8u; (*n)++) {
        buf |= (uint64_t)s[8u + *n] << (8u * *n);
    }
    if ((len & 1u) || len / 2u > cap) {
        return VIBEOS_STATUS_INVALID_PARAMETER;
    }
    if (len != 0u && nt_copy_in(out, buf, len) != 0) {
        return VIBEOS_STATUS_ACCESS_VIOLATION;
    }
    *n = len / 2u;
    return VIBEOS_STATUS_SUCCESS;
}

/* OBJECT_ATTRIBUTES as a key to start from and a path under it: a root key's
 * handle and a relative path, or no root and a path under \Registry. The key
 * is referenced, the path is in g_path. */
static uint32_t key_from_oa(uint64_t oa, vibeos_reg_key_t *base, uint32_t *n, uint32_t *attributes) {
    static const char prefix[] = "\\REGISTRY";
    uint8_t a[32];
    uint64_t root = 0, name = 0;
    uint32_t st, i, start = 0;

    if (oa == 0u || nt_copy_in(a, oa, sizeof(a)) != 0) {
        return VIBEOS_STATUS_ACCESS_VIOLATION;
    }
    for (i = 0; i < 8u; i++) {
        root |= (uint64_t)a[8u + i] << (8u * i);
        name |= (uint64_t)a[16u + i] << (8u * i);
    }
    *attributes = (uint32_t)a[24] | ((uint32_t)a[25] << 8) | ((uint32_t)a[26] << 16) |
                  ((uint32_t)a[27] << 24);
    st = read_ustring(name, g_path, PATH_UNITS, n);
    if (st != VIBEOS_STATUS_SUCCESS) {
        return st;
    }
    while (*n > 0u && g_path[*n - 1u] == '\\') {
        (*n)--;   /* "Software\" names Software, as Windows reads it */
    }
    if (root != 0u) {
        if (*n > 0u && g_path[0] == '\\') {
            return VIBEOS_STATUS_OBJECT_PATH_SYNTAX_BAD;
        }
        return key_ref(root, 0u, base);
    }
    if (*n == 0u || g_path[0] != '\\') {
        return VIBEOS_STATUS_OBJECT_PATH_SYNTAX_BAD;
    }
    for (i = 0; prefix[i]; i++) {
        if (i >= *n || vibeos_reg_upcase(g_path[i]) != (uint16_t)prefix[i]) {
            return VIBEOS_STATUS_OBJECT_TYPE_MISMATCH;   /* another namespace's object */
        }
    }
    if (*n > i && g_path[i] != '\\') {
        return VIBEOS_STATUS_OBJECT_TYPE_MISMATCH;      /* "\RegistryX" */
    }
    start = *n > i ? i + 1u : i;
    for (i = start; i < *n; i++) {
        g_path[i - start] = g_path[i];
    }
    *n -= start;
    *base = vibeos_reg_root();
    return VIBEOS_STATUS_SUCCESS;
}

/* ---- creating, opening, deleting -------------------------------------------------------- */

static long key_open(uint64_t out_u, uint32_t want, uint64_t oa, int create, uint32_t options,
                     uint64_t disp_u) {
    vibeos_reg_key_t base, k;
    uint32_t st, n = 0, attributes = 0, disp = 0;

    if (create && (options & ~(REG_OPTION_VOLATILE | REG_OPTION_CREATE_LINK | REG_OPTION_BACKUP |
                               REG_OPTION_OPEN_LINK)) != 0u) {
        return NT_STATUS(VIBEOS_STATUS_INVALID_PARAMETER);
    }
    if (create && (options & REG_OPTION_CREATE_LINK)) {
        return NT_STATUS(VIBEOS_STATUS_NOT_SUPPORTED);   /* symbolic keys: not here (WO-085) */
    }
    io_lock();
    reg_start();
    st = key_from_oa(oa, &base, &n, &attributes);
    if (st == VIBEOS_STATUS_SUCCESS) {
        st = create ? vibeos_reg_create(base, g_path, n,
                                        (options & REG_OPTION_VOLATILE) ? VIBEOS_REG_VOLATILE : 0u, &k,
                                        &disp)
                    : vibeos_reg_open(base, g_path, n, &k);
        vibeos_reg_unref(base);
    }
    if (st == VIBEOS_STATUS_SUCCESS) {
        st = key_handle(k, want, attributes, out_u);
    }
    if (st == VIBEOS_STATUS_SUCCESS && create && disp_u != 0u &&
        nt_copy_out(disp_u, &disp, sizeof(disp)) != 0) {
        st = VIBEOS_STATUS_ACCESS_VIOLATION;
    }
    if (create) {
        flush_behind();
    }
    io_unlock();
    return NT_STATUS(st);
}

/* NtCreateKey(KeyHandle, DesiredAccess, ObjectAttributes, TitleIndex, Class,
 * CreateOptions, Disposition). The class is not kept: KeyFullInformation
 * reports none. */
static long nt_create_key(uint64_t out, uint64_t want, uint64_t oa, uint64_t options,
                          uint64_t disp) {
    return key_open(out, (uint32_t)want, oa, 1, (uint32_t)options, disp);
}

static long nt_open_key(uint64_t out, uint64_t want, uint64_t oa) {
    return key_open(out, (uint32_t)want, oa, 0, 0u, 0u);
}

/* NtOpenKeyEx(KeyHandle, DesiredAccess, ObjectAttributes, OpenOptions): the
 * options are the backup and link ones, neither of which changes an open
 * here. */
static long nt_open_key_ex(uint64_t out, uint64_t want, uint64_t oa, uint64_t options) {
    if ((uint32_t)options & ~(REG_OPTION_BACKUP | REG_OPTION_OPEN_LINK)) {
        return NT_STATUS(VIBEOS_STATUS_INVALID_PARAMETER);
    }
    return key_open(out, (uint32_t)want, oa, 0, 0u, 0u);
}

static long nt_delete_key(uint64_t h) {
    vibeos_reg_key_t k;
    uint32_t st;

    io_lock();
    reg_start();
    st = key_ref(h, VIBEOS_DELETE, &k);
    if (st == VIBEOS_STATUS_SUCCESS) {
        st = vibeos_reg_delete(k);
        vibeos_reg_unref(k);
    }
    flush_behind();
    io_unlock();
    return NT_STATUS(st);
}

static long nt_rename_key(uint64_t h, uint64_t name_u) {
    vibeos_reg_key_t k;
    uint32_t st, n = 0;

    io_lock();
    reg_start();
    /* The name before the handle's rights, as Windows judges them: an empty
     * name is INVALID_PARAMETER even through a handle that may not write
     * (ntdll:reg's test_NtRenameKey). */
    st = read_ustring(name_u, g_path, PATH_UNITS, &n);
    if (st == VIBEOS_STATUS_SUCCESS && n == 0u) {
        st = VIBEOS_STATUS_INVALID_PARAMETER;
    }
    if (st == VIBEOS_STATUS_SUCCESS) {
        st = key_ref(h, KEY_WRITE & ~VIBEOS_READ_CONTROL, &k);
        if (st == VIBEOS_STATUS_SUCCESS) {
            st = vibeos_reg_rename(k, g_path, n);
            vibeos_reg_unref(k);
        }
    }
    flush_behind();
    io_unlock();
    return NT_STATUS(st);
}

static long nt_flush_key(uint64_t h) {
    vibeos_reg_key_t k;
    uint32_t st;

    io_lock();
    reg_start();
    st = key_ref(h, 0u, &k);
    if (st == VIBEOS_STATUS_SUCCESS) {
        vibeos_reg_unref(k);
        flush_now();
    }
    io_unlock();
    return NT_STATUS(st);
}

/* ---- answering in a program's buffer ------------------------------------------------------
 *
 * Every information class has a fixed part and a variable one. Windows' rule,
 * which Wine's tests check: the length the whole answer needs is always
 * reported; a buffer shorter than the fixed part gets nothing and
 * STATUS_BUFFER_TOO_SMALL; one that holds the fixed part but not the rest gets
 * as much as fits and STATUS_BUFFER_OVERFLOW. */

static uint32_t answer(uint64_t info_u, uint32_t len, uint64_t ret_u, uint32_t fixed, uint32_t total) {
    uint32_t st = VIBEOS_STATUS_SUCCESS, n = total;

    if (ret_u != 0u && nt_copy_out(ret_u, &total, sizeof(total)) != 0) {
        return VIBEOS_STATUS_ACCESS_VIOLATION;
    }
    if (len < fixed) {
        return VIBEOS_STATUS_BUFFER_TOO_SMALL;
    }
    if (len < total) {
        n = len;
        st = VIBEOS_STATUS_BUFFER_OVERFLOW;
    }
    if (n != 0u && nt_copy_out(info_u, g_stage, n) != 0) {
        return VIBEOS_STATUS_ACCESS_VIOLATION;
    }
    return st;
}

static void put32(uint32_t at, uint32_t v) {
    g_stage[at] = (uint8_t)v;
    g_stage[at + 1u] = (uint8_t)(v >> 8);
    g_stage[at + 2u] = (uint8_t)(v >> 16);
    g_stage[at + 3u] = (uint8_t)(v >> 24);
}

static void put64(uint32_t at, uint64_t v) {
    put32(at, (uint32_t)v);
    put32(at + 4u, (uint32_t)(v >> 32));
}

/* A value's record of class `cls` into the staging buffer, from its name (in
 * g_vname, `nlen` bytes), type and data (already at `data_at`). The fixed
 * part's size and the whole's. */
static uint32_t value_record(uint32_t cls, uint32_t type, uint32_t nlen, uint32_t dlen,
                             uint32_t *fixed, uint32_t *total) {
    uint32_t i;

    switch (cls) {
        case VALUE_BASIC:
            *fixed = 12u;
            *total = 12u + nlen;
            put32(0, 0);
            put32(4, type);
            put32(8, nlen);
            for (i = 0; i < nlen; i++) {
                g_stage[12u + i] = ((const uint8_t *)g_vname)[i];
            }
            return VIBEOS_STATUS_SUCCESS;
        case VALUE_FULL:
            *fixed = 20u;
            *total = 20u + nlen + dlen;
            put32(0, 0);
            put32(4, type);
            put32(8, 20u + nlen);   /* DataOffset: right after the name, unaligned as Wine has it */
            put32(12, dlen);
            put32(16, nlen);
            for (i = 0; i < nlen; i++) {
                g_stage[20u + i] = ((const uint8_t *)g_vname)[i];
            }
            return VIBEOS_STATUS_SUCCESS;
        case VALUE_PARTIAL:
            *fixed = 12u;
            *total = 12u + dlen;
            put32(0, 0);
            put32(4, type);
            put32(8, dlen);
            return VIBEOS_STATUS_SUCCESS;
        case VALUE_PARTIAL64:
            *fixed = 8u;
            *total = 8u + dlen;
            put32(0, type);
            put32(4, dlen);
            return VIBEOS_STATUS_SUCCESS;
        default:
            return VIBEOS_STATUS_INVALID_PARAMETER;
    }
}

/* Where a class wants the data, given the name's length: it is fetched
 * there before the record's head is written around it. */
static uint32_t data_at(uint32_t cls, uint32_t nlen) {
    switch (cls) {
        case VALUE_FULL: return 20u + nlen;
        case VALUE_PARTIAL: return 12u;
        case VALUE_PARTIAL64: return 8u;
        default: return 0u;
    }
}

/* ---- values ------------------------------------------------------------------------------- */

/* NtSetValueKey(KeyHandle, ValueName, TitleIndex, Type, Data, DataSize). */
static long nt_set_value_key(uint64_t h, uint64_t name_u, uint64_t type, uint64_t data_u,
                             uint64_t size) {
    vibeos_reg_key_t k;
    uint32_t st, n = 0;

    if ((uint32_t)size > VIBEOS_REG_DATA_MAX) {
        return NT_STATUS(VIBEOS_STATUS_INSUFFICIENT_RESOURCES);
    }
    io_lock();
    reg_start();
    st = key_ref(h, KEY_SET_VALUE, &k);
    if (st == VIBEOS_STATUS_SUCCESS) {
        st = read_ustring(name_u, g_vname, VIBEOS_REG_VALUE_NAME_MAX, &n);
        if (st == VIBEOS_STATUS_SUCCESS && (uint32_t)size != 0u &&
            nt_copy_in(g_stage, data_u, (uint32_t)size) != 0) {
            st = VIBEOS_STATUS_ACCESS_VIOLATION;
        }
        if (st == VIBEOS_STATUS_SUCCESS) {
            st = vibeos_reg_set(k, g_vname, n, (uint32_t)type, g_stage, (uint32_t)size);
        }
        vibeos_reg_unref(k);
    }
    flush_behind();
    io_unlock();
    return NT_STATUS(st);
}

/* NtQueryValueKey(KeyHandle, ValueName, Class, Information, Length,
 * ResultLength). */
static long nt_query_value_key(uint64_t h, uint64_t name_u, uint64_t cls, uint64_t info_u,
                               uint64_t len, uint64_t ret_u) {
    vibeos_reg_key_t k;
    uint32_t st, n = 0, type = 0, dlen = 0, fixed = 0, total = 0;

    if ((uint32_t)cls > VALUE_PARTIAL64) {
        return NT_STATUS(VIBEOS_STATUS_INVALID_PARAMETER);
    }
    io_lock();
    reg_start();
    st = key_ref(h, KEY_QUERY_VALUE, &k);
    if (st == VIBEOS_STATUS_SUCCESS) {
        st = read_ustring(name_u, g_vname, VIBEOS_REG_VALUE_NAME_MAX, &n);
        if (st == VIBEOS_STATUS_SUCCESS) {
            uint32_t at = data_at((uint32_t)cls, n * 2u);

            st = vibeos_reg_get(k, g_vname, n, &type, (uint32_t)cls == VALUE_BASIC ? 0 : g_stage + at,
                                STAGE_BYTES - at, &dlen);
        }
        if (st == VIBEOS_STATUS_SUCCESS) {
            st = value_record((uint32_t)cls, type, n * 2u, dlen, &fixed, &total);
        }
        if (st == VIBEOS_STATUS_SUCCESS) {
            st = answer(info_u, (uint32_t)len, ret_u, fixed, total);
        }
        vibeos_reg_unref(k);
    }
    io_unlock();
    return NT_STATUS(st);
}

/* NtEnumerateValueKey(KeyHandle, Index, Class, Information, Length,
 * ResultLength). */
static long nt_enumerate_value_key(uint64_t h, uint64_t index, uint64_t cls, uint64_t info_u,
                                   uint64_t len, uint64_t ret_u) {
    vibeos_reg_key_t k;
    uint32_t st, nlen = 0, type = 0, dlen = 0, fixed = 0, total = 0;

    if ((uint32_t)cls > VALUE_PARTIAL64) {
        return NT_STATUS(VIBEOS_STATUS_INVALID_PARAMETER);
    }
    io_lock();
    reg_start();
    st = key_ref(h, KEY_QUERY_VALUE, &k);
    if (st == VIBEOS_STATUS_SUCCESS) {
        /* The name first, to learn where the data goes after it. */
        st = vibeos_reg_enum_value(k, (uint32_t)index, g_vname, sizeof(g_vname), &nlen, &type, 0, 0,
                                   &dlen);
        if (st == VIBEOS_STATUS_SUCCESS && (uint32_t)cls != VALUE_BASIC) {
            uint32_t at = data_at((uint32_t)cls, nlen), ignored;

            st = vibeos_reg_enum_value(k, (uint32_t)index, 0, 0, &ignored, &type, g_stage + at,
                                       STAGE_BYTES - at, &dlen);
        }
        if (st == VIBEOS_STATUS_SUCCESS) {
            st = value_record((uint32_t)cls, type, nlen, dlen, &fixed, &total);
        }
        if (st == VIBEOS_STATUS_SUCCESS) {
            st = answer(info_u, (uint32_t)len, ret_u, fixed, total);
        }
        vibeos_reg_unref(k);
    }
    io_unlock();
    return NT_STATUS(st);
}

static long nt_delete_value_key(uint64_t h, uint64_t name_u) {
    vibeos_reg_key_t k;
    uint32_t st, n = 0;

    io_lock();
    reg_start();
    st = key_ref(h, KEY_SET_VALUE, &k);
    if (st == VIBEOS_STATUS_SUCCESS) {
        st = read_ustring(name_u, g_vname, VIBEOS_REG_VALUE_NAME_MAX, &n);
        if (st == VIBEOS_STATUS_SUCCESS) {
            st = vibeos_reg_unset(k, g_vname, n);
        }
        vibeos_reg_unref(k);
    }
    flush_behind();
    io_unlock();
    return NT_STATUS(st);
}

/* ---- describing keys ---------------------------------------------------------------------- */

/* Key `k`'s record of class `cls` in the staging buffer; its name (`nlen`
 * bytes) is already in g_vname - the key's own, or a subkey's. */
static uint32_t key_record(vibeos_reg_key_t k, uint32_t cls, uint32_t nlen, uint64_t last_write,
                           uint32_t *fixed, uint32_t *total) {
    vibeos_reg_info_t in;
    uint32_t i, st;

    switch (cls) {
        case KEY_BASIC:
            *fixed = 16u;
            *total = 16u + nlen;
            put64(0, last_write);
            put32(8, 0);
            put32(12, nlen);
            for (i = 0; i < nlen; i++) {
                g_stage[16u + i] = ((const uint8_t *)g_vname)[i];
            }
            return VIBEOS_STATUS_SUCCESS;
        case KEY_NODE:
            *fixed = 24u;
            *total = 24u + nlen;
            put64(0, last_write);
            put32(8, 0);
            put32(12, 0xFFFFFFFFu);   /* ClassOffset: no class */
            put32(16, 0);
            put32(20, nlen);
            for (i = 0; i < nlen; i++) {
                g_stage[24u + i] = ((const uint8_t *)g_vname)[i];
            }
            return VIBEOS_STATUS_SUCCESS;
        case KEY_FULL:
        case KEY_CACHED:
            st = vibeos_reg_info(k, &in);
            if (st != VIBEOS_STATUS_SUCCESS) {
                return st;
            }
            put64(0, in.last_write);
            put32(8, 0);
            if (cls == KEY_FULL) {
                *fixed = *total = 44u;
                put32(12, 0xFFFFFFFFu);
                put32(16, 0);
                put32(20, in.subkeys);
                put32(24, in.max_subkey_name);
                put32(28, 0);
                put32(32, in.values);
                put32(36, in.max_value_name);
                put32(40, in.max_value_data);
            } else {
                *fixed = 36u;
                *total = 36u;
                put32(12, in.subkeys);
                put32(16, in.max_subkey_name);
                put32(20, in.values);
                put32(24, in.max_value_name);
                put32(28, in.max_value_data);
                put32(32, nlen);
            }
            return VIBEOS_STATUS_SUCCESS;
        default:
            return VIBEOS_STATUS_INVALID_PARAMETER;
    }
}

/* NtEnumerateKey(KeyHandle, Index, Class, Information, Length, ResultLength). */
static long nt_enumerate_key(uint64_t h, uint64_t index, uint64_t cls, uint64_t info_u, uint64_t len,
                             uint64_t ret_u) {
    vibeos_reg_key_t k, sub;
    uint32_t st, nlen = 0, fixed = 0, total = 0;
    uint64_t lw = 0;

    if ((uint32_t)cls > KEY_FULL) {
        return NT_STATUS(VIBEOS_STATUS_INVALID_PARAMETER);
    }
    io_lock();
    reg_start();
    st = key_ref(h, KEY_ENUMERATE_SUB_KEYS, &k);
    if (st == VIBEOS_STATUS_SUCCESS) {
        st = vibeos_reg_enum_key(k, (uint32_t)index, g_vname, sizeof(g_vname), &nlen, &lw);
        if (st == VIBEOS_STATUS_SUCCESS) {
            sub = k;
            if ((uint32_t)cls == KEY_FULL) {
                st = vibeos_reg_open(k, g_vname, nlen / 2u, &sub);
            }
            if (st == VIBEOS_STATUS_SUCCESS) {
                st = key_record(sub, (uint32_t)cls, nlen, lw, &fixed, &total);
                if ((uint32_t)cls == KEY_FULL) {
                    vibeos_reg_unref(sub);
                }
            }
        }
        if (st == VIBEOS_STATUS_SUCCESS) {
            st = answer(info_u, (uint32_t)len, ret_u, fixed, total);
        }
        vibeos_reg_unref(k);
    }
    io_unlock();
    return NT_STATUS(st);
}

/* NtQueryKey(KeyHandle, Class, Information, Length, ResultLength). The name
 * class asks nothing of the handle; the rest, KEY_QUERY_VALUE. */
static long nt_query_key(uint64_t h, uint64_t cls, uint64_t info_u, uint64_t len, uint64_t ret_u) {
    vibeos_reg_key_t k;
    vibeos_reg_info_t in;
    uint32_t st, nlen = 0, fixed = 0, total = 0;

    if ((uint32_t)cls > KEY_CACHED) {
        return NT_STATUS(VIBEOS_STATUS_INVALID_PARAMETER);
    }
    io_lock();
    reg_start();
    st = key_ref(h, (uint32_t)cls == KEY_NAME ? 0u : KEY_QUERY_VALUE, &k);
    if (st == VIBEOS_STATUS_SUCCESS) {
        if ((uint32_t)cls == KEY_NAME) {
            st = vibeos_reg_path(k, (uint16_t *)(void *)(g_stage + 4u), STAGE_BYTES - 4u, &nlen);
            if (st == VIBEOS_STATUS_SUCCESS) {
                put32(0, nlen);
                fixed = 4u;
                total = 4u + nlen;
            }
        } else {
            st = vibeos_reg_name(k, g_vname, sizeof(g_vname), &nlen);
            if (st == VIBEOS_STATUS_SUCCESS) {
                st = vibeos_reg_info(k, &in);
            }
            if (st == VIBEOS_STATUS_SUCCESS) {
                st = key_record(k, (uint32_t)cls, nlen, in.last_write, &fixed, &total);
            }
        }
        if (st == VIBEOS_STATUS_SUCCESS) {
            st = answer(info_u, (uint32_t)len, ret_u, fixed, total);
        }
        vibeos_reg_unref(k);
    }
    io_unlock();
    return NT_STATUS(st);
}

#define NT_KEY_SERVICES(X) \
    X(VIBEOS_NT_NR_NtCreateKey, NtCreateKey, OPEN, \
      PTRS(OUT(0, 8), IN(2, 48), OUT_OPT(6, 4)), \
      nt_create_key(ARG(0), NT_ARG32(1), ARG(2), NT_ARG32(5), ARG(6))) \
    X(VIBEOS_NT_NR_NtOpenKey, NtOpenKey, OPEN, \
      PTRS(OUT(0, 8), IN(2, 48)), \
      nt_open_key(ARG(0), NT_ARG32(1), ARG(2))) \
    X(VIBEOS_NT_NR_NtOpenKeyEx, NtOpenKeyEx, OPEN, \
      PTRS(OUT(0, 8), IN(2, 48)), \
      nt_open_key_ex(ARG(0), NT_ARG32(1), ARG(2), NT_ARG32(3))) \
    X(VIBEOS_NT_NR_NtDeleteKey, NtDeleteKey, UNLINK_AT, NOPTR, nt_delete_key(ARG(0))) \
    X(VIBEOS_NT_NR_NtRenameKey, NtRenameKey, RENAME, \
      PTRS(IN(1, 16)), nt_rename_key(ARG(0), ARG(1))) \
    X(VIBEOS_NT_NR_NtFlushKey, NtFlushKey, FSYNC, NOPTR, nt_flush_key(ARG(0))) \
    X(VIBEOS_NT_NR_NtSetValueKey, NtSetValueKey, WRITE, \
      PTRS(IN(1, 16)), \
      nt_set_value_key(ARG(0), ARG(1), NT_ARG32(3), ARG(4), NT_ARG32(5))) \
    X(VIBEOS_NT_NR_NtQueryValueKey, NtQueryValueKey, READ, \
      PTRS(IN(1, 16), NT_OUT_BUF(3, 4), OUT_OPT(5, 4)), \
      nt_query_value_key(ARG(0), ARG(1), NT_ARG32(2), ARG(3), NT_ARG32(4), ARG(5))) \
    X(VIBEOS_NT_NR_NtEnumerateValueKey, NtEnumerateValueKey, READ, \
      PTRS(NT_OUT_BUF(3, 4), OUT_OPT(5, 4)), \
      nt_enumerate_value_key(ARG(0), NT_ARG32(1), NT_ARG32(2), ARG(3), NT_ARG32(4), ARG(5))) \
    X(VIBEOS_NT_NR_NtDeleteValueKey, NtDeleteValueKey, WRITE, \
      PTRS(IN(1, 16)), nt_delete_value_key(ARG(0), ARG(1))) \
    X(VIBEOS_NT_NR_NtEnumerateKey, NtEnumerateKey, READ, \
      PTRS(NT_OUT_BUF(3, 4), OUT_OPT(5, 4)), \
      nt_enumerate_key(ARG(0), NT_ARG32(1), NT_ARG32(2), ARG(3), NT_ARG32(4), ARG(5))) \
    X(VIBEOS_NT_NR_NtQueryKey, NtQueryKey, READ, \
      PTRS(NT_OUT_BUF(2, 3), OUT_OPT(4, 4)), \
      nt_query_key(ARG(0), NT_ARG32(1), ARG(2), NT_ARG32(3), ARG(4)))

NT_DEFINE_SERVICES(key, NT_KEY_SERVICES)

/* The registry as a reboot leaves it: whatever was pending written, and the
 * next call starts it again from its hives. For the host tests, which have no
 * reboot to ask for. */
void nt_reg_restart(void) {
    io_lock();
    if (g_started && g_pending) {
        flush_now();
    }
    vibeos_reg_fini();   /* the memory too, as a reboot gives it back */
    g_started = 0;
    g_pending = 0;
    g_no_volume = 0;
    io_unlock();
}
