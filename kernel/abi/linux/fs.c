/* Linux ABI: files, descriptors and pipes.
 *
 * What a program does once it is running: look at files, read directories, talk
 * through pipes and the console. The list came from tracing BusyBox rather than
 * from reasoning about it.
 *
 * Lifted out of arch_hw.c (C4 stage 3). Since docs/abi/ A3 a descriptor names an
 * open file description (vibeos/file.h) and what each kind of file does is its
 * type's (kernel/abi/files/), so these handlers are the ABI's half only: find the
 * description, call its type, translate the answer. The `if` chains that decided
 * by hand whether a descriptor was a pipe, a socket or the console are gone. */

#include "linux_internal.h"

/* The *at calls interpret a relative path against a directory descriptor, or
 * against the working directory when that descriptor is AT_FDCWD.
 *
 * Reading it needs care. Arguments the Linux ABI types as `int` arrive in the
 * low half of a register, and writing a 32-bit register zeroes the upper half:
 * a caller doing `mov $-100, %edi` delivers 0x00000000ffffff9c, not
 * 0xffffffffffffff9c. Comparing the full 64 bits against -100 therefore never
 * matches, and every relative open fails with ENOSYS - which is exactly what
 * BusyBox reported as "can't open: Function not implemented". Read the low 32
 * bits and sign-extend, as the kernel this ABI belongs to does. */
#define AT_FDCWD           (-100)

#define AT_EMPTY_PATH      0x1000

#define AT_REMOVEDIR       0x200

/* struct stat, x86-64 layout. Byte offsets rather than a struct definition
 * because the layout is the ABI: it is fixed by Linux, not by this compiler. */
#define STAT_SIZE          144u

#define STAT_OFF_MODE       24u

#define STAT_OFF_NLINK      16u

#define STAT_OFF_UID        28u

#define STAT_OFF_GID        32u

#define STAT_OFF_SIZE       48u

#define STAT_OFF_BLKSIZE    56u

#define STAT_OFF_BLOCKS     64u

#define STAT_OFF_INO         8u

/* fcntl commands (asm-generic/fcntl.h). */
#define F_DUPFD          0
#define F_GETFD          1
#define F_SETFD          2
#define F_GETFL          3
#define F_SETFL          4
#define F_GETLK          5
#define F_SETLK          6
#define F_SETLKW         7
#define F_DUPFD_CLOEXEC  1030

/* The flags F_SETFL may change; the access mode and creation flags stay what
 * open made them, as on Linux. */
#define LINUX_SETFL_MASK (VIBEOS_O_APPEND | VIBEOS_O_NONBLOCK)

#define CLOSE_RANGE_UNSHARE (1u << 1)
#define CLOSE_RANGE_CLOEXEC (1u << 2)

/* ---- descriptors ----------------------------------------------------------------
 *
 * A call holds a reference to the description it works on for as long as it runs
 * - Linux's fdget/fdput - so a sibling thread closing the descriptor meanwhile
 * takes the number away and not the file: a read blocked on a pipe is not left
 * reading a slot somebody else has been handed. */

static vibeos_procstate_t *linux_cur_ps(void) {
    int me = ks_current();
    return me < 0 ? 0 : ks_ps(me);
}

vibeos_file_t *linux_file_get(uint64_t fd) {
    vibeos_procstate_t *ps = linux_cur_ps();
    vibeos_file_t *f;

    if (!ps) {
        return 0;
    }
    ks_lock(&ps->files_lock, __func__);
    f = vibeos_fdtable_get(&ps->files, fd);
    vibeos_file_get(f);
    ks_unlock(&ps->files_lock);
    return f;
}

/* Install a description at the lowest free number at or above `min`, taking over
 * the caller's reference; on failure the reference is released here. */
long linux_fd_install(vibeos_file_t *f, uint32_t fdflags, uint32_t min) {
    vibeos_procstate_t *ps = linux_cur_ps();
    int fd;

    if (!ps) {
        vibeos_file_put(f);
        return -VIBEOS_EINVAL;
    }
    ks_lock(&ps->files_lock, __func__);
    fd = vibeos_fdtable_install(&ps->files, f, fdflags, min);
    ks_unlock(&ps->files_lock);
    if (fd < 0) {
        vibeos_file_put(f);
        return fd == VIBEOS_FDT_NOMEM ? -VIBEOS_ENOMEM : -VIBEOS_EMFILE;
    }
    return fd;
}

/* Take a number out of the table and release its description - outside the
 * table's lock, because a release writes a file back or wakes a pipe's reader. */
long linux_fd_close(uint64_t fd) {
    vibeos_procstate_t *ps = linux_cur_ps();
    vibeos_file_t *f;

    if (!ps) {
        return -VIBEOS_EBADF;
    }
    ks_lock(&ps->files_lock, __func__);
    f = vibeos_fdtable_remove(&ps->files, fd);
    ks_unlock(&ps->files_lock);
    if (!f) {
        return -VIBEOS_EBADF;
    }
    vibeos_file_put(f);
    return 0;
}

/* ---- paths -----------------------------------------------------------------------
 *
 * Every call that takes a path makes it absolute here first (docs/abi/ A4):
 * against the directory `dirfd` names, or the working directory for AT_FDCWD,
 * and never above the process's root. What reaches a filesystem is then a path
 * inside a mount, found through the mount table - before A4 every path went to
 * the boot volume as the program wrote it. */

