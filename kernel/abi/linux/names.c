/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Linux ABI: names and what is known about a file (docs/abi/ L1 step 5).
 *
 * rename, rmdir, link and symlink; statx, access, statfs; the chmod, chown and
 * utime families; mknod and openat2. Each is the ABI's half of an operation a
 * filesystem implements (vibeos/vfs.h): walk the path, refuse what Linux refuses
 * before any filesystem is asked - a rename across two mounts, a hard link to a
 * directory, the removal of a mount's root - and translate the answer.
 *
 * What a filesystem cannot do it says itself, through the wrappers: EROFS from
 * one that writes nothing, EPERM from one that writes and has nowhere to put the
 * thing (a symbolic link on FAT, an owner other than root). Nothing here asks
 * which filesystem it is talking to.
 *
 * There is one user, root, until L2 gives a process credentials. So no call
 * here refuses for want of permission, and access() answers as it does for
 * root on Linux: everything but writing on a read-only filesystem and running
 * a file nobody may run. */

#include "linux_internal.h"

#define LINUX_CWD ((uint64_t)(uint32_t)LINUX_AT_FDCWD)

/* The path a filesystem is given: inside its mount, and "/" for its root. */
static const char *linux_tail(const vibeos_path_t *w) {
    return *w->tail ? w->tail : "/";
}

/* A change of attributes, to the node a descriptor holds when the walk came
 * from one (M-080), to the name otherwise. */
static long linux_setattr_walked(const vibeos_path_t *w, const vibeos_fs_attr_t *attr) {
    long r;

    if (w->by_node) {
        return vibeos_fs_setattr_node(w->mnt, &w->node, linux_tail(w), attr);
    }
    /* The node the walk found and the permission was judged on, not what
     * the name holds by the time this runs: a chmod of one's own file
     * landed on a file swapped in under its name (issue #195). Only a
     * filesystem that cannot be asked by node is asked by name. */
    if (w->exists) {
        r = vibeos_fs_setattr_node(w->mnt, &w->node, linux_tail(w), attr);
        if (r != -VIBEOS_ENOSYS) {
            return r;
        }
    }
    return vibeos_fs_setattr(w->mnt, linux_tail(w), attr);
}

