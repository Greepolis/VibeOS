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
    k.stx_nlink = st.nlink ? st.nlink : 1u;
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
    if (vibeos_uaccess_copy((void *)(uintptr_t)ubuf, &k, sizeof(k)) != 0) {
        return -VIBEOS_EFAULT;
    }
    return 0;
}

/* ---- access -------------------------------------------------------------------------- */

/* access, faccessat and faccessat2: may the caller read, write or run it? The
 * caller is root, so the answers are root's: writing is refused on a filesystem
 * that writes nothing (EROFS), and running a regular file with no execute bit
 * at all (EACCES). AT_EACCESS asks about the effective identity where the real
 * one is the default; there is one identity, so it changes nothing. */
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
    if ((mode & LINUX_X_OK) && (w.node.mode & VIBEOS_S_IFMT) == VIBEOS_S_IFREG &&
        (w.node.mode & 0111u) == 0u) {
        return -VIBEOS_EACCES;
    }
    return 0;
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
    return vibeos_fs_rename(a.mnt, a.tail, b.tail,
                            (flags & LINUX_RENAME_NOREPLACE) ? VIBEOS_RENAME_NOREPLACE : 0u);
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
    return vibeos_fs_symlink(w.mnt, target, w.tail);
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
    if (type != 0u && type != VIBEOS_S_IFREG) {
        return -VIBEOS_EPERM;
    }
    return vibeos_fs_create(w.mnt, w.tail, (uint32_t)mode & 07777u & ~linux_umask(), &node);
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
    attr.valid = VIBEOS_ATTR_MODE;
    attr.mode = (uint32_t)mode & 07777u;
    return vibeos_fs_setattr(w->mnt, linux_tail(w), &attr);
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
    if ((mode & VIBEOS_S_IFMT) == VIBEOS_S_IFREG) {
        uint32_t drop = VIBEOS_S_ISUID | ((mode & 0010u) ? VIBEOS_S_ISGID : 0u);
        if (mode & drop) {
            attr.valid |= VIBEOS_ATTR_MODE;
            attr.mode = (mode & 07777u) & ~drop;
        }
    }
    return vibeos_fs_setattr(w->mnt, linux_tail(w), &attr);
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
    return attr.valid == 0u ? 0 : vibeos_fs_setattr(w->mnt, linux_tail(w), &attr);
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
            when[i].ns = (uint64_t)ts[i].tv_sec * 1000000000ull + (uint64_t)ts[i].tv_nsec;
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
            when[i].ns = (uint64_t)tv[i].tv_sec * 1000000000ull + (uint64_t)tv[i].tv_usec * 1000ull;
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
    when[0].ns = when[1].ns = vibeos_fs_now_ns();
    if (times_uptr != 0u) {
        linux_utimbuf_t ut;
        if (vibeos_uaccess_copy(&ut, (const void *)(uintptr_t)times_uptr, sizeof(ut)) != 0) {
            return -VIBEOS_EFAULT;
        }
        if (ut.actime < 0 || ut.modtime < 0) {
            return -VIBEOS_EINVAL;
        }
        when[0].ns = (uint64_t)ut.actime * 1000000000ull;
        when[1].ns = (uint64_t)ut.modtime * 1000000000ull;
    }
    r = linux_walk_at(LINUX_CWD, path_uptr, 0u, &w);
    return r != 0 ? r : linux_times_walked(&w, &when[0], &when[1]);
}

/* ---- statfs -------------------------------------------------------------------------- */

static long linux_statfs_walked(const vibeos_path_t *w, uint64_t ubuf) {
    vibeos_fs_statfs_t s;
    linux_statfs_t k;
    uint8_t *raw = (uint8_t *)&k;
    uint32_t i;
    int r = vibeos_fs_statfs(w->mnt, &s);

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
    k.f_fsid[0] = (int32_t)linux_dev_of(w->mnt);
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

    return r != 0 ? r : linux_statfs_walked(&w, ubuf);
}

static long linux_sys_fstatfs(uint64_t fd, uint64_t ubuf) {
    vibeos_path_t w;
    long r = linux_walk_fd(fd, &w);

    if (r == -VIBEOS_EINVAL) {
        return -VIBEOS_ENOSYS;
    }
    return r != 0 ? r : linux_statfs_walked(&w, ubuf);
}