/* The working directory or the root, copied out under the lock that guards them:
 * a chdir in a sibling thread must not hand this one half a path. */
static void linux_ps_path(vibeos_procstate_t *ps, int which_root, char *out) {
    const char *src;
    uint32_t i;

    ks_lock(&ps->files_lock, __func__);
    src = which_root ? ps->root : ps->cwd;
    for (i = 0; i + 1u < VIBEOS_PATH_MAX && src[i]; i++) {
        out[i] = src[i];
    }
    out[i] = 0;
    ks_unlock(&ps->files_lock);
}

long linux_path_at(uint64_t dirfd, uint64_t upath, char *abs) {
    vibeos_procstate_t *ps = linux_cur_ps();
    char raw[VIBEOS_PATH_MAX + 1u];
    char base[VIBEOS_PATH_MAX], root[VIBEOS_PATH_MAX];
    uint32_t n = 0;

    if (!ps) {
        return -VIBEOS_EINVAL;
    }
    /* One byte more than a path may have, so a string that fills it is known to
     * be too long rather than silently cut to fit - the copy truncates. */
    if (ks_copy_user_string(upath, raw, (int)sizeof(raw)) != 0) {
        return -VIBEOS_EFAULT;
    }
    while (raw[n]) {
        n++;
    }
    if (n >= VIBEOS_PATH_MAX) {
        return -VIBEOS_ENAMETOOLONG;
    }
    linux_ps_path(ps, 1, root);
    if (raw[0] == '/' || VIBEOS_ARG_INT(dirfd) == AT_FDCWD) {
        linux_ps_path(ps, 0, base);
    } else {
        /* A relative path against a directory descriptor: the directory it
         * was opened on, which its description remembers whole. */
        vibeos_file_t *f = linux_file_get(dirfd);
        uint32_t i;
        if (!f) {
            return -VIBEOS_EBADF;
        }
        if (f->ops != &vibeos_fops_dir) {
            vibeos_file_put(f);
            return -VIBEOS_ENOTDIR;
        }
        for (i = 0; i + 1u < VIBEOS_PATH_MAX && f->path[i]; i++) {
            base[i] = f->path[i];
        }
        base[i] = 0;
        vibeos_file_put(f);
    }
    return vibeos_path_normalize(root, base, raw, abs, VIBEOS_PATH_MAX);
}

/* ---- read and write -------------------------------------------------------------- */

static long linux_sys_read(uint64_t fd, uint64_t buf, uint64_t len) {
    vibeos_file_t *f = linux_file_get(fd);
    long r;

    if (!f) {
        return -VIBEOS_EBADF;
    }
    if (f->ops->read) {
        r = f->ops->read(f, buf, len);
    } else {
        r = (f->ops == &vibeos_fops_dir) ? -VIBEOS_EISDIR : -VIBEOS_EINVAL;
    }
    vibeos_file_put(f);
    return r;
}

static long linux_sys_write(uint64_t fd, uint64_t buf, uint64_t len) {
    vibeos_file_t *f = linux_file_get(fd);
    long r;

    if (!f) {
        return -VIBEOS_EBADF;
    }
    r = f->ops->write ? f->ops->write(f, buf, len) : -VIBEOS_EBADF;
    vibeos_file_put(f);
    return r;
}

/* writev()/readv(): scatter-gather over the single-buffer paths. The iovec array
 * is itself user memory, so it is validated like any other user pointer before
 * being walked. */
typedef struct {
    uint64_t base;
    uint64_t len;
} linux_iovec_t;

static long linux_sys_writev(uint64_t fd, uint64_t iov_uptr, uint64_t iovcnt) {
    long total = 0;
    uint64_t i;

    if (iovcnt > 1024u) {
        return -VIBEOS_EINVAL;   /* Linux caps this at UIO_MAXIOV */
    }
    for (i = 0; i < iovcnt; i++) {
        linux_iovec_t v;
        long n;
        /* Each descriptor copied into the kernel before base/len are read: the
         * array-wide range check and these reads are two instants, and a
         * sibling munmap of the array page faults in ring 0 otherwise (H-020). */
        if (vibeos_uaccess_copy(&v, (const void *)(uintptr_t)
                (iov_uptr + i * sizeof(linux_iovec_t)), sizeof(v)) != 0) {
            return total > 0 ? total : -VIBEOS_EFAULT;
        }
        if (v.len == 0u) {
            continue;
        }
        if (!linux_user_ok(v.base, v.len, 0)) {
            return total > 0 ? total : -VIBEOS_EFAULT;
        }
        n = linux_sys_write(fd, v.base, v.len);
        if (n < 0) {
            return total > 0 ? total : n;
        }
        total += n;
        if ((uint64_t)n < v.len) {
            break;   /* a short write ends the call, as it does on Linux */
        }
    }
    return total;
}

