/* Filesystem dispatch: mount a driver, call it through function pointers.
 *
 * Deliberately thin. Everything hard belongs to the drivers; this file exists
 * so the syscall layer stops naming one of them. Portable, so the refusal
 * cases - an unmounted volume, a driver that does not implement an operation -
 * can be tested without a disk, and those are the cases a boot never shows
 * because a booted system always has its volume.
 */

#include "vibeos/vfs.h"
#include "vibeos/mbz.h"
#include "vibeos/abi_linux.h"

int vibeos_fs_mount(vibeos_fsmount_t *mnt, const vibeos_fs_ops_t *ops,
                     void *fs, const char *type) {
    if (!mnt || !ops || !ops->lookup || !ops->read_at) {
        /* lookup and read_at are the minimum: a filesystem that cannot resolve
         * a name or read a byte is not one. Everything else may be absent, and
         * the wrappers below report that as a refusal rather than a crash. */
        return -1;
    }
    mnt->ops = ops;
    mnt->fs = fs;
    mnt->type = type;
    mnt->mounted = 1;
    return 0;
}

void vibeos_fs_unmount(vibeos_fsmount_t *mnt) {
    if (mnt) {
        mnt->mounted = 0;
        mnt->ops = 0;
        mnt->fs = 0;
    }
}

int vibeos_fs_is_mounted(const vibeos_fsmount_t *mnt) {
    return (mnt && mnt->mounted && mnt->ops) ? 1 : 0;
}

const char *vibeos_fs_type(const vibeos_fsmount_t *mnt) {
    return (mnt && mnt->mounted && mnt->type) ? mnt->type : "none";
}

/* A node as every caller may read it (L1): zeroed before the driver is asked,
 * because five drivers were written before most of these fields existed and a
 * field a driver never heard of must not carry stack garbage into stat; then
 * completed from what the driver did say. */
static void fs_node_clear(vibeos_fs_node_t *n) {
    uint8_t *p = (uint8_t *)n;
    uint32_t i;
    for (i = 0; i < sizeof(*n); i++) {
        p[i] = 0;
    }
}

static void fs_node_complete(vibeos_fs_node_t *n) {
    if ((n->mode & VIBEOS_S_IFMT) == 0u) {
        n->mode |= n->is_dir ? VIBEOS_S_IFDIR : VIBEOS_S_IFREG;
        if ((n->mode & 07777u) == 0u) {
            n->mode |= n->is_dir ? 0755u : 0644u;
        }
    }
    n->is_dir = (n->mode & VIBEOS_S_IFMT) == VIBEOS_S_IFDIR;
    if (n->nlink == 0u) {
        n->nlink = 1u;
    }
}

int vibeos_fs_lookup(vibeos_fsmount_t *mnt, const char *path,
                      vibeos_fs_node_t *out) {
    int r;

    if (!vibeos_fs_is_mounted(mnt) || !path || !out) {
        return -1;
    }
    fs_node_clear(out);
    r = mnt->ops->lookup(mnt->fs, path, out);
    if (r == 0) {
        fs_node_complete(out);
    }
    return r;
}

long vibeos_fs_read_at(vibeos_fsmount_t *mnt, const vibeos_fs_node_t *node,
                        uint64_t offset, void *buf, uint32_t len) {
    if (!vibeos_fs_is_mounted(mnt) || !node || !buf) {
        return -1;
    }
    return mnt->ops->read_at(mnt->fs, node, offset, buf, len);
}

long vibeos_fs_write_file(vibeos_fsmount_t *mnt, const char *path,
                           const void *buf, uint32_t len) {
    if (!vibeos_fs_is_mounted(mnt) || !path || !buf) {
        return -1;
    }
    if (!mnt->ops->write_file) {
        return -1;   /* a read-only filesystem, and saying so is the answer */
    }
    return mnt->ops->write_file(mnt->fs, path, buf, len);
}