/* ---- extended attributes --------------------------------------------------------------
 *
 * No filesystem here stores them, and the twelve calls say what Linux says of
 * a filesystem that does not: EOPNOTSUPP to setting, getting and removing one,
 * and an empty list - which is how `ls -l`, `cp -a` and `tar` find out and
 * carry on. What Linux refuses before it asks the filesystem is refused first,
 * in its order: a flag setxattr does not have, a name that is empty or too
 * long (ERANGE), a value too large (E2BIG), and then the file itself - ENOENT
 * or EBADF beats "not supported", so a program that mistyped a path is told
 * that. `how` is the walk's flags for a path, or -1 when `where` is a
 * descriptor. */
static long linux_xattr_file(uint64_t where, int how) {
    vibeos_path_t w;
    vibeos_file_t *f;

    if (how >= 0) {
        return linux_walk_at(LINUX_CWD, where, (uint32_t)how, &w);
    }
    if (!(f = linux_file_get(where))) {
        return -VIBEOS_EBADF;
    }
    vibeos_file_put(f);
    return 0;
}

static long linux_xattr_name(uint64_t name_uptr) {
    char name[LINUX_XATTR_NAME_MAX + 2];
    uint32_t n = 0;

    if (ks_copy_user_string(name_uptr, name, (int)sizeof(name)) != 0) {
        return -VIBEOS_EFAULT;
    }
    while (name[n]) {
        n++;
    }
    return (n == 0u || n > LINUX_XATTR_NAME_MAX) ? -VIBEOS_ERANGE : 0;
}

static long linux_xattr_set(uint64_t where, int how, uint64_t name_uptr, uint64_t size,
                            uint64_t flags) {
    long r;

    if (flags & ~(uint64_t)(LINUX_XATTR_CREATE | LINUX_XATTR_REPLACE)) {
        return -VIBEOS_EINVAL;
    }
    if ((r = linux_xattr_name(name_uptr)) != 0) {
        return r;
    }
    if (size > LINUX_XATTR_SIZE_MAX) {
        return -VIBEOS_E2BIG;
    }
    r = linux_xattr_file(where, how);
    return r != 0 ? r : -VIBEOS_EOPNOTSUPP;
}

/* getxattr and removexattr: a name, then the file, then the refusal. */
static long linux_xattr_named(uint64_t where, int how, uint64_t name_uptr) {
    long r = linux_xattr_name(name_uptr);

    if (r == 0) {
        r = linux_xattr_file(where, how);
    }
    return r != 0 ? r : -VIBEOS_EOPNOTSUPP;
}

/* listxattr: no names, in no bytes. */
static long linux_xattr_list(uint64_t where, int how) {
    return linux_xattr_file(where, how);
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
    X(188, setxattr,    XATTR_SET,   NOPTR, linux_xattr_set(ARG(0), 0, ARG(1), ARG(3), ARG(4))) \
    X(189, lsetxattr,   XATTR_SET,   NOPTR, linux_xattr_set(ARG(0), VIBEOS_PATH_NOFOLLOW, ARG(1), ARG(3), ARG(4))) \
    X(190, fsetxattr,   XATTR_SET,   NOPTR, linux_xattr_set(ARG(0), -1, ARG(1), ARG(3), ARG(4))) \
    X(191, getxattr,    XATTR_GET,   NOPTR, linux_xattr_named(ARG(0), 0, ARG(1))) \
    X(192, lgetxattr,   XATTR_GET,   NOPTR, linux_xattr_named(ARG(0), VIBEOS_PATH_NOFOLLOW, ARG(1))) \
    X(193, fgetxattr,   XATTR_GET,   NOPTR, linux_xattr_named(ARG(0), -1, ARG(1))) \
    X(194, listxattr,   XATTR_LIST,  NOPTR, linux_xattr_list(ARG(0), 0)) \
    X(195, llistxattr,  XATTR_LIST,  NOPTR, linux_xattr_list(ARG(0), VIBEOS_PATH_NOFOLLOW)) \
    X(196, flistxattr,  XATTR_LIST,  NOPTR, linux_xattr_list(ARG(0), -1)) \
    X(197, removexattr, XATTR_REMOVE, NOPTR, linux_xattr_named(ARG(0), 0, ARG(1))) \
    X(198, lremovexattr, XATTR_REMOVE, NOPTR, linux_xattr_named(ARG(0), VIBEOS_PATH_NOFOLLOW, ARG(1))) \
    X(199, fremovexattr, XATTR_REMOVE, NOPTR, linux_xattr_named(ARG(0), -1, ARG(1))) \
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