static long linux_sys_readv(uint64_t fd, uint64_t iov_uptr, uint64_t iovcnt) {
    long total = 0;
    uint64_t i;

    if (iovcnt > 1024u) {
        return -VIBEOS_EINVAL;
    }
    for (i = 0; i < iovcnt; i++) {
        linux_iovec_t v;
        long n;
        /* See writev: the iovec is copied in before base/len are read (H-020). */
        if (vibeos_uaccess_copy(&v, (const void *)(uintptr_t)
                (iov_uptr + i * sizeof(linux_iovec_t)), sizeof(v)) != 0) {
            return total > 0 ? total : -VIBEOS_EFAULT;
        }
        if (v.len == 0u) {
            continue;
        }
        if (!linux_user_ok(v.base, v.len, 1)) {
            return total > 0 ? total : -VIBEOS_EFAULT;
        }
        n = linux_sys_read(fd, v.base, v.len);
        if (n < 0) {
            return total > 0 ? total : n;
        }
        total += n;
        if ((uint64_t)n < v.len) {
            break;
        }
    }
    return total;
}

static long linux_sys_lseek(uint64_t fd, uint64_t off, uint64_t whence) {
    vibeos_file_t *f = linux_file_get(fd);
    long r;

    if (!f) {
        return -VIBEOS_EBADF;
    }
    /* A pipe, a socket or the console has no position: ESPIPE, as Linux says. */
    r = f->ops->seek ? f->ops->seek(f, (int64_t)off, (int)whence) : -VIBEOS_ESPIPE;
    vibeos_file_put(f);
    return r;
}

/* ---- opening and closing ------------------------------------------------------------ */

/* openat(dirfd, path, flags): resolve a file or directory and take a
 * descriptor. With a write flag the file is created or replaced when its last
 * descriptor goes. open() is openat(AT_FDCWD). */
static long linux_sys_openat(uint64_t dirfd, uint64_t path_uptr, uint64_t flags) {
    char abs[VIBEOS_PATH_MAX];
    vibeos_file_t *f;
    long err;

    if (ks_current() < 0 || !ks_id(ks_current())->is_user) {
        return -VIBEOS_EINVAL;
    }
    err = linux_path_at(dirfd, path_uptr, abs);
    if (err != 0) {
        return err;
    }
    f = vibeos_open_path(abs, (uint32_t)flags, &err);
    if (!f) {
        return err;
    }
    return linux_fd_install(f, (flags & VIBEOS_O_CLOEXEC) ? VIBEOS_FD_CLOEXEC : 0u, 0);
}

static long linux_sys_open(uint64_t path_uptr, uint64_t flags) {
    return linux_sys_openat((uint64_t)(uint32_t)AT_FDCWD, path_uptr, flags);
}

static long linux_sys_close(uint64_t fd) {
    return linux_fd_close(fd);
}

/* close_range(first, last, flags): close - or with CLOSE_RANGE_CLOEXEC, mark
 * close-on-exec - every descriptor in the range. What a program runs before it
 * execs something it does not trust with its descriptors. */
static long linux_sys_close_range(uint64_t first, uint64_t last, uint64_t flags) {
    vibeos_procstate_t *ps = linux_cur_ps();
    int hi;
    uint64_t fd;

    if (!ps || first > last || (flags & ~(uint64_t)(CLOSE_RANGE_UNSHARE | CLOSE_RANGE_CLOEXEC))) {
        return -VIBEOS_EINVAL;
    }
    if (flags & CLOSE_RANGE_UNSHARE) {
        /* Giving this thread a table of its own is unshare(CLONE_FILES), which
         * is not implemented; refusing is better than closing descriptors in a
         * table the other threads are still using. */
        return -VIBEOS_EINVAL;
    }
    ks_lock(&ps->files_lock, __func__);
    hi = vibeos_fdtable_highest(&ps->files);
    ks_unlock(&ps->files_lock);
    if (hi < 0 || first > (uint64_t)hi) {
        return 0;
    }
    if (last > (uint64_t)hi) {
        last = (uint64_t)hi;
    }
    for (fd = first; fd <= last; fd++) {
        if (flags & CLOSE_RANGE_CLOEXEC) {
            ks_lock(&ps->files_lock, __func__);
            if (vibeos_fdtable_get(&ps->files, fd)) {
                (void)vibeos_fdtable_set_flags(&ps->files, fd,
                    vibeos_fdtable_flags(&ps->files, fd) | VIBEOS_FD_CLOEXEC);
            }
            ks_unlock(&ps->files_lock);
        } else {
            (void)linux_fd_close(fd);
        }
    }
    return 0;
}

/* ---- pipes and duplicates -------------------------------------------------------------- */

/* pipe2(): two descriptors onto one buffer, read end first. */
static long linux_sys_pipe2(uint64_t fds_uptr, uint64_t flags) {
    vibeos_file_t *rd, *wr;
    uint32_t fdflags = (flags & VIBEOS_O_CLOEXEC) ? VIBEOS_FD_CLOEXEC : 0u;
    long r, rfd, wfd;
    int kfds[2];

    if (ks_current() < 0 || !ks_id(ks_current())->is_user) {
        return -VIBEOS_EINVAL;
    }
    if (flags & ~(uint64_t)(VIBEOS_O_CLOEXEC | VIBEOS_O_NONBLOCK)) {
        return -VIBEOS_EINVAL;
    }
    r = vibeos_open_pipe((uint32_t)flags, &rd, &wr);
    if (r != 0) {
        return r;
    }
    rfd = linux_fd_install(rd, fdflags, 0);
    if (rfd < 0) {
        vibeos_file_put(wr);
        return rfd;
    }
    wfd = linux_fd_install(wr, fdflags, 0);
    if (wfd < 0) {
        (void)linux_fd_close((uint64_t)rfd);
        return wfd;
    }
    kfds[0] = (int)rfd;
    kfds[1] = (int)wfd;
    /* Copied out fault-safe: a sibling munmap between the range check and this
     * write would fault in ring 0. On fault both descriptors are already
     * installed, so they are closed rather than leaked (uaccess follow-up to
     * 6a94a32). */
    if (vibeos_uaccess_copy((void *)(uintptr_t)fds_uptr, kfds, sizeof(kfds)) != 0) {
        (void)linux_fd_close((uint64_t)rfd);
        (void)linux_fd_close((uint64_t)wfd);
        return -VIBEOS_EFAULT;
    }
    return 0;
}