int vibeos_fs_list(vibeos_fsmount_t *mnt, const char *path, uint32_t index,
                    char *name, uint32_t name_cap, uint64_t *out_size,
                    int *out_is_dir) {
    if (!vibeos_fs_is_mounted(mnt) || !path || !name || name_cap == 0u) {
        return -1;
    }
    if (!mnt->ops->list) {
        return -1;
    }
    return mnt->ops->list(mnt->fs, path, index, name, name_cap, out_size, out_is_dir);
}

/* ---- what happened, for whoever listens (docs/abi/ L4 step 6) ------------------- */

static vibeos_fs_notify_fn g_notify;
static uint32_t g_notify_cookie;

void vibeos_fs_set_notify(vibeos_fs_notify_fn fn) {
    __atomic_store_n(&g_notify, fn, __ATOMIC_RELEASE);
}

int vibeos_fs_notify_active(void) {
    return __atomic_load_n(&g_notify, __ATOMIC_ACQUIRE) != 0;
}

void vibeos_fs_notify(vibeos_fsmount_t *mnt, const char *path, uint64_t id, uint32_t event, uint32_t cookie,
                      int is_dir) {
    vibeos_fs_notify_fn fn = __atomic_load_n(&g_notify, __ATOMIC_ACQUIRE);

    if (fn && mnt && path) {
        fn(mnt, path, id, event, cookie, is_dir);
    }
}

/* The node at `path` before an operation changes it, when somebody listens;
 * found is 0 otherwise, and nothing is looked up. */
static int fs_peek(vibeos_fsmount_t *mnt, const char *path, vibeos_fs_node_t *out) {
    return vibeos_fs_notify_active() && vibeos_fs_lookup(mnt, path, out) == 0;
}

/* A name gone: the node's link count changed, its directory lost an entry, and
 * if that was its last name the node itself is gone. */
static void fs_notify_gone(vibeos_fsmount_t *mnt, const char *path, const vibeos_fs_node_t *n) {
    if (!n->is_dir) {
        vibeos_fs_notify(mnt, path, n->id, VIBEOS_FSN_NLINK, 0, 0);
    }
    vibeos_fs_notify(mnt, path, n->id, VIBEOS_FSN_DELETE, 0, n->is_dir);
    if (n->is_dir || n->nlink <= 1u) {
        vibeos_fs_notify(mnt, path, n->id, VIBEOS_FSN_DELETE_SELF, 0, n->is_dir);
    }
}

/* A name made: its directory gained an entry. */
static void fs_notify_made(vibeos_fsmount_t *mnt, const char *path) {
    vibeos_fs_node_t n;

    if (fs_peek(mnt, path, &n)) {
        vibeos_fs_notify(mnt, path, n.id, VIBEOS_FSN_CREATE, 0, n.is_dir);
    }
}

int vibeos_fs_unlink(vibeos_fsmount_t *mnt, const char *path) {
    vibeos_fs_node_t n;
    int had, r;

    if (!vibeos_fs_is_mounted(mnt) || !path || !mnt->ops->unlink) {
        return -1;
    }
    had = fs_peek(mnt, path, &n);
    r = mnt->ops->unlink(mnt->fs, path);
    if (r == 0 && had) {
        fs_notify_gone(mnt, path, &n);
    }
    return r;
}

int vibeos_fs_mkdir(vibeos_fsmount_t *mnt, const char *path) {
    int r;

    if (!vibeos_fs_is_mounted(mnt) || !path || !mnt->ops->mkdir) {
        return -1;
    }
    r = mnt->ops->mkdir(mnt->fs, path);
    if (r == 0) {
        fs_notify_made(mnt, path);
    }
    return r;
}

/* ---- L1's operations ----------------------------------------------------------- */

int vibeos_fs_writable(const vibeos_fsmount_t *mnt) {
    return vibeos_fs_is_mounted(mnt) &&
           (mnt->ops->write_file || mnt->ops->write_at || mnt->ops->create);
}