static int linux_same_path(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/* ---- statx --------------------------------------------------------------------------- */

static void linux_statx_time(linux_statx_timestamp_t *t, uint64_t ns) {
    t->tv_sec = (int64_t)(ns / 1000000000ull);
    t->tv_nsec = (uint32_t)(ns % 1000000000ull);
}

/* statx(dirfd, path, flags, mask, buf): stat with a mask of what is wanted and
 * a mask of what was given. What is given is always what stat reports - the
 * basic set - whatever was asked for: Linux says a filesystem may return more
 * or less than the request, and the caller reads stx_mask. There is no birth
 * time on any filesystem here, and its bit stays clear. */
static long linux_sys_statx(uint64_t dirfd, uint64_t path_uptr, uint64_t flags, uint64_t mask,
                            uint64_t ubuf) {
    vibeos_file_stat_t st;
    linux_statx_t k;
    uint8_t *raw = (uint8_t *)&k;
    uint64_t dev = 0;
    uint32_t i;
    long r;

    if (flags & ~(uint64_t)(LINUX_AT_EMPTY_PATH | LINUX_AT_SYMLINK_NOFOLLOW |
                            LINUX_AT_NO_AUTOMOUNT | LINUX_AT_STATX_SYNC_TYPE)) {
        return -VIBEOS_EINVAL;
    }
    if ((flags & LINUX_AT_STATX_SYNC_TYPE) == LINUX_AT_STATX_SYNC_TYPE) {
        return -VIBEOS_EINVAL;   /* force-sync and don't-sync at once */
    }
    if (mask & LINUX_STATX_RESERVED) {
        return -VIBEOS_EINVAL;
    }
    r = linux_stat_get(dirfd, path_uptr, flags, &st, &dev);
    if (r < 0) {
        return r;
    }
    for (i = 0; i < sizeof(k); i++) {
        raw[i] = 0;   /* padding and everything Linux has added since: copied out */
    }
    k.stx_mask = LINUX_STATX_BASIC_STATS;
    k.stx_blksize = 512u;
    k.stx_nlink = st.nlink || st.unlinked ? st.nlink : 1u;
    k.stx_uid = st.uid;
    k.stx_gid = st.gid;
    k.stx_mode = (uint16_t)st.mode;
    k.stx_ino = st.ino;
    k.stx_size = st.size;
    k.stx_blocks = vibeos_ceil_div_u64(st.size, 512ull);
    linux_statx_time(&k.stx_atime, st.atime_ns);
    linux_statx_time(&k.stx_ctime, st.ctime_ns);
    linux_statx_time(&k.stx_mtime, st.mtime_ns);
    k.stx_dev_minor = (uint32_t)dev;
    k.stx_rdev_major = st.rdev >> 8;
    k.stx_rdev_minor = st.rdev & 0xffu;
    if (vibeos_uaccess_copy((void *)(uintptr_t)ubuf, &k, sizeof(k)) != 0) {
        return -VIBEOS_EFAULT;
    }
    return 0;
}

/* ---- access -------------------------------------------------------------------------- */

/* access, faccessat and faccessat2: may the caller read, write or run it? The
 * answer is for the *real* identity - "could the user who ran this program do
 * it?", which is what a program running with borrowed privileges needs to
 * know before it uses them - unless AT_EACCESS asks about the one the call
 * would actually be judged by. Writing is also refused on a filesystem that
 * writes nothing (EROFS). */
static long linux_access_at(uint64_t dirfd, uint64_t path_uptr, uint64_t mode, uint64_t flags) {
    vibeos_path_t w;
    long r;

    if (mode & ~(uint64_t)(LINUX_R_OK | LINUX_W_OK | LINUX_X_OK)) {
        return -VIBEOS_EINVAL;
    }
    if (flags & ~(uint64_t)(LINUX_AT_EACCESS | LINUX_AT_SYMLINK_NOFOLLOW | LINUX_AT_EMPTY_PATH)) {
        return -VIBEOS_EINVAL;
    }
    r = linux_walk_at_empty(dirfd, path_uptr, flags,
                            (flags & LINUX_AT_SYMLINK_NOFOLLOW) ? VIBEOS_PATH_NOFOLLOW : 0u, &w);
    if (r != 0) {
        return r;
    }
    if ((mode & LINUX_W_OK) && !vibeos_fs_writable(w.mnt)) {
        return -VIBEOS_EROFS;
    }
    {
        vibeos_cred_t c;
        uint32_t want = ((mode & LINUX_R_OK) ? VIBEOS_MAY_READ : 0u) |
                        ((mode & LINUX_W_OK) ? VIBEOS_MAY_WRITE : 0u) |
                        ((mode & LINUX_X_OK) ? VIBEOS_MAY_EXEC : 0u);
        linux_cred(&c);
        if (flags & LINUX_AT_EACCESS) {
            c.uid = c.fsuid;   /* judged as an open would be */
            c.gid = c.fsgid;
        }
        return vibeos_cred_may(&c, 1, w.node.mode, w.node.uid, w.node.gid, want);
    }
}

/* ---- rename -------------------------------------------------------------------------- */

/* Is `path` inside the directory `dir`? Both absolute and normal. */
static int linux_path_inside(const char *dir, const char *path) {
    while (*dir && *dir == *path) {
        dir++;
        path++;
    }
    return *dir == 0 && *path == '/';
}

/* rename, renameat and renameat2. What Linux decides before a filesystem is
 * asked is decided here, so every filesystem answers alike: the two names on
 * one mount (EXDEV), neither a mount's root (EBUSY), a directory only over a
 * directory and a file only over a file, a directory never into itself.
 * RENAME_EXCHANGE and RENAME_WHITEOUT are EINVAL, which is what Linux answers
 * from a filesystem that has neither. */
static long linux_rename_at(uint64_t olddir, uint64_t old_uptr, uint64_t newdir, uint64_t new_uptr,
                            uint64_t flags) {
    vibeos_path_t a, b;
    long r;

    if (flags & ~(uint64_t)(LINUX_RENAME_NOREPLACE | LINUX_RENAME_EXCHANGE | LINUX_RENAME_WHITEOUT)) {
        return -VIBEOS_EINVAL;
    }
    if (flags & (LINUX_RENAME_EXCHANGE | LINUX_RENAME_WHITEOUT)) {
        return -VIBEOS_EINVAL;
    }
    /* The names themselves move: a symbolic link is renamed, not followed. */
    r = linux_walk_at(olddir, old_uptr, VIBEOS_PATH_NOFOLLOW, &a);
    if (r != 0) {
        return r;
    }
    r = linux_walk_at(newdir, new_uptr, VIBEOS_PATH_CREATE | VIBEOS_PATH_NOFOLLOW, &b);
    if (r != 0) {
        return r;
    }
    if (a.mnt != b.mnt) {
        return -VIBEOS_EXDEV;
    }
    if (*a.tail == 0 || *b.tail == 0) {
        return -VIBEOS_EBUSY;
    }
    if (a.node.is_dir && linux_path_inside(a.path, b.path)) {
        return -VIBEOS_EINVAL;
    }
    if (b.exists) {
        if (flags & LINUX_RENAME_NOREPLACE) {
            return -VIBEOS_EEXIST;
        }
        if (linux_same_path(a.path, b.path)) {
            return 0;   /* onto itself: nothing to do, and no error */
        }
        if (a.node.is_dir && !b.node.is_dir) {
            return -VIBEOS_ENOTDIR;
        }
        if (!a.node.is_dir && b.node.is_dir) {
            return -VIBEOS_EISDIR;
        }
    } else if (b.trailing_slash && !a.node.is_dir) {
        return -VIBEOS_ENOTDIR;   /* "new/" promises a directory */
    }
    if ((r = linux_may_remove(&a)) != 0 || (r = b.exists ? linux_may_remove(&b) : linux_may_add(&b)) != 0) {
        return r;
    }
    r = vibeos_fs_rename(a.mnt, a.tail, b.tail,
                         (flags & LINUX_RENAME_NOREPLACE) ? VIBEOS_RENAME_NOREPLACE : 0u);
    if (r == 0 && a.node.is_dir) {
        linux_paths_moved(a.path, b.path);
    } else if (r == 0) {
        /* A file's descriptions follow it too: one left naming the old path
         * removed whatever came to be there on a delete-on-close (v7 review). */
        vibeos_file_paths_moved(a.path, b.path);
    }
    return r;
}

/* A path that named something inside `from` names it inside `to` now; 0 if it
 * did not, or the new one would not fit. */
static int linux_path_moved(char *p, uint32_t cap, const char *from, const char *to) {
    char out[VIBEOS_PATH_MAX];
    uint32_t n = 0, k = 0;

    while (from[n] && from[n] == p[n]) {
        n++;
    }
    if (from[n] != 0 || (p[n] != 0 && p[n] != '/')) {
        return 0;
    }
    while (to[k] && k + 1u < sizeof(out)) {
        out[k] = to[k];
        k++;
    }
    while (p[n] && k + 1u < sizeof(out)) {
        out[k++] = p[n++];
    }
    if (p[n] != 0 || k + 1u > cap) {
        return 0;
    }
    out[k] = 0;
    for (n = 0; n <= k; n++) {
        p[n] = out[n];
    }
    return 1;
}

/* A directory renamed: the working directories and roots inside it, and the
 * paths of descriptions open under it, follow it. This kernel keeps where a
 * process stands as a path, and Linux as the directory itself - so until this,
 * a process that renamed its own working directory had every relative name
 * fail (LTP's inotify02, whose own cleanup then failed the same way). */
void linux_paths_moved(const char *from, const char *to) {
    uint32_t s;

    for (s = 0; s < ks_slots(); s++) {
        vibeos_procstate_t *ps;

        /* A reference taken where the slot cannot change hands, as every
         * other reader of another task's state does: a bare ks_ps() could be
         * a state whose last thread has since gone and whose slot a new
         * process now holds - and this would rewrite that process's cwd
         * under a lock its creation was initialising (issue #196). */
        ks_lock(ks_sched_lock(), __func__);
        ps = linux_ps_hold((int)s);
        ks_unlock(ks_sched_lock());
        if (!ps) {
            continue;
        }
        ks_lock(&ps->files_lock, __func__);
        (void)linux_path_moved(ps->cwd, sizeof(ps->cwd), from, to);
        (void)linux_path_moved(ps->root, sizeof(ps->root), from, to);
        ks_unlock(&ps->files_lock);
        ks_procstate_put(ps);
    }
    vibeos_file_paths_moved(from, to);
}

/* ---- rmdir --------------------------------------------------------------------------- */

/* rmdir(path), and unlinkat's AT_REMOVEDIR. The walk resolves "." and "..", so
 * what the program wrote as its last component is read from the string: Linux
 * refuses "." as EINVAL - a directory is removed by its name in its parent -
 * and ".." as ENOTEMPTY. */
long linux_rmdir_at(uint64_t dirfd, uint64_t path_uptr) {
    char raw[VIBEOS_PATH_MAX + 1u];
    vibeos_path_t w;
    uint32_t n = 0, s;
    long r;

    r = linux_walk_at(dirfd, path_uptr, VIBEOS_PATH_NOFOLLOW, &w);
    if (r != 0) {
        return r;
    }
    if (!w.node.is_dir) {
        return -VIBEOS_ENOTDIR;
    }
    if (ks_copy_user_string(path_uptr, raw, (int)VIBEOS_PATH_MAX + 1) != 0) {
        return -VIBEOS_EFAULT;
    }
    while (raw[n]) {
        n++;
    }
    while (n > 0u && raw[n - 1u] == '/') {
        n--;
    }
    for (s = n; s > 0u && raw[s - 1u] != '/'; s--) {
    }
    if (n - s == 1u && raw[s] == '.') {
        return -VIBEOS_EINVAL;
    }
    if (n - s == 2u && raw[s] == '.' && raw[s + 1u] == '.') {
        return -VIBEOS_ENOTEMPTY;
    }
    if (*w.tail == 0) {
        return -VIBEOS_EBUSY;   /* a mount's root, the process's root among them */
    }
    {
        long may = linux_may_remove(&w);
        if (may != 0) {
            return may;
        }
    }
    return vibeos_fs_rmdir(w.mnt, w.tail);
}

/* ---- links --------------------------------------------------------------------------- */

/* link and linkat: a second name for a file. Never for a directory (EPERM),
 * never across mounts (EXDEV). The existing name is not followed unless
 * AT_SYMLINK_FOLLOW says so, which is Linux's default and not POSIX's. */
static long linux_link_at(uint64_t olddir, uint64_t old_uptr, uint64_t newdir, uint64_t new_uptr,
                          uint64_t flags) {
    vibeos_path_t a, b;
    long r;

    if (flags & ~(uint64_t)(LINUX_AT_SYMLINK_FOLLOW | LINUX_AT_EMPTY_PATH)) {
        return -VIBEOS_EINVAL;
    }
    /* A name for what a descriptor holds is CAP_DAC_READ_SEARCH's, as Linux
     * has it: a descriptor passed on, or one opened before a directory above
     * it was closed off, would otherwise give the file a new name anybody can
     * reach (issue #180). Linux answers ENOENT. */
    if ((flags & LINUX_AT_EMPTY_PATH) != 0u) {
        vibeos_cred_t me;

        linux_cred(&me);
        if (me.euid != 0u) {
            return -VIBEOS_ENOENT;
        }
    }
    r = linux_walk_at_empty(olddir, old_uptr, flags,
                            (flags & LINUX_AT_SYMLINK_FOLLOW) ? 0u : VIBEOS_PATH_NOFOLLOW, &a);
    if (r != 0) {
        return r;
    }
    if (a.node.is_dir) {
        return -VIBEOS_EPERM;
    }
    r = linux_walk_at(newdir, new_uptr, VIBEOS_PATH_CREATE | VIBEOS_PATH_NOFOLLOW, &b);
    if (r != 0) {
        return r;
    }
    if (b.exists) {
        return -VIBEOS_EEXIST;
    }
    if (a.mnt != b.mnt) {
        return -VIBEOS_EXDEV;
    }
    if ((r = linux_may_add(&b)) != 0) {
        return r;
    }
    /* A link is made by name. A descriptor whose name now reaches something
     * else would link that instead (issue #182). */
    if (a.by_node && a.name_stale) {
        return -VIBEOS_ENOENT;
    }
    return vibeos_fs_link(a.mnt, a.tail, b.tail);
}

/* symlink and symlinkat: a name whose contents are a path, kept as written and
 * resolved only when somebody walks through it - so the target need not exist,
 * and is not looked at here beyond its length. */
static long linux_symlink_at(uint64_t target_uptr, uint64_t newdir, uint64_t new_uptr) {
    char target[VIBEOS_PATH_MAX + 1u];
    vibeos_path_t w;
    uint32_t n = 0;
    long r;

    if (ks_copy_user_string(target_uptr, target, (int)VIBEOS_PATH_MAX + 1) != 0) {
        return -VIBEOS_EFAULT;
    }
    while (target[n]) {
        n++;
    }
    if (n == 0u) {
        return -VIBEOS_ENOENT;
    }
    if (n >= VIBEOS_PATH_MAX) {
        return -VIBEOS_ENAMETOOLONG;
    }
    r = linux_walk_at(newdir, new_uptr, VIBEOS_PATH_CREATE | VIBEOS_PATH_NOFOLLOW, &w);
    if (r != 0) {
        return r;
    }
    if (w.exists) {
        return -VIBEOS_EEXIST;
    }
    if ((r = linux_may_add(&w)) != 0) {
        return r;
    }
    r = vibeos_fs_symlink(w.mnt, target, w.tail);
    if (r == 0) {
        linux_own_new(&w);
    }
    return r;
}

/* ---- mknod and openat2 --------------------------------------------------------------- */

/* mknod and mknodat. A regular file is made, which is what `mknod name` with
 * no type and S_IFREG both ask for. A FIFO, a socket or a device node is EPERM:
 * no filesystem here has anywhere to keep one, and a name that looked like a
 * FIFO and opened as an empty file would be worse than a refusal. */
static long linux_mknod_at(uint64_t dirfd, uint64_t path_uptr, uint64_t mode) {
    vibeos_path_t w;
    vibeos_fs_node_t node;
    uint32_t type = (uint32_t)mode & VIBEOS_S_IFMT;
    long r;

    if (type == VIBEOS_S_IFDIR) {
        return -VIBEOS_EPERM;    /* mkdir makes directories */
    }
    if (type != 0u && type != VIBEOS_S_IFREG && type != VIBEOS_S_IFIFO &&
        type != VIBEOS_S_IFSOCK && type != VIBEOS_S_IFCHR && type != VIBEOS_S_IFBLK) {
        return -VIBEOS_EINVAL;
    }
    r = linux_walk_at(dirfd, path_uptr, VIBEOS_PATH_CREATE | VIBEOS_PATH_NOFOLLOW, &w);
    if (r != 0) {
        return r;
    }
    if (w.exists) {
        return -VIBEOS_EEXIST;
    }
    if (type != 0u && type != VIBEOS_S_IFREG && type != VIBEOS_S_IFSOCK) {
        return -VIBEOS_EPERM;
    }
    if ((r = linux_may_add(&w)) != 0) {
        return r;
    }
    /* A socket node is the filesystem's to keep, if it can (docs/abi/ L5). */
    r = type == VIBEOS_S_IFSOCK
            ? vibeos_fs_mknod(w.mnt, w.tail, VIBEOS_S_IFSOCK | ((uint32_t)mode & 07777u & ~linux_umask()), &node)
            : vibeos_fs_create(w.mnt, w.tail, (uint32_t)mode & 07777u & ~linux_umask(), &node);
    if (r == 0) {
        linux_own_node(&w, &node);
    }
    return r;
}

/* openat2(dirfd, path, how, size): openat with its arguments in a structure
 * that can grow, and with restrictions on how the path may be resolved.
 *
 * The structure is read as Linux reads an extensible one: a caller's may be
 * longer than the kernel's as long as the part the kernel does not know is
 * zero (E2BIG otherwise), and may not be shorter than the first version.
 *
 * Of the resolve flags only NO_MAGICLINKS is honoured, and that trivially -
 * there is no /proc to hold one. BENEATH, IN_ROOT, NO_XDEV and NO_SYMLINKS are
 * promises about the walk this kernel's walk does not make, so they are ENOSYS:
 * the answer a caller already handles by falling back to openat and checking
 * for itself, where EINVAL would say its arguments were wrong. CACHED is
 * EAGAIN, Linux's own "not without blocking, ask again without it". */
static long linux_sys_openat2(uint64_t dirfd, uint64_t path_uptr, uint64_t how_uptr, uint64_t size) {
    linux_open_how_t how;
    uint64_t off;

    if (size < sizeof(how)) {
        return -VIBEOS_EINVAL;
    }
    if (size > 4096u) {
        return -VIBEOS_E2BIG;
    }
    /* The row declares the structure as a buffer of `size` bytes, so the range
     * was judged before this ran - which makes an absurd size on a good
     * pointer EFAULT where Linux says E2BIG first. */
    if (vibeos_uaccess_copy(&how, (const void *)(uintptr_t)how_uptr, sizeof(how)) != 0) {
        return -VIBEOS_EFAULT;
    }
    for (off = sizeof(how); off < size; off++) {
        uint8_t b = 0;
        if (vibeos_uaccess_copy(&b, (const void *)(uintptr_t)(how_uptr + off), 1u) != 0) {
            return -VIBEOS_EFAULT;
        }
        if (b != 0u) {
            return -VIBEOS_E2BIG;
        }
    }
    /* A bit that is no open flag at all is refused, where open and openat
     * ignore it - that strictness is the point of the call. A flag Linux has
     * and this kernel gives no meaning to (O_NOATIME, O_DIRECT) is taken as
     * open takes it. */
    if (how.flags & ~(uint64_t)LINUX_OPEN_VALID) {
        return -VIBEOS_EINVAL;
    }
    if (how.mode & ~(uint64_t)07777u) {
        return -VIBEOS_EINVAL;
    }
    if (how.mode != 0u && !(how.flags & VIBEOS_O_CREAT)) {
        return -VIBEOS_EINVAL;
    }
    if (how.resolve & ~(uint64_t)(LINUX_RESOLVE_NO_XDEV | LINUX_RESOLVE_NO_MAGICLINKS |
                                  LINUX_RESOLVE_NO_SYMLINKS | LINUX_RESOLVE_BENEATH |
                                  LINUX_RESOLVE_IN_ROOT | LINUX_RESOLVE_CACHED)) {
        return -VIBEOS_EINVAL;
    }
    if ((how.resolve & LINUX_RESOLVE_BENEATH) && (how.resolve & LINUX_RESOLVE_IN_ROOT)) {
        return -VIBEOS_EINVAL;
    }
    if (how.resolve & LINUX_RESOLVE_CACHED) {
        return -VIBEOS_EAGAIN;
    }
    if (how.resolve & ~(uint64_t)LINUX_RESOLVE_NO_MAGICLINKS) {
        return -VIBEOS_ENOSYS;
    }
    return linux_sys_openat(dirfd, path_uptr, how.flags, how.mode);
}

/* ---- chmod and chown ----------------------------------------------------------------- */

/* The chmod family: the permission bits, and the three above them. A symbolic
 * link has none of its own to change - Linux answers EOPNOTSUPP when asked not
 * to follow one. */
static long linux_chmod_walked(const vibeos_path_t *w, uint64_t mode) {
    vibeos_fs_attr_t attr;

    if ((w->node.mode & VIBEOS_S_IFMT) == VIBEOS_S_IFLNK) {
        return -VIBEOS_EOPNOTSUPP;
    }
    /* The owner's to change, or the superuser's: EPERM for anybody else,
     * whatever the file's own bits would let them do to its contents. */
    if (linux_may_own(&w->node) != 0) {
        return -VIBEOS_EPERM;
    }
    attr.valid = VIBEOS_ATTR_MODE;
    attr.mode = (uint32_t)mode & 07777u;
    return linux_setattr_walked(w, &attr);
}

static long linux_chmod_at(uint64_t dirfd, uint64_t path_uptr, uint64_t mode, uint64_t flags) {
    vibeos_path_t w;
    long r;

    if (flags & ~(uint64_t)(LINUX_AT_SYMLINK_NOFOLLOW | LINUX_AT_EMPTY_PATH)) {
        return -VIBEOS_EINVAL;
    }
    r = linux_walk_at_empty(dirfd, path_uptr, flags,
                            (flags & LINUX_AT_SYMLINK_NOFOLLOW) ? VIBEOS_PATH_NOFOLLOW : 0u, &w);
    return r != 0 ? r : linux_chmod_walked(&w, mode);
}

static long linux_sys_fchmod(uint64_t fd, uint64_t mode) {
    vibeos_path_t w;
    long r = linux_walk_fd(fd, &w);

    return r != 0 ? r : linux_chmod_walked(&w, mode);
}

/* The chown family. An id of -1 leaves that one alone. Changing either takes
 * the set-user-id bit off a regular file, and the set-group-id bit when the
 * file is group-executable (without that bit it means mandatory locking, and
 * stays) - as Linux does even for root, so that a file handed to somebody else
 * does not go on running as whoever owned it. */
static long linux_chown_walked(const vibeos_path_t *w, uint64_t uid, uint64_t gid) {
    vibeos_fs_attr_t attr;
    uint32_t mode = w->node.mode;

    attr.valid = 0;
    if (VIBEOS_ARG_INT(uid) != -1) {
        attr.valid |= VIBEOS_ATTR_UID;
        attr.uid = (uint32_t)uid;
    }
    if (VIBEOS_ARG_INT(gid) != -1) {
        attr.valid |= VIBEOS_ATTR_GID;
        attr.gid = (uint32_t)gid;
    }
    if (attr.valid == 0u) {
        return 0;
    }
    {
        /* Giving a file away is the superuser's. Its owner may hand it to
         * another of the owner's own groups, and that is all. */
        vibeos_cred_t c;
        linux_cred(&c);
        if (c.fsuid != 0u &&
            (c.fsuid != w->node.uid ||
             ((attr.valid & VIBEOS_ATTR_UID) && attr.uid != w->node.uid) ||
             ((attr.valid & VIBEOS_ATTR_GID) && !vibeos_cred_in_group(&c, attr.gid, 0)))) {
            return -VIBEOS_EPERM;
        }
    }
    if ((mode & VIBEOS_S_IFMT) == VIBEOS_S_IFREG) {
        uint32_t drop = VIBEOS_S_ISUID | ((mode & 0010u) ? VIBEOS_S_ISGID : 0u);
        if (mode & drop) {
            attr.valid |= VIBEOS_ATTR_MODE;
            attr.mode = (mode & 07777u) & ~drop;
        }
    }
    return linux_setattr_walked(w, &attr);
}

static long linux_chown_at(uint64_t dirfd, uint64_t path_uptr, uint64_t uid, uint64_t gid,
                           uint64_t flags) {
    vibeos_path_t w;
    long r;

    if (flags & ~(uint64_t)(LINUX_AT_SYMLINK_NOFOLLOW | LINUX_AT_EMPTY_PATH)) {
        return -VIBEOS_EINVAL;
    }
    r = linux_walk_at_empty(dirfd, path_uptr, flags,
                            (flags & LINUX_AT_SYMLINK_NOFOLLOW) ? VIBEOS_PATH_NOFOLLOW : 0u, &w);
    return r != 0 ? r : linux_chown_walked(&w, uid, gid);
}

static long linux_sys_fchown(uint64_t fd, uint64_t uid, uint64_t gid) {
    vibeos_path_t w;
    long r = linux_walk_fd(fd, &w);

    return r != 0 ? r : linux_chown_walked(&w, uid, gid);
}

/* ---- times --------------------------------------------------------------------------- */

/* One of the two times a call sets: left alone, or a value in nanoseconds on
 * the clock the filesystems stamp with (vibeos_fs_now_ns). */
typedef struct {
    int set;
    int now;        /* the caller asked for the present moment, not a time of its choosing */
    uint64_t ns;
} linux_when_t;

static long linux_times_walked(const vibeos_path_t *w, const linux_when_t *a, const linux_when_t *m) {
    vibeos_fs_attr_t attr;

    attr.valid = 0;
    if (a->set) {
        attr.valid |= VIBEOS_ATTR_ATIME;
        attr.atime_ns = a->ns;
    }
    if (m->set) {
        attr.valid |= VIBEOS_ATTR_MTIME;
        attr.mtime_ns = m->ns;
    }
    if (attr.valid == 0u) {
        return 0;
    }
    /* A file's times are its owner's to set. Anybody who may write it may
     * stamp it "now", which is what touch does - `now` says both times are. */
    if (linux_may_own(&w->node) != 0 &&
        !(a->now && m->now && linux_may(&w->node, VIBEOS_MAY_WRITE) == 0)) {
        return a->now && m->now ? -VIBEOS_EACCES : -VIBEOS_EPERM;
    }
    return linux_setattr_walked(w, &attr);
}

/* A time as nanoseconds since 1970, held at the largest this kernel stores
 * (the year 2554) rather than wrapped: a tv_sec past it multiplied by 10^9 in 64
 * bits used to land on some other, earlier time (external review, 2026-10-07). */
static uint64_t linux_ns_of(int64_t sec, uint64_t ns) {
    if ((uint64_t)sec > (~0ull - ns) / 1000000000ull) {
        return ~0ull;
    }
    return (uint64_t)sec * 1000000000ull + ns;
}

/* utimensat(dirfd, path, times, flags): both times to the nanosecond, each of
 * which may be "now" or "leave it". No times at all is both now; no path at all
 * is `dirfd` itself, which is how futimens is spelled. */
static long linux_sys_utimensat(uint64_t dirfd, uint64_t path_uptr, uint64_t times_uptr,
                                uint64_t flags) {
    linux_when_t when[2];
    vibeos_path_t w;
    uint64_t now = vibeos_fs_now_ns();
    uint32_t i;
    long r;

    if (flags & ~(uint64_t)(LINUX_AT_SYMLINK_NOFOLLOW | LINUX_AT_EMPTY_PATH)) {
        return -VIBEOS_EINVAL;
    }
    when[0].set = when[1].set = 1;
    when[0].now = when[1].now = 1;
    when[0].ns = when[1].ns = now;
    if (times_uptr != 0u) {
        linux_timespec_t ts[2];
        if (vibeos_uaccess_copy(ts, (const void *)(uintptr_t)times_uptr, sizeof(ts)) != 0) {
            return -VIBEOS_EFAULT;
        }
        for (i = 0; i < 2u; i++) {
            if (ts[i].tv_nsec == LINUX_UTIME_NOW) {
                continue;
            }
            if (ts[i].tv_nsec == LINUX_UTIME_OMIT) {
                when[i].set = 0;
                continue;
            }
            if (ts[i].tv_nsec < 0 || ts[i].tv_nsec >= 1000000000ll || ts[i].tv_sec < 0) {
                return -VIBEOS_EINVAL;
            }
            when[i].now = 0;
            when[i].ns = linux_ns_of(ts[i].tv_sec, (uint64_t)ts[i].tv_nsec);
        }
    }
    if (path_uptr == 0u) {
        r = linux_walk_fd(dirfd, &w);
    } else {
        r = linux_walk_at_empty(dirfd, path_uptr, flags,
                                (flags & LINUX_AT_SYMLINK_NOFOLLOW) ? VIBEOS_PATH_NOFOLLOW : 0u, &w);
    }
    return r != 0 ? r : linux_times_walked(&w, &when[0], &when[1]);
}

/* utimes and futimesat: the same to the microsecond, with no "leave it". */
static long linux_utimes_at(uint64_t dirfd, uint64_t path_uptr, uint64_t times_uptr) {
    linux_when_t when[2];
    vibeos_path_t w;
    uint32_t i;
    long r;

    when[0].set = when[1].set = 1;
    when[0].now = when[1].now = 1;
    when[0].ns = when[1].ns = vibeos_fs_now_ns();
    if (times_uptr != 0u) {
        linux_timeval_t tv[2];
        if (vibeos_uaccess_copy(tv, (const void *)(uintptr_t)times_uptr, sizeof(tv)) != 0) {
            return -VIBEOS_EFAULT;
        }
        for (i = 0; i < 2u; i++) {
            if (tv[i].tv_usec < 0 || tv[i].tv_usec >= 1000000ll || tv[i].tv_sec < 0) {
                return -VIBEOS_EINVAL;
            }
            when[i].now = 0;
            when[i].ns = linux_ns_of(tv[i].tv_sec, (uint64_t)tv[i].tv_usec * 1000ull);
        }
    }
    r = linux_walk_at(dirfd, path_uptr, 0u, &w);
    return r != 0 ? r : linux_times_walked(&w, &when[0], &when[1]);
}

/* utime: the same to the second. */
static long linux_sys_utime(uint64_t path_uptr, uint64_t times_uptr) {
    linux_when_t when[2];
    vibeos_path_t w;
    long r;

    when[0].set = when[1].set = 1;
    when[0].now = when[1].now = 1;
    when[0].ns = when[1].ns = vibeos_fs_now_ns();
    if (times_uptr != 0u) {
        linux_utimbuf_t ut;
        if (vibeos_uaccess_copy(&ut, (const void *)(uintptr_t)times_uptr, sizeof(ut)) != 0) {
            return -VIBEOS_EFAULT;
        }
        if (ut.actime < 0 || ut.modtime < 0) {
            return -VIBEOS_EINVAL;
        }
        when[0].now = when[1].now = 0;
        when[0].ns = linux_ns_of(ut.actime, 0);   /* held, not wrapped: M-095 */
        when[1].ns = linux_ns_of(ut.modtime, 0);
    }
    r = linux_walk_at(LINUX_CWD, path_uptr, 0u, &w);
    return r != 0 ? r : linux_times_walked(&w, &when[0], &when[1]);
}

/* ---- statfs -------------------------------------------------------------------------- */

static long linux_statfs_mnt(vibeos_fsmount_t *mnt, uint64_t ubuf) {
    vibeos_fs_statfs_t s;
    linux_statfs_t k;
    uint8_t *raw = (uint8_t *)&k;
    uint32_t i;
    int r = vibeos_fs_statfs(mnt, &s);

    if (r != 0) {
        return r;
    }
    for (i = 0; i < sizeof(k); i++) {
        raw[i] = 0;
    }
    k.f_type = (int64_t)s.magic;
    k.f_bsize = (int64_t)s.block_size;
    k.f_frsize = (int64_t)s.block_size;
    k.f_blocks = (int64_t)s.blocks;
    k.f_bfree = (int64_t)s.blocks_free;
    k.f_bavail = (int64_t)s.blocks_free;   /* nothing is held back for root */
    k.f_files = (int64_t)s.files;
    k.f_ffree = (int64_t)s.files_free;
    k.f_fsid[0] = (int32_t)linux_dev_of(mnt);
    k.f_namelen = (int64_t)s.name_max;
    k.f_flags = s.read_only ? LINUX_ST_RDONLY : 0;
    if (vibeos_uaccess_copy((void *)(uintptr_t)ubuf, &k, sizeof(k)) != 0) {
        return -VIBEOS_EFAULT;
    }
    return 0;
}

/* statfs(path, buf) and fstatfs(fd, buf): the filesystem a file is on. A
 * descriptor on none - a pipe, a socket - is ENOSYS, Linux's answer from a
 * filesystem with nothing to report. */
static long linux_sys_statfs(uint64_t path_uptr, uint64_t ubuf) {
    vibeos_path_t w;
    long r = linux_walk_at(LINUX_CWD, path_uptr, 0u, &w);

    return r != 0 ? r : linux_statfs_mnt(w.mnt, ubuf);
}

/* The description's mount, and nothing else: which file it is does not matter,
 * so neither does where its name now leads (M-080). */
static long linux_sys_fstatfs(uint64_t fd, uint64_t ubuf) {
    vibeos_file_t *f = linux_file_get(fd);
    long r;

    if (!f) {
        return -VIBEOS_EBADF;
    }
    r = f->mnt ? linux_statfs_mnt(f->mnt, ubuf) : -VIBEOS_ENOSYS;
    vibeos_file_put(f);
    return r;
}

/* ---- extended attributes ---------------------------------------------------------------
 *
 * Linux's twelve calls over the VFS's four (E4-011). A name is split into its
 * namespace and the rest, as ext4 stores it: user., trusted. and security.;
 * the ACL names (system.posix_acl_*) and anything else are EOPNOTSUPP until
 * ACLs exist, because storing an ACL nobody enforces would be a permission
 * that lies. What Linux refuses before it asks the filesystem is refused first,
 * in its order: a flag setxattr does not have, a name empty or too long
 * (ERANGE), a value too large (E2BIG), the file itself (ENOENT, EBADF), then
 * the namespace's rule, then the filesystem. A filesystem with no attributes
 * answers EOPNOTSUPP and lists none, which is how `ls -l`, `cp -a` and `tar`
 * find out and carry on. Values go through one page: past it is ENOSPC, which
 * is what ext4 says of a value its block cannot hold. */

#define LINUX_XATTR_VALUE_CAP 4096u

typedef struct {
    vibeos_fsmount_t *mnt;
    vibeos_fs_node_t node;
    vibeos_file_t *f;   /* held while the call runs, for a descriptor */
} linux_xattr_target_t;

static long linux_xattr_name(uint64_t name_uptr, char *name) {
    uint32_t n = 0;

    if (ks_copy_user_string(name_uptr, name, LINUX_XATTR_NAME_MAX + 2) != 0) {
        return -VIBEOS_EFAULT;
    }
    while (name[n]) {
        n++;
    }
    return (n == 0u || n > LINUX_XATTR_NAME_MAX) ? -VIBEOS_ERANGE : 0;
}

/* The namespace of a full name, and where the rest starts. */
static long linux_xattr_split(const char *name, uint32_t *index, const char **rest, uint32_t *rlen) {
    static const struct {
        const char *prefix;
        uint32_t len, index;
    } ns[] = {
        {"user.", 5u, VIBEOS_XATTR_USER},
        {"trusted.", 8u, VIBEOS_XATTR_TRUSTED},
        {"security.", 9u, VIBEOS_XATTR_SECURITY},
    };
    uint32_t k, i, n = 0;

    while (name[n]) {
        n++;
    }
    for (k = 0; k < sizeof(ns) / sizeof(ns[0]); k++) {
        for (i = 0; i < ns[k].len && name[i] == ns[k].prefix[i]; i++) {
        }
        if (i == ns[k].len) {
            if (n == ns[k].len) {
                return -VIBEOS_EINVAL;   /* a prefix with nothing after it */
            }
            *index = ns[k].index;
            *rest = name + ns[k].len;
            *rlen = n - ns[k].len;
            return 0;
        }
    }
    return -VIBEOS_EOPNOTSUPP;
}

static long linux_xattr_target(uint64_t where, int how, linux_xattr_target_t *t) {
    t->f = 0;
    t->mnt = 0;
    if (how >= 0) {
        vibeos_path_t w;
        long r = linux_walk_at(LINUX_CWD, where, (uint32_t)how, &w);

        if (r != 0) {
            return r;
        }
        if (!w.exists) {
            return -VIBEOS_ENOENT;
        }
        t->mnt = w.mnt;
        t->node = w.node;
        return 0;
    }
    if (!(t->f = linux_file_get(where))) {
        return -VIBEOS_EBADF;
    }
    if (!t->f->mnt || vibeos_file_node(t->f, &t->node) != 0) {
        vibeos_file_put(t->f);
        t->f = 0;
        return -VIBEOS_EOPNOTSUPP;   /* a pipe, a socket: nothing to attach a name to */
    }
    t->mnt = t->f->mnt;
    return 0;
}

static void linux_xattr_done(linux_xattr_target_t *t) {
    if (t->f) {
        vibeos_file_put(t->f);
    }
}

/* Linux's xattr_permission: user.* lives only on files and directories, and a
 * sticky directory's only for its owner to change; trusted.* is the
 * superuser's, and to anybody else it does not exist; security.* is the
 * security module's to judge, and there is none. */
static long linux_xattr_may(const vibeos_fs_node_t *node, uint32_t index, int write) {
    vibeos_cred_t c;

    linux_cred(&c);
    if (index == VIBEOS_XATTR_TRUSTED) {
        return c.euid == 0u ? 0 : (write ? -VIBEOS_EPERM : -VIBEOS_ENODATA);
    }
    if (index == VIBEOS_XATTR_USER) {
        uint32_t t = node->mode & VIBEOS_S_IFMT;

        if (t != VIBEOS_S_IFREG && t != VIBEOS_S_IFDIR) {
            return write ? -VIBEOS_EPERM : -VIBEOS_ENODATA;
        }
        if (write && t == VIBEOS_S_IFDIR && (node->mode & VIBEOS_S_ISVTX) != 0u && c.euid != 0u &&
            c.euid != node->uid) {
            return -VIBEOS_EPERM;
        }
        return linux_may(node, write ? VIBEOS_MAY_WRITE : VIBEOS_MAY_READ);
    }
    return 0;
}

static long linux_xattr_set(uint64_t where, int how, uint64_t name_uptr, uint64_t value_uptr, uint64_t size,
                            uint64_t flags) {
    char name[LINUX_XATTR_NAME_MAX + 2];
    linux_xattr_target_t t;
    const char *rest;
    uint32_t index, rlen, vflags = 0;
    uint8_t *kv = 0;
    long r;

    if (flags & ~(uint64_t)(LINUX_XATTR_CREATE | LINUX_XATTR_REPLACE)) {
        return -VIBEOS_EINVAL;
    }
    if ((r = linux_xattr_name(name_uptr, name)) != 0) {
        return r;
    }
    if (size > LINUX_XATTR_SIZE_MAX) {
        return -VIBEOS_E2BIG;
    }
    if ((r = linux_xattr_target(where, how, &t)) != 0) {
        return r;
    }
    if ((r = linux_xattr_split(name, &index, &rest, &rlen)) == 0 && (r = linux_xattr_may(&t.node, index, 1)) == 0) {
        if (size > LINUX_XATTR_VALUE_CAP) {
            r = t.mnt->ops->setxattr ? -VIBEOS_ENOSPC : -VIBEOS_EOPNOTSUPP;
        } else if (size > 0u && (!linux_user_ok(value_uptr, size, 0) || !(kv = (uint8_t *)ks_page_alloc()))) {
            r = kv ? -VIBEOS_EFAULT : -VIBEOS_ENOMEM;
        } else if (size > 0u && vibeos_uaccess_copy(kv, (const void *)(uintptr_t)value_uptr, size) != 0) {
            r = -VIBEOS_EFAULT;
        } else {
            vflags = (flags & LINUX_XATTR_CREATE ? VIBEOS_XATTR_CREATE : 0u) |
                     (flags & LINUX_XATTR_REPLACE ? VIBEOS_XATTR_REPLACE : 0u);
            r = vibeos_fs_setxattr(t.mnt, &t.node, index, rest, rlen, kv, (uint32_t)size, vflags);
        }
    }
    if (kv) {
        ks_page_free(kv, "setxattr value");
    }
    linux_xattr_done(&t);
    return r;
}

static long linux_xattr_get(uint64_t where, int how, uint64_t name_uptr, uint64_t value_uptr, uint64_t size) {
    char name[LINUX_XATTR_NAME_MAX + 2];
    linux_xattr_target_t t;
    const char *rest;
    uint32_t index, rlen, cap;
    uint8_t *kv = 0;
    long r;

    if ((r = linux_xattr_name(name_uptr, name)) != 0) {
        return r;
    }
    if ((r = linux_xattr_target(where, how, &t)) != 0) {
        return r;
    }
    if ((r = linux_xattr_split(name, &index, &rest, &rlen)) == 0 && (r = linux_xattr_may(&t.node, index, 0)) == 0) {
        cap = size > LINUX_XATTR_VALUE_CAP ? LINUX_XATTR_VALUE_CAP : (uint32_t)size;
        if (cap > 0u && !(kv = (uint8_t *)ks_page_alloc())) {
            r = -VIBEOS_ENOMEM;
        } else {
            r = vibeos_fs_getxattr(t.mnt, &t.node, index, rest, rlen, kv, cap);
            if (r > 0 && cap > 0u &&
                (!linux_user_ok(value_uptr, (uint64_t)r, 1) ||
                 vibeos_uaccess_copy((void *)(uintptr_t)value_uptr, kv, (uint64_t)r) != 0)) {
                r = -VIBEOS_EFAULT;
            }
        }
    }
    if (kv) {
        ks_page_free(kv, "getxattr value");
    }
    linux_xattr_done(&t);
    return r;
}

static long linux_xattr_remove(uint64_t where, int how, uint64_t name_uptr) {
    char name[LINUX_XATTR_NAME_MAX + 2];
    linux_xattr_target_t t;
    const char *rest;
    uint32_t index, rlen;
    long r;

    if ((r = linux_xattr_name(name_uptr, name)) != 0) {
        return r;
    }
    if ((r = linux_xattr_target(where, how, &t)) != 0) {
        return r;
    }
    if ((r = linux_xattr_split(name, &index, &rest, &rlen)) == 0 && (r = linux_xattr_may(&t.node, index, 1)) == 0) {
        r = vibeos_fs_removexattr(t.mnt, &t.node, index, rest, rlen);
    }
    linux_xattr_done(&t);
    return r;
}

/* listxattr: every name the caller may see, each with its prefix and a NUL. */
static long linux_xattr_list(uint64_t where, int how, uint64_t list_uptr, uint64_t size) {
    static const char *const prefix[8] = {0, "user.", 0, 0, "trusted.", 0, "security.", 0};
    linux_xattr_target_t t;
    char nm[LINUX_XATTR_NAME_MAX + 1];
    uint8_t *out = 0;
    uint32_t k, index, nlen, total = 0;
    vibeos_cred_t c;
    long r;
    int rc;

    if ((r = linux_xattr_target(where, how, &t)) != 0) {
        return r;
    }
    linux_cred(&c);
    if (size > 0u && !(out = (uint8_t *)ks_page_alloc())) {
        linux_xattr_done(&t);
        return -VIBEOS_ENOMEM;
    }
    for (k = 0; (rc = vibeos_fs_listxattr(t.mnt, &t.node, k, &index, nm, sizeof(nm), &nlen)) == 0; k++) {
        const char *pf = index < 8u ? prefix[index] : 0;
        uint32_t pl = 0, i;

        if (!pf || (index == VIBEOS_XATTR_TRUSTED && c.euid != 0u)) {
            continue;   /* a namespace this kernel does not name, or the caller may not see */
        }
        while (pf[pl]) {
            pl++;
        }
        if (out) {
            if (total + pl + nlen + 1u > size || total + pl + nlen + 1u > LINUX_XATTR_VALUE_CAP) {
                r = -VIBEOS_ERANGE;
                break;
            }
            for (i = 0; i < pl; i++) {
                out[total + i] = (uint8_t)pf[i];
            }
            for (i = 0; i < nlen; i++) {
                out[total + pl + i] = (uint8_t)nm[i];
            }
            out[total + pl + nlen] = 0;
        }
        total += pl + nlen + 1u;
    }
    if (r == 0 && rc < 0) {
        r = rc;
    }
    if (r == 0) {
        r = (long)total;
        if (out && total > 0u &&
            (!linux_user_ok(list_uptr, total, 1) || vibeos_uaccess_copy((void *)(uintptr_t)list_uptr, out, total) != 0)) {
            r = -VIBEOS_EFAULT;
        }
    }
    if (out) {
        ks_page_free(out, "listxattr names");
    }
    linux_xattr_done(&t);
    return r;
}

/* ---- the syscalls this file implements ---------------------------------------
 *
 *   A path argument is not a declared pointer: it is copied in by
 *   ks_copy_user_string, which checks and copies as one fault-safe step.
 *   access   and faccessat have no flags argument - the C library's faccessat
 *            emulates them - so only faccessat2 passes one on. fchmodat is the
 *            same against fchmodat2. */

#define LINUX_NAMES_SYSCALLS(X) \
    X(21,  access,      ACCESS,      NOPTR, linux_access_at(LINUX_CWD, ARG(0), ARG(1), 0)) \
    X(82,  rename,      RENAME,      NOPTR, linux_rename_at(LINUX_CWD, ARG(0), LINUX_CWD, ARG(1), 0)) \
    X(84,  rmdir,       RMDIR,       NOPTR, linux_rmdir_at(LINUX_CWD, ARG(0))) \
    X(86,  link,        LINK,        NOPTR, linux_link_at(LINUX_CWD, ARG(0), LINUX_CWD, ARG(1), 0)) \
    X(88,  symlink,     SYMLINK,     NOPTR, linux_symlink_at(ARG(0), LINUX_CWD, ARG(1))) \
    X(90,  chmod,       CHMOD,       NOPTR, linux_chmod_at(LINUX_CWD, ARG(0), ARG(1), 0)) \
    X(91,  fchmod,      FCHMOD,      NOPTR, linux_sys_fchmod(ARG(0), ARG(1))) \
    X(92,  chown,       CHOWN,       NOPTR, linux_chown_at(LINUX_CWD, ARG(0), ARG(1), ARG(2), 0)) \
    X(93,  fchown,      FCHOWN,      NOPTR, linux_sys_fchown(ARG(0), ARG(1), ARG(2))) \
    X(94,  lchown,      CHOWN,       NOPTR, linux_chown_at(LINUX_CWD, ARG(0), ARG(1), ARG(2), LINUX_AT_SYMLINK_NOFOLLOW)) \
    X(132, utime,       UTIME,       PTRS(IN_OPT(1, sizeof(linux_utimbuf_t))), linux_sys_utime(ARG(0), ARG(1))) \
    X(133, mknod,       MKNOD,       NOPTR, linux_mknod_at(LINUX_CWD, ARG(0), ARG(1))) \
    X(137, statfs,      STATFS,      PTRS(OUT(1, sizeof(linux_statfs_t))), linux_sys_statfs(ARG(0), ARG(1))) \
    X(138, fstatfs,     FSTATFS,     PTRS(OUT(1, sizeof(linux_statfs_t))), linux_sys_fstatfs(ARG(0), ARG(1))) \
    X(188, setxattr,    XATTR_SET,   NOPTR, linux_xattr_set(ARG(0), 0, ARG(1), ARG(2), ARG(3), ARG(4))) \
    X(189, lsetxattr,   XATTR_SET,   NOPTR, linux_xattr_set(ARG(0), VIBEOS_PATH_NOFOLLOW, ARG(1), ARG(2), ARG(3), ARG(4))) \
    X(190, fsetxattr,   XATTR_SET,   NOPTR, linux_xattr_set(ARG(0), -1, ARG(1), ARG(2), ARG(3), ARG(4))) \
    X(191, getxattr,    XATTR_GET,   NOPTR, linux_xattr_get(ARG(0), 0, ARG(1), ARG(2), ARG(3))) \
    X(192, lgetxattr,   XATTR_GET,   NOPTR, linux_xattr_get(ARG(0), VIBEOS_PATH_NOFOLLOW, ARG(1), ARG(2), ARG(3))) \
    X(193, fgetxattr,   XATTR_GET,   NOPTR, linux_xattr_get(ARG(0), -1, ARG(1), ARG(2), ARG(3))) \
    X(194, listxattr,   XATTR_LIST,  NOPTR, linux_xattr_list(ARG(0), 0, ARG(1), ARG(2))) \
    X(195, llistxattr,  XATTR_LIST,  NOPTR, linux_xattr_list(ARG(0), VIBEOS_PATH_NOFOLLOW, ARG(1), ARG(2))) \
    X(196, flistxattr,  XATTR_LIST,  NOPTR, linux_xattr_list(ARG(0), -1, ARG(1), ARG(2))) \
    X(197, removexattr, XATTR_REMOVE, NOPTR, linux_xattr_remove(ARG(0), 0, ARG(1))) \
    X(198, lremovexattr, XATTR_REMOVE, NOPTR, linux_xattr_remove(ARG(0), VIBEOS_PATH_NOFOLLOW, ARG(1))) \
    X(199, fremovexattr, XATTR_REMOVE, NOPTR, linux_xattr_remove(ARG(0), -1, ARG(1))) \
    X(235, utimes,      UTIME,       PTRS(IN_OPT(1, 2 * sizeof(linux_timeval_t))), linux_utimes_at(LINUX_CWD, ARG(0), ARG(1))) \
    X(259, mknodat,     MKNOD,       NOPTR, linux_mknod_at(ARG(0), ARG(1), ARG(2))) \
    X(260, fchownat,    CHOWN,       NOPTR, linux_chown_at(ARG(0), ARG(1), ARG(2), ARG(3), ARG(4))) \
    X(261, futimesat,   UTIME,       PTRS(IN_OPT(2, 2 * sizeof(linux_timeval_t))), linux_utimes_at(ARG(0), ARG(1), ARG(2))) \
    X(264, renameat,    RENAME,      NOPTR, linux_rename_at(ARG(0), ARG(1), ARG(2), ARG(3), 0)) \
    X(265, linkat,      LINK,        NOPTR, linux_link_at(ARG(0), ARG(1), ARG(2), ARG(3), ARG(4))) \
    X(266, symlinkat,   SYMLINK,     NOPTR, linux_symlink_at(ARG(0), ARG(1), ARG(2))) \
    X(268, fchmodat,    CHMOD,       NOPTR, linux_chmod_at(ARG(0), ARG(1), ARG(2), 0)) \
    X(269, faccessat,   ACCESS,      NOPTR, linux_access_at(ARG(0), ARG(1), ARG(2), 0)) \
    X(280, utimensat,   UTIME,       PTRS(IN_OPT(2, 2 * sizeof(linux_timespec_t))), linux_sys_utimensat(ARG(0), ARG(1), ARG(2), ARG(3))) \
    X(316, renameat2,   RENAME,      NOPTR, linux_rename_at(ARG(0), ARG(1), ARG(2), ARG(3), ARG(4))) \
    X(332, statx,       STATX,       PTRS(OUT(4, sizeof(linux_statx_t))), linux_sys_statx(ARG(0), ARG(1), ARG(2), ARG(3), ARG(4))) \
    X(437, openat2,     OPEN_AT2,    PTRS(IN_BUF(2, 3)), linux_sys_openat2(ARG(0), ARG(1), ARG(2), ARG(3))) \
    X(439, faccessat2,  ACCESS,      NOPTR, linux_access_at(ARG(0), ARG(1), ARG(2), ARG(3))) \
    X(452, fchmodat2,   CHMOD,       NOPTR, linux_chmod_at(ARG(0), ARG(1), ARG(2), ARG(3)))

LINUX_DEFINE_SYSCALLS(names, LINUX_NAMES_SYSCALLS)