/* dup2 and dup3: make newfd name what oldfd names - the same description, so the
 * two share an offset. This is how a shell attaches a pipe to a program's
 * standard input or output without the program knowing. */
static long linux_dup_to(uint64_t oldfd, uint64_t newfd, uint32_t fdflags) {
    vibeos_procstate_t *ps = linux_cur_ps();
    vibeos_file_t *f, *old = 0;
    int r;

    if (!ps) {
        return -VIBEOS_EINVAL;
    }
    /* One critical section: the source read, the target replaced and the new
     * reference taken, so a sibling's close of either descriptor lands wholly
     * before or wholly after. Whatever the target named is released outside
     * the lock, because releasing a pipe end wakes whoever was waiting on it. */
    ks_lock(&ps->files_lock, __func__);
    f = vibeos_fdtable_get(&ps->files, oldfd);
    if (!f) {
        ks_unlock(&ps->files_lock);
        return -VIBEOS_EBADF;
    }
    vibeos_file_get(f);
    r = vibeos_fdtable_install_at(&ps->files, newfd, f, fdflags, &old);
    ks_unlock(&ps->files_lock);
    if (r != 0) {
        vibeos_file_put(f);
        return r == VIBEOS_FDT_NOMEM ? -VIBEOS_ENOMEM : -VIBEOS_EBADF;
    }
    if (old) {
        vibeos_file_put(old);
    }
    return (long)newfd;
}

static long linux_sys_dup2(uint64_t oldfd, uint64_t newfd) {
    if (oldfd == newfd) {
        /* Linux answers newfd if oldfd is open, EBADF if not, and changes
         * nothing - not even the close-on-exec flag. */
        vibeos_file_t *f = linux_file_get(oldfd);
        if (!f) {
            return -VIBEOS_EBADF;
        }
        vibeos_file_put(f);
        return (long)newfd;
    }
    return linux_dup_to(oldfd, newfd, 0);
}

static long linux_sys_dup3(uint64_t oldfd, uint64_t newfd, uint64_t flags) {
    if (oldfd == newfd || (flags & ~(uint64_t)VIBEOS_O_CLOEXEC)) {
        return -VIBEOS_EINVAL;
    }
    return linux_dup_to(oldfd, newfd, (flags & VIBEOS_O_CLOEXEC) ? VIBEOS_FD_CLOEXEC : 0u);
}

/* dup() and F_DUPFD: the lowest free number at or above `min`. */
static long linux_dup_from(uint64_t oldfd, uint32_t min, uint32_t fdflags) {
    vibeos_file_t *f = linux_file_get(oldfd);

    if (!f) {
        return -VIBEOS_EBADF;
    }
    return linux_fd_install(f, fdflags, min);   /* takes the reference get gave */
}

static long linux_sys_dup(uint64_t oldfd) {
    return linux_dup_from(oldfd, 0, 0);
}

/* fcntl(): descriptor flags, status flags and duplicates. Record locks are not
 * implemented and say so with ENOLCK - "no locks available" - rather than
 * pretend a lock was taken: SQLite relies on them for correctness. */
static long linux_sys_fcntl(uint64_t fd, uint64_t cmd, uint64_t arg) {
    vibeos_procstate_t *ps = linux_cur_ps();
    vibeos_file_t *f;
    long r;

    switch (VIBEOS_ARG_INT(cmd)) {
        case F_DUPFD:
        case F_DUPFD_CLOEXEC:
            if (arg >= VIBEOS_FD_MAX) {
                return -VIBEOS_EINVAL;
            }
            return linux_dup_from(fd, (uint32_t)arg,
                                  VIBEOS_ARG_INT(cmd) == F_DUPFD_CLOEXEC ? VIBEOS_FD_CLOEXEC : 0u);
        case F_GETFD:
        case F_SETFD:
            if (!ps) {
                return -VIBEOS_EBADF;
            }
            ks_lock(&ps->files_lock, __func__);
            if (!vibeos_fdtable_get(&ps->files, fd)) {
                r = -VIBEOS_EBADF;
            } else if (VIBEOS_ARG_INT(cmd) == F_GETFD) {
                r = (long)vibeos_fdtable_flags(&ps->files, fd);
            } else {
                r = vibeos_fdtable_set_flags(&ps->files, fd,
                                             (uint32_t)arg & VIBEOS_FD_CLOEXEC) == 0 ? 0 : -VIBEOS_EBADF;
            }
            ks_unlock(&ps->files_lock);
            return r;
        case F_GETFL:
            if (!(f = linux_file_get(fd))) {
                return -VIBEOS_EBADF;
            }
            r = (long)f->flags;
            vibeos_file_put(f);
            return r;
        case F_SETFL:
            if (!(f = linux_file_get(fd))) {
                return -VIBEOS_EBADF;
            }
            /* The description's, so every descriptor naming it sees the change:
             * that is what makes O_NONBLOCK on a dup'd pipe end mean anything. */
            f->flags = (f->flags & ~LINUX_SETFL_MASK) | ((uint32_t)arg & LINUX_SETFL_MASK);
            vibeos_file_put(f);
            return 0;
        case F_GETLK:
        case F_SETLK:
        case F_SETLKW:
            if (!(f = linux_file_get(fd))) {
                return -VIBEOS_EBADF;
            }
            vibeos_file_put(f);
            return -VIBEOS_ENOLCK;
        default:
            return -VIBEOS_EINVAL;
    }
}