/* What a missing operation means: a filesystem that writes nothing is
 * read-only, one that writes but lacks this cannot represent it. */
static int fs_missing(const vibeos_fsmount_t *mnt) {
    return vibeos_fs_writable(mnt) ? -VIBEOS_EPERM : -VIBEOS_EROFS;
}

long vibeos_fs_write_at(vibeos_fsmount_t *mnt, const vibeos_fs_node_t *node,
                         uint64_t offset, const void *buf, uint32_t len) {
    if (!vibeos_fs_is_mounted(mnt) || !node || (!buf && len)) {
        return -VIBEOS_EINVAL;
    }
    if (!mnt->ops->write_at) {
        return vibeos_fs_writable(mnt) ? -VIBEOS_EOPNOTSUPP : -VIBEOS_EROFS;
    }
    return mnt->ops->write_at(mnt->fs, node, offset, buf, len);
}

int vibeos_fs_truncate(vibeos_fsmount_t *mnt, const vibeos_fs_node_t *node, uint64_t size) {
    if (!vibeos_fs_is_mounted(mnt) || !node) {
        return -VIBEOS_EINVAL;
    }
    if (!mnt->ops->truncate) {
        return vibeos_fs_writable(mnt) ? -VIBEOS_EOPNOTSUPP : -VIBEOS_EROFS;
    }
    return mnt->ops->truncate(mnt->fs, node, size);
}

int vibeos_fs_share_page(vibeos_fsmount_t *mnt, const vibeos_fs_node_t *node, uint64_t offset,
                         void **page) {
    if (!vibeos_fs_is_mounted(mnt) || !node || !page || (offset & 0xFFFull) != 0u) {
        return -VIBEOS_EINVAL;
    }
    if (!mnt->ops->share_page) {
        return -VIBEOS_ENODEV;
    }
    return mnt->ops->share_page(mnt->fs, node, offset, page);
}

int vibeos_fs_create(vibeos_fsmount_t *mnt, const char *path, uint32_t mode,
                     vibeos_fs_node_t *out) {
    int r;

    if (!vibeos_fs_is_mounted(mnt) || !path || !out) {
        return -VIBEOS_EINVAL;
    }
    if (!mnt->ops->create) {
        return vibeos_fs_writable(mnt) ? -VIBEOS_EOPNOTSUPP : -VIBEOS_EROFS;
    }
    fs_node_clear(out);
    r = mnt->ops->create(mnt->fs, path, mode, out);
    if (r == 0) {
        fs_node_complete(out);
        vibeos_fs_notify(mnt, path, out->id, VIBEOS_FSN_CREATE, 0, out->is_dir);
    }
    return r;
}

int vibeos_fs_rmdir(vibeos_fsmount_t *mnt, const char *path) {
    vibeos_fs_node_t n;
    int had, r;

    if (!vibeos_fs_is_mounted(mnt) || !path) {
        return -VIBEOS_EINVAL;
    }
    if (!mnt->ops->rmdir) {
        return fs_missing(mnt);
    }
    had = fs_peek(mnt, path, &n);
    r = mnt->ops->rmdir(mnt->fs, path);
    if (r == 0 && had) {
        fs_notify_gone(mnt, path, &n);
    }
    return r;
}

/* A rename is two records with one cookie, the source's then the target's, and
 * the node's own MOVE_SELF; a node the target replaced is gone. */