/* ---- metadata ---------------------------------------------------------------------- */

static void linux_stat_wr64(uint8_t *base, uint32_t off, uint64_t v) {
    uint8_t *p = base + off;
    uint32_t i;
    for (i = 0; i < 8u; i++) {
        p[i] = (uint8_t)(v >> (8u * i));
    }
}

static void linux_stat_wr32(uint8_t *base, uint32_t off, uint32_t v) {
    uint8_t *p = base + off;
    uint32_t i;
    for (i = 0; i < 4u; i++) {
        p[i] = (uint8_t)(v >> (8u * i));
    }
}

/* Fill a struct stat the caller can believe. Assembled in the kernel and copied
 * out once: filling the user buffer field by field would fault in ring 0 if a
 * sibling munmaps it between the range check and any of these writes (uaccess
 * follow-up to 6a94a32, same class as H-026). */
static long linux_write_stat(uint64_t ubuf, const vibeos_file_stat_t *st) {
    uint8_t kbuf[STAT_SIZE];
    uint32_t i;

    for (i = 0; i < STAT_SIZE; i++) {
        kbuf[i] = 0;
    }
    linux_stat_wr64(kbuf, STAT_OFF_INO, st->ino);
    linux_stat_wr64(kbuf, STAT_OFF_NLINK, 1);
    linux_stat_wr32(kbuf, STAT_OFF_MODE, st->mode);
    linux_stat_wr32(kbuf, STAT_OFF_UID, 0);
    linux_stat_wr32(kbuf, STAT_OFF_GID, 0);
    linux_stat_wr64(kbuf, STAT_OFF_SIZE, st->size);
    linux_stat_wr64(kbuf, STAT_OFF_BLKSIZE, 512);
    linux_stat_wr64(kbuf, STAT_OFF_BLOCKS, vibeos_ceil_div_u64(st->size, 512ull));
    if (vibeos_uaccess_copy((void *)(uintptr_t)ubuf, kbuf, STAT_SIZE) != 0) {
        return -VIBEOS_EFAULT;
    }
    return 0;
}

/* fstat(): the type decides - a pipe is a FIFO and a socket a socket, where both
 * used to come out as whatever the table entry happened to resemble. */
static long linux_sys_fstat(uint64_t fd, uint64_t ubuf) {
    vibeos_file_t *f = linux_file_get(fd);
    vibeos_file_stat_t st;
    long r;

    if (!f) {
        return -VIBEOS_EBADF;
    }
    st.mode = 0;
    st.size = 0;
    st.ino = 0;
    r = f->ops->stat ? (long)f->ops->stat(f, &st) : 0;
    vibeos_file_put(f);
    return r < 0 ? r : linux_write_stat(ubuf, &st);
}

/* newfstatat(dirfd, path, buf, flags): stat by name, or by fd when the path is
 * empty and AT_EMPTY_PATH is set. */
static long linux_sys_newfstatat(uint64_t dirfd, uint64_t path_uptr, uint64_t ubuf,
                                 uint64_t flags) {
    char abs[VIBEOS_PATH_MAX];
    vibeos_file_stat_t st;
    vibeos_fs_node_t node;
    vibeos_fsmount_t *mnt;
    const char *tail;
    char first = 0;
    long r;

    if (vibeos_uaccess_copy(&first, (const void *)(uintptr_t)path_uptr, 1u) != 0) {
        return -VIBEOS_EFAULT;
    }
    if (first == 0) {
        if ((flags & AT_EMPTY_PATH) == 0) {
            return -VIBEOS_ENOENT;
        }
        return linux_sys_fstat(dirfd, ubuf);
    }
    r = linux_path_at(dirfd, path_uptr, abs);
    if (r != 0) {
        return r;
    }
    /* Directory or file? The answer changes what a program does, not just what
     * it prints: ls given a directory lists it and given a file names it, so
     * reporting the wrong one produces a plausible wrong result rather than an
     * error. The filesystem decides; how it decides is its business. */
    r = vibeos_path_lookup(abs, &mnt, &tail, &node);
    if (r != 0) {
        return r;
    }
    st.mode = node.is_dir ? (VIBEOS_S_IFDIR | 0755u) : (VIBEOS_S_IFREG | 0644u);
    st.size = node.is_dir ? 0u : node.size;   /* 64-bit: do not narrow (M-018) */
    st.ino = node.id ? node.id : 2u;
    return linux_write_stat(ubuf, &st);
}

/* getdents64(fd, buf, len): dirent64 records from the directory the descriptor
 * names, so user space can list a directory. */
static long linux_sys_getdents64(uint64_t fd, uint64_t buf, uint64_t len) {
    vibeos_file_t *f = linux_file_get(fd);
    long r;

    if (!f) {
        return -VIBEOS_EBADF;
    }
    r = f->ops->getdents ? f->ops->getdents(f, buf, len) : -VIBEOS_ENOTDIR;
    vibeos_file_put(f);
    return r;
}

/* ioctl(): the type answers. Only the console answers anything, and only the
 * process-group questions; ENOTTY is the truthful answer everywhere else, and it
 * is the answer a libc uses to decide stdout is a file or a pipe and should be
 * block buffered. */
static long linux_sys_ioctl(uint64_t fd, uint64_t req, uint64_t arg) {
    vibeos_file_t *f = linux_file_get(fd);
    long r;

    if (!f) {
        return -VIBEOS_EBADF;
    }
    r = f->ops->ioctl ? f->ops->ioctl(f, req, arg) : -VIBEOS_ENOTTY;
    vibeos_file_put(f);
    return r;
}

/* ---- paths ----------------------------------------------------------------------------- */

/* unlinkat(dirfd, path, flags) and unlink(path). A directory is EISDIR, as
 * Linux answers unlink on one; AT_REMOVEDIR asks for rmdir, which no filesystem
 * here implements yet, and is refused rather than approximated. */
static long linux_sys_unlinkat(uint64_t dirfd, uint64_t path_uptr, uint64_t flags) {
    char abs[VIBEOS_PATH_MAX];
    vibeos_fs_node_t node;
    vibeos_fsmount_t *mnt;
    const char *tail;
    long r;

    if (flags & ~(uint64_t)AT_REMOVEDIR) {
        return -VIBEOS_EINVAL;
    }
    if (flags & AT_REMOVEDIR) {
        return -VIBEOS_EINVAL;
    }
    r = linux_path_at(dirfd, path_uptr, abs);
    if (r != 0) {
        return r;
    }
    r = vibeos_path_lookup(abs, &mnt, &tail, &node);
    if (r != 0) {
        return r;
    }
    if (node.is_dir) {
        return -VIBEOS_EISDIR;
    }
    return (vibeos_fs_unlink(mnt, tail) == 0) ? 0 : -VIBEOS_EIO;
}

static long linux_sys_unlink(uint64_t path_uptr) {
    return linux_sys_unlinkat((uint64_t)(uint32_t)AT_FDCWD, path_uptr, 0);
}

/* mkdirat(dirfd, path, mode) and mkdir(path, mode). The mode is not kept: there
 * is one user and no permission bits on these filesystems. */
static long linux_sys_mkdirat(uint64_t dirfd, uint64_t path_uptr) {
    char abs[VIBEOS_PATH_MAX];
    vibeos_fs_node_t node;
    vibeos_fsmount_t *mnt;
    const char *tail;
    long r;

    r = linux_path_at(dirfd, path_uptr, abs);
    if (r != 0) {
        return r;
    }
    r = vibeos_path_parent(abs, &mnt, &tail);
    if (r != 0) {
        return r;
    }
    if (vibeos_fs_lookup(mnt, tail, &node) == 0) {
        return -VIBEOS_EEXIST;
    }
    return (vibeos_fs_mkdir(mnt, tail) == 0) ? 0 : -VIBEOS_EIO;
}

static long linux_sys_mkdir(uint64_t path_uptr) {
    return linux_sys_mkdirat((uint64_t)(uint32_t)AT_FDCWD, path_uptr);
}

/* chdir(path) and fchdir(fd): the working directory, which relative paths start
 * from. It must be a directory that exists now; it is kept as a path, so a
 * directory removed later leaves a working directory that names nothing, and the
 * next relative lookup says ENOENT - as on Linux. */
static long linux_set_cwd(const char *abs) {
    vibeos_procstate_t *ps = linux_cur_ps();
    vibeos_fs_node_t node;
    vibeos_fsmount_t *mnt;
    const char *tail;
    uint32_t i;
    long r;

    if (!ps) {
        return -VIBEOS_EINVAL;
    }
    r = vibeos_path_lookup(abs, &mnt, &tail, &node);
    if (r != 0) {
        return r;
    }
    if (!node.is_dir) {
        return -VIBEOS_ENOTDIR;
    }
    ks_lock(&ps->files_lock, __func__);
    for (i = 0; i + 1u < VIBEOS_PATH_MAX && abs[i]; i++) {
        ps->cwd[i] = abs[i];
    }
    ps->cwd[i] = 0;
    ks_unlock(&ps->files_lock);
    return 0;
}

static long linux_sys_chdir(uint64_t path_uptr) {
    char abs[VIBEOS_PATH_MAX];
    long r = linux_path_at((uint64_t)(uint32_t)AT_FDCWD, path_uptr, abs);

    return r != 0 ? r : linux_set_cwd(abs);
}