int vibeos_fs_rename(vibeos_fsmount_t *mnt, const char *from, const char *to, uint32_t flags) {
    vibeos_fs_node_t n, old;
    int had, had_old, r;

    if (!vibeos_fs_is_mounted(mnt) || !from || !to || (flags & ~VIBEOS_RENAME_NOREPLACE)) {
        return -VIBEOS_EINVAL;
    }
    if (!mnt->ops->rename) {
        return fs_missing(mnt);
    }
    had = fs_peek(mnt, from, &n);
    had_old = fs_peek(mnt, to, &old);
    r = mnt->ops->rename(mnt->fs, from, to, flags);
    if (r == 0 && had) {
        uint32_t cookie = __atomic_add_fetch(&g_notify_cookie, 1u, __ATOMIC_ACQ_REL);

        if (cookie == 0u) {
            cookie = __atomic_add_fetch(&g_notify_cookie, 1u, __ATOMIC_ACQ_REL);   /* 0 is "no pair" */
        }
        vibeos_fs_notify(mnt, from, n.id, VIBEOS_FSN_MOVED_FROM, cookie, n.is_dir);
        vibeos_fs_notify(mnt, to, n.id, VIBEOS_FSN_MOVED_TO, cookie, n.is_dir);
        vibeos_fs_notify(mnt, to, n.id, VIBEOS_FSN_MOVE_SELF, 0, n.is_dir);
        if (had_old && old.id != n.id && (old.is_dir || old.nlink <= 1u)) {
            vibeos_fs_notify(mnt, to, old.id, VIBEOS_FSN_DELETE_SELF, 0, old.is_dir);
        }
    }
    return r;
}

int vibeos_fs_link(vibeos_fsmount_t *mnt, const char *existing, const char *path) {
    int r;

    if (!vibeos_fs_is_mounted(mnt) || !existing || !path) {
        return -VIBEOS_EINVAL;
    }
    if (!mnt->ops->link) {
        return fs_missing(mnt);
    }
    r = mnt->ops->link(mnt->fs, existing, path);
    if (r == 0 && vibeos_fs_notify_active()) {
        vibeos_fs_node_t n;

        if (vibeos_fs_lookup(mnt, path, &n) == 0) {
            vibeos_fs_notify(mnt, existing, n.id, VIBEOS_FSN_NLINK, 0, 0);
            vibeos_fs_notify(mnt, path, n.id, VIBEOS_FSN_CREATE, 0, 0);
        }
    }
    return r;
}

int vibeos_fs_symlink(vibeos_fsmount_t *mnt, const char *target, const char *path) {
    int r;

    if (!vibeos_fs_is_mounted(mnt) || !target || !path) {
        return -VIBEOS_EINVAL;
    }
    if (!mnt->ops->symlink) {
        return fs_missing(mnt);
    }
    r = mnt->ops->symlink(mnt->fs, target, path);
    if (r == 0) {
        fs_notify_made(mnt, path);
    }
    return r;
}

long vibeos_fs_readlink(vibeos_fsmount_t *mnt, const char *path, char *buf, uint32_t cap) {
    if (!vibeos_fs_is_mounted(mnt) || !path || !buf) {
        return -VIBEOS_EINVAL;
    }
    return mnt->ops->readlink ? mnt->ops->readlink(mnt->fs, path, buf, cap) : -VIBEOS_EINVAL;
}

int vibeos_fs_setattr(vibeos_fsmount_t *mnt, const char *path, const vibeos_fs_attr_t *attr) {
    vibeos_fs_node_t n;
    int r;

    if (!vibeos_fs_is_mounted(mnt) || !path || !attr) {
        return -VIBEOS_EINVAL;
    }
    if (!mnt->ops->setattr) {
        return fs_missing(mnt);
    }
    r = mnt->ops->setattr(mnt->fs, path, attr);
    if (r == 0 && fs_peek(mnt, path, &n)) {
        vibeos_fs_notify(mnt, path, n.id, VIBEOS_FSN_ATTRIB, 0, n.is_dir);
    }
    return r;
}