static long linux_sys_fchdir(uint64_t fd) {
    vibeos_file_t *f = linux_file_get(fd);
    char abs[VIBEOS_PATH_MAX];
    uint32_t i;

    if (!f) {
        return -VIBEOS_EBADF;
    }
    if (f->ops != &vibeos_fops_dir) {
        vibeos_file_put(f);
        return -VIBEOS_ENOTDIR;
    }
    for (i = 0; i + 1u < VIBEOS_PATH_MAX && f->path[i]; i++) {
        abs[i] = f->path[i];
    }
    abs[i] = 0;
    vibeos_file_put(f);
    return linux_set_cwd(abs);
}

/* getcwd(): the working directory, and ERANGE when it does not fit - which is
 * how a C library learns to try a bigger buffer. Linux returns the length with
 * its terminator. */
static long linux_sys_getcwd(uint64_t ubuf, uint64_t size) {
    vibeos_procstate_t *ps = linux_cur_ps();
    char cwd[VIBEOS_PATH_MAX];
    uint64_t n = 0;

    if (!ps) {
        return -VIBEOS_EINVAL;
    }
    linux_ps_path(ps, 0, cwd);
    while (cwd[n]) {
        n++;
    }
    if (size < n + 1u) {
        return -VIBEOS_ERANGE;
    }
    if (!linux_user_ok(ubuf, n + 1u, 1)) {
        return -VIBEOS_EFAULT;
    }
    /* Fault-safe copy out: a sibling munmap between the check and the write
     * would fault in ring 0 (uaccess follow-up to 6a94a32). */
    if (vibeos_uaccess_copy((void *)(uintptr_t)ubuf, cwd, n + 1u) != 0) {
        return -VIBEOS_EFAULT;
    }
    return (long)(n + 1u);
}

/* readlinkat(): the only symlink that exists here is the one a program uses to
 * find itself, and it is answered from what execve was actually given rather
 * than from a made-up path. Everything else is not a link, which is what
 * EINVAL means. */
static long linux_sys_readlinkat(uint64_t dirfd, uint64_t path_uptr, uint64_t ubuf,
                                 uint64_t bufsz) {
    char path[VIBEOS_PATH_MAX];
    const char *self;
    uint64_t n = 0;
    long r;

    /* Made absolute first, so "self/exe" from /proc and "/proc/./self/exe" are
     * the same question as the one programs usually ask. */
    r = linux_path_at(dirfd, path_uptr, path);
    if (r != 0) {
        return r;
    }
    if (!(path[0] == '/' && path[1] == 'p' && path[2] == 'r' && path[3] == 'o' &&
          path[4] == 'c' && path[5] == '/' && path[6] == 's' && path[7] == 'e' &&
          path[8] == 'l' && path[9] == 'f' && path[10] == '/' && path[11] == 'e' &&
          path[12] == 'x' && path[13] == 'e' && path[14] == 0)) {
        return -VIBEOS_EINVAL;
    }
    if (ks_current() < 0) {
        return -VIBEOS_EINVAL;
    }
    self = ks_image(ks_current())->exe_path;
    while (self[n]) {
        n++;
    }
    if (n == 0) {
        return -VIBEOS_ENOENT;
    }
    if (n > bufsz) {
        n = bufsz;
    }
    if (!linux_user_ok(ubuf, n, 1)) {
        return -VIBEOS_EFAULT;
    }
    /* self is a kernel string; copy out fault-safe so a sibling munmap between
     * the check and the write cannot fault in ring 0 (uaccess follow-up). */
    if (vibeos_uaccess_copy((void *)(uintptr_t)ubuf, self, n) != 0) {
        return -VIBEOS_EFAULT;
    }
    return (long)n;   /* not terminated, as Linux does not terminate it */
}

/* ---- what fork, exec and exit do to a table ------------------------------------------------ */

/* A new process inherits another's descriptors: a copy of the table, every
 * description in it one reference fuller. fork and exec copy; a thread shares, so
 * clone does neither. Under the source's lock, because a sibling of the forking
 * thread may be opening or closing in the same table. The destination is new and
 * nobody else can see it yet. 0, or -ENOMEM with the destination empty. */
int linux_fds_copy(vibeos_procstate_t *dst, vibeos_procstate_t *src) {
    int r;

    if (!dst || !src) {
        return -VIBEOS_EINVAL;
    }
    ks_lock(&src->files_lock, __func__);
    r = vibeos_fdtable_copy(&dst->files, &src->files);
    ks_unlock(&src->files_lock);
    return r == 0 ? 0 : -VIBEOS_ENOMEM;
}

/* A thread has finished with its process's table: exit, or exec moving to a
 * copy. The last one to leave closes everything, and returns 1 so exit knows the
 * process's sockets go too.
 *
 * Exiting closes everything, and for a pipe that is not tidiness: the reader at
 * the other end is waiting for its writers to reach zero, and a program that
 * produced its output and exited without closing is the normal case. Leaving
 * the count high is how ls | wc -l prints nothing and hangs. */
int linux_files_leave(vibeos_procstate_t *ps) {
    uint32_t n;

    if (!ps) {
        return 0;
    }
    /* Never below zero. A thread that leaves without having been counted -
     * a clone that forgot the increment - would otherwise take the count to
     * zero early, close the table under the threads still using it, and then
     * wrap it so that nobody ever closes it again. The early close is visible
     * once, in whichever thread happens to be first; the wrap is not visible
     * at all. So the underflow is counted and gated instead: the mechanism
     * rather than one lucky symptom. */
    for (;;) {
        n = __atomic_load_n(&ps->files_users, __ATOMIC_ACQUIRE);
        if (n == 0u) {
            vibeos_task_stats()->files_double_leave++;
            return 0;
        }
        if (__atomic_compare_exchange_n(&ps->files_users, &n, n - 1u, 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
            break;
        }
    }
    if (n != 1u) {
        return 0;
    }
    /* Nobody else uses this table now - the count was the last thing that said
     * somebody might - so no lock. */
    vibeos_fdtable_destroy(&ps->files);
    return 1;
}

/* ---- the syscalls this file implements ---------------------------------------
 *
 *   sendfile  every caller has to cope with it failing, and does: a read-and-write
 *             loop is the documented fallback. Refusing is therefore free, while
 *             serving it would mean a second copy of the file and console paths
 *             purely to move bytes between kernel buffers.
 *   pipe      is pipe2 with no flags.
 *   ioctl     the row keeps the console's two pointer descriptors, so a bad
 *             pointer is refused before the type is asked. */
#define TIOCGPGRP 0x540Fu
#define TIOCSPGRP 0x5410u

#define LINUX_FS_SYSCALLS(X) \
    X(0,   read,        READ,        PTRS(OUT_BUF(1, 2)), linux_sys_read(ARG(0), ARG(1), ARG(2))) \
    X(1,   write,       WRITE,       PTRS(IN_BUF(1, 2)), linux_sys_write(ARG(0), ARG(1), ARG(2))) \
    X(2,   open,        OPEN,        NOPTR, linux_sys_open(ARG(0), ARG(1))) \
    X(3,   close,       CLOSE,       NOPTR, linux_sys_close(ARG(0))) \
    X(5,   fstat,       FSTAT,       PTRS(OUT(1, STAT_SIZE)), linux_sys_fstat(ARG(0), ARG(1))) \
    X(8,   lseek,       LSEEK,       NOPTR, linux_sys_lseek(ARG(0), ARG(1), ARG(2))) \
    X(16,  ioctl,       IOCTL,       PTRS(OUT_IF(1, TIOCGPGRP, 2, sizeof(uint32_t)), IN_IF(1, TIOCSPGRP, 2, sizeof(uint32_t))), linux_sys_ioctl(ARG(0), ARG(1), ARG(2))) \
    X(19,  readv,       READV,       PTRS(IN_VEC(1, 2, sizeof(linux_iovec_t), 1024)), linux_sys_readv(ARG(0), ARG(1), ARG(2))) \
    X(20,  writev,      WRITEV,      PTRS(IN_VEC(1, 2, sizeof(linux_iovec_t), 1024)), linux_sys_writev(ARG(0), ARG(1), ARG(2))) \
    X(22,  pipe,        PIPE,        PTRS(OUT(0, 8)), linux_sys_pipe2(ARG(0), 0)) \
    X(32,  dup,         DUP,         NOPTR, linux_sys_dup(ARG(0))) \
    X(33,  dup2,        DUP2,        NOPTR, linux_sys_dup2(ARG(0), ARG(1))) \
    X(40,  sendfile,    SENDFILE,    NOPTR, -VIBEOS_ENOSYS) \
    X(72,  fcntl,       FCNTL,       NOPTR, linux_sys_fcntl(ARG(0), ARG(1), ARG(2))) \
    X(79,  getcwd,      GETCWD,      NOPTR, linux_sys_getcwd(ARG(0), ARG(1))) \
    X(80,  chdir,       CHDIR,       NOPTR, linux_sys_chdir(ARG(0))) \
    X(81,  fchdir,      FCHDIR,      NOPTR, linux_sys_fchdir(ARG(0))) \
    X(83,  mkdir,       MKDIR,       NOPTR, linux_sys_mkdir(ARG(0))) \
    X(87,  unlink,      UNLINK,      NOPTR, linux_sys_unlink(ARG(0))) \
    X(217, getdents64,  GETDENTS,    PTRS(OUT_BUF(1, 2)), linux_sys_getdents64(ARG(0), ARG(1), ARG(2))) \
    X(257, openat,      OPEN_AT,     NOPTR, linux_sys_openat(ARG(0), ARG(1), ARG(2))) \
    X(258, mkdirat,     MKDIR_AT,    NOPTR, linux_sys_mkdirat(ARG(0), ARG(1))) \
    X(262, newfstatat,  STAT_AT,     PTRS(OUT(2, STAT_SIZE)), linux_sys_newfstatat(ARG(0), ARG(1), ARG(2), ARG(3))) \
    X(263, unlinkat,    UNLINK_AT,   NOPTR, linux_sys_unlinkat(ARG(0), ARG(1), ARG(2))) \
    X(267, readlinkat,  READLINK_AT, NOPTR, linux_sys_readlinkat(ARG(0), ARG(1), ARG(2), ARG(3))) \
    X(292, dup3,        DUP3,        NOPTR, linux_sys_dup3(ARG(0), ARG(1), ARG(2))) \
    X(293, pipe2,       PIPE2,       PTRS(OUT(0, 8)), linux_sys_pipe2(ARG(0), ARG(1))) \
    X(436, close_range, CLOSE_RANGE, NOPTR, linux_sys_close_range(ARG(0), ARG(1), ARG(2)))

LINUX_DEFINE_SYSCALLS(fs, LINUX_FS_SYSCALLS)