int vibeos_fs_statfs(vibeos_fsmount_t *mnt, vibeos_fs_statfs_t *out) {
    uint8_t *p = (uint8_t *)out;
    uint32_t i;

    if (!vibeos_fs_is_mounted(mnt) || !out) {
        return -VIBEOS_EINVAL;
    }
    for (i = 0; i < sizeof(*out); i++) {
        p[i] = 0;
    }
    out->block_size = 512u;
    out->name_max = 255u;
    out->read_only = !vibeos_fs_writable(mnt);
    return mnt->ops->statfs ? mnt->ops->statfs(mnt->fs, out) : 0;
}

int vibeos_fs_sync(vibeos_fsmount_t *mnt) {
    if (!vibeos_fs_is_mounted(mnt)) {
        return -VIBEOS_EINVAL;
    }
    return mnt->ops->sync ? mnt->ops->sync(mnt->fs) : 0;
}

static uint64_t (*g_now_ns)(void);

void vibeos_fs_set_clock(uint64_t (*now_ns)(void)) {
    g_now_ns = now_ns;
}

uint64_t vibeos_fs_now_ns(void) {
    return g_now_ns ? g_now_ns() : 0u;
}

long vibeos_fs_read_file(vibeos_fsmount_t *mnt, const char *path,
                          void *buf, uint32_t cap) {
    vibeos_fs_node_t node;
    uint64_t done = 0;

    if (!vibeos_fs_is_mounted(mnt) || !path || !buf) {
        return -1;
    }
    if (mnt->ops->lookup(mnt->fs, path, &node) != 0) {
        return -1;
    }
    if (node.is_dir || node.size > (uint64_t)cap) {
        return -1;
    }
    while (done < node.size) {
        uint64_t want = node.size - done;
        long got;
        if (want > 0xFFFFFFFFull) {
            want = 0xFFFFFFFFull;
        }
        got = mnt->ops->read_at(mnt->fs, &node, done, (uint8_t *)buf + done,
                                (uint32_t)want);
        if (got <= 0) {
            /* Short of the declared size is a failure, not a smaller file.
             * Returning what was copied here is precisely how a truncated
             * program came to be handed to execve as if it were whole. */
            return -1;
        }
        done += (uint64_t)got;
    }
    return (long)done;
}

/* ---- the mount table (I4b step 4) ----------------------------------------
 *
 * See include/vibeos/vfs.h for why this is a fixed array, why resolution is
 * longest-prefix, and why a second claim on one path is refused rather than
 * allowed to overwrite.
 */

typedef struct {
    char path[VIBEOS_FS_MOUNT_PATH_MAX];
    uint32_t path_len;
    vibeos_fsmount_t *mnt;
} fs_attach_t;

static fs_attach_t g_mounts[VIBEOS_FS_MOUNTS_MAX];
static uint32_t g_mount_count;
static void (*g_lock)(void);
static void (*g_unlock)(void);

void vibeos_fs_set_lock(void (*lock)(void), void (*unlock)(void)) {
    g_lock = lock;
    g_unlock = unlock;
}

/* See vibeos_fs_set_lock in vfs.h. With none registered - the host tests, or an
 * architecture that forgot - the call still runs, and says so. */
static void mt_lock(void) {
    if (g_lock) {
        g_lock();
    } else {
        vibeos_mbz_hit(VIBEOS_MBZ_MOUNT_UNLOCKED, 0u);
    }
}

static void mt_unlock(void) {
    if (g_unlock) {
        g_unlock();
    }
}

static uint32_t fs_strlen(const char *s) {
    uint32_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

static int fs_same(const char *a, const char *b, uint32_t n) {
    uint32_t i;
    for (i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

int vibeos_fs_attach(const char *path, vibeos_fsmount_t *mnt) {
    uint32_t len, i;

    if (!path || !mnt || path[0] != '/') {
        return -1;
    }
    len = fs_strlen(path);
    if (len == 0u || len >= VIBEOS_FS_MOUNT_PATH_MAX) {
        return -1;
    }
    /* A trailing slash on anything but the root would make "/usr/" and "/usr"
     * two different mounts of the same directory, and the resolver below would
     * then answer differently for the same path depending on which was
     * registered. One spelling, checked here. */
    if (len > 1u && path[len - 1u] == '/') {
        return -1;
    }
    mt_lock();
    for (i = 0; i < g_mount_count; i++) {
        if (g_mounts[i].path_len == len && fs_same(g_mounts[i].path, path, len)) {
            mt_unlock();
            return -1;   /* taken; see the header on why not overwritten */
        }
    }
    if (g_mount_count >= VIBEOS_FS_MOUNTS_MAX) {
        mt_unlock();
        return -1;
    }
    for (i = 0; i < len; i++) {
        g_mounts[g_mount_count].path[i] = path[i];
    }
    g_mounts[g_mount_count].path[len] = 0;
    g_mounts[g_mount_count].path_len = len;
    g_mounts[g_mount_count].mnt = mnt;
    g_mount_count++;
    mt_unlock();
    return 0;
}

int vibeos_fs_detach(const char *path) {
    uint32_t len, i;

    if (!path) {
        return -1;
    }
    len = fs_strlen(path);
    mt_lock();
    for (i = 0; i < g_mount_count; i++) {
        if (g_mounts[i].path_len == len && fs_same(g_mounts[i].path, path, len)) {
            /* The last entry moves into the hole. Order carries no meaning
             * here precisely because resolution is longest-prefix rather than
             * first-match, which is what makes that safe - for a reader that
             * cannot see the move half done, which is what the lock is for. */
            g_mounts[i] = g_mounts[g_mount_count - 1u];
            g_mount_count--;
            mt_unlock();
            return 0;
        }
    }
    mt_unlock();
    return -1;
}

int vibeos_fs_resolve(const char *path, vibeos_fsmount_t **out_mnt,
                      const char **out_tail) {
    uint32_t i;
    int best = -1;
    uint32_t best_len = 0;

    if (!path || !out_mnt || !out_tail || path[0] != '/') {
        return -1;
    }
    mt_lock();
    for (i = 0; i < g_mount_count; i++) {
        uint32_t n = g_mounts[i].path_len;
        if (!fs_same(g_mounts[i].path, path, n)) {
            continue;
        }
        /* The prefix has to end on a boundary. Without this "/usrlocal" would
         * match a mount at "/usr", which is a wrong answer that looks entirely
         * reasonable in a log. The root is the exception: it ends on '/'
         * already. */
        if (n > 1u && path[n] != 0 && path[n] != '/') {
            continue;
        }
        if (best < 0 || n > best_len) {
            best = (int)i;
            best_len = n;
        }
    }
    if (best < 0) {
        mt_unlock();
        return -1;
    }
    *out_mnt = g_mounts[best].mnt;
    mt_unlock();
    /* What is left, without the mount's own prefix and without a leading
     * slash: a driver is handed a path relative to its own root, which is what
     * lets one driver be mounted in two places. */
    {
        const char *tail = path + best_len;
        while (*tail == '/') {
            tail++;
        }
        *out_tail = tail;
    }
    return 0;
}

uint32_t vibeos_fs_mount_count(void) {
    uint32_t n;

    mt_lock();
    n = g_mount_count;
    mt_unlock();
    return n;
}

/* For reporting. The pointer names the entry's own storage, which a later
 * detach may reuse - read it before anything else changes the table. */
const char *vibeos_fs_mount_path(uint32_t index) {
    const char *p;

    mt_lock();
    p = (index < g_mount_count) ? g_mounts[index].path : 0;
    mt_unlock();
    return p;
}

vibeos_fsmount_t *vibeos_fs_mount_at(uint32_t index) {
    vibeos_fsmount_t *m;

    mt_lock();
    m = (index < g_mount_count) ? g_mounts[index].mnt : 0;
    mt_unlock();
    return m;
}

void vibeos_fs_detach_all(void) {
    mt_lock();
    g_mount_count = 0;
    mt_unlock();
}
