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
 * bits and sign-extend, as the kernel this ABI belongs to does. The constants,
 * and struct stat's layout, are in vibeos/linux_layout.h (docs/abi/ A5). */

/* The flags F_SETFL may change; the access mode and creation flags stay what
 * open made them, as on Linux. */
#define LINUX_SETFL_MASK (VIBEOS_O_APPEND | VIBEOS_O_NONBLOCK)

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

/* What a walk starts from: the path as the program wrote it, the process's root,
 * and the directory a relative path is relative to. */
static long linux_walk_inputs(uint64_t dirfd, uint64_t upath, char *raw, char *root,
                              char *base) {
    vibeos_procstate_t *ps = linux_cur_ps();
    uint32_t n = 0;

    if (!ps) {
        return -VIBEOS_EINVAL;
    }
    /* One byte more than a path may have, so a string that fills it is known to
     * be too long rather than silently cut to fit - the copy truncates. */
    if (ks_copy_user_string(upath, raw, (int)VIBEOS_PATH_MAX + 1) != 0) {
        return -VIBEOS_EFAULT;
    }
    while (raw[n]) {
        n++;
    }
    if (n >= VIBEOS_PATH_MAX) {
        return -VIBEOS_ENAMETOOLONG;
    }
    linux_ps_path(ps, 1, root);
    if (raw[0] == '/' || VIBEOS_ARG_INT(dirfd) == LINUX_AT_FDCWD) {
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
    return 0;
}

long linux_walk_at(uint64_t dirfd, uint64_t upath, uint32_t flags, vibeos_path_t *w) {
    char raw[VIBEOS_PATH_MAX + 1u];
    char base[VIBEOS_PATH_MAX], root[VIBEOS_PATH_MAX];
    long r = linux_walk_inputs(dirfd, upath, raw, root, base);

    return r != 0 ? r : vibeos_path_walk(root, base, raw, flags, w);
}

/* Is this path /proc/self/exe? /proc does not exist, so the question is asked
 * of the path as written, made absolute - "self/exe" from /proc and
 * "/proc/./self/exe" are the one programs usually ask. */
static int linux_is_proc_self_exe(uint64_t dirfd, uint64_t upath) {
    char raw[VIBEOS_PATH_MAX + 1u];
    char base[VIBEOS_PATH_MAX], root[VIBEOS_PATH_MAX], abs[VIBEOS_PATH_MAX];
    const char *want = "/proc/self/exe";
    uint32_t i;

    if (linux_walk_inputs(dirfd, upath, raw, root, base) != 0 ||
        vibeos_path_normalize(root, base, raw, abs, VIBEOS_PATH_MAX) != 0) {
        return 0;
    }
    for (i = 0; want[i] && abs[i] == want[i]; i++) {
    }
    return want[i] == 0 && abs[i] == 0;
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
        if (v.iov_len == 0u) {
            continue;
        }
        if (!linux_user_ok(v.iov_base, v.iov_len, 0)) {
            return total > 0 ? total : -VIBEOS_EFAULT;
        }
        n = linux_sys_write(fd, v.iov_base, v.iov_len);
        if (n < 0) {
            return total > 0 ? total : n;
        }
        total += n;
        if ((uint64_t)n < v.iov_len) {
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
        if (v.iov_len == 0u) {
            continue;
        }
        if (!linux_user_ok(v.iov_base, v.iov_len, 1)) {
            return total > 0 ? total : -VIBEOS_EFAULT;
        }
        n = linux_sys_read(fd, v.iov_base, v.iov_len);
        if (n < 0) {
            return total > 0 ? total : n;
        }
        total += n;
        if ((uint64_t)n < v.iov_len) {
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
/* The permission bits a new file may not have. */
static uint32_t linux_umask(void) {
    vibeos_procstate_t *ps = linux_cur_ps();
    return ps ? ps->umask : 022u;
}

static long linux_sys_openat(uint64_t dirfd, uint64_t path_uptr, uint64_t flags, uint64_t mode) {
    vibeos_path_t w;
    vibeos_file_t *f;
    long err;

    if (ks_current() < 0 || !ks_id(ks_current())->is_user) {
        return -VIBEOS_EINVAL;
    }
    err = linux_walk_at(dirfd, path_uptr, vibeos_open_walk_flags((uint32_t)flags), &w);
    if (err != 0) {
        return err;
    }
    f = vibeos_open_path(&w, (uint32_t)flags, (uint32_t)mode & ~linux_umask(), &err);
    if (!f) {
        return err;
    }
    return linux_fd_install(f, (flags & VIBEOS_O_CLOEXEC) ? VIBEOS_FD_CLOEXEC : 0u, 0);
}

static long linux_sys_open(uint64_t path_uptr, uint64_t flags, uint64_t mode) {
    return linux_sys_openat((uint64_t)(uint32_t)LINUX_AT_FDCWD, path_uptr, flags, mode);
}

/* creat(path, mode): open to write a new or emptied file. */
static long linux_sys_creat(uint64_t path_uptr, uint64_t mode) {
    return linux_sys_openat((uint64_t)(uint32_t)LINUX_AT_FDCWD, path_uptr,
                            VIBEOS_O_CREAT | VIBEOS_O_WRONLY | VIBEOS_O_TRUNC, mode);
}

/* umask(mask): the new one in, the old one out. It cannot fail. */
static long linux_sys_umask(uint64_t mask) {
    vibeos_procstate_t *ps = linux_cur_ps();
    uint32_t old;

    if (!ps) {
        return 022;
    }
    ks_lock(&ps->files_lock, __func__);
    old = ps->umask;
    ps->umask = (uint32_t)mask & 0777u;
    ks_unlock(&ps->files_lock);
    return (long)old;
}

/* ---- at an offset (L1) ---------------------------------------------------------------
 *
 * pread64 and pwrite64 leave the description's position alone; a file type
 * without positions - a pipe, a socket, the console - is ESPIPE, as on Linux. */

static long linux_sys_pread64(uint64_t fd, uint64_t buf, uint64_t len, uint64_t off) {
    vibeos_file_t *f;
    long r;

    if ((int64_t)off < 0) {
        return -VIBEOS_EINVAL;
    }
    if (!(f = linux_file_get(fd))) {
        return -VIBEOS_EBADF;
    }
    r = f->ops->pread ? f->ops->pread(f, buf, len, off) : -VIBEOS_ESPIPE;
    vibeos_file_put(f);
    return r;
}

static long linux_sys_pwrite64(uint64_t fd, uint64_t buf, uint64_t len, uint64_t off) {
    vibeos_file_t *f;
    long r;

    if ((int64_t)off < 0) {
        return -VIBEOS_EINVAL;
    }
    if (!(f = linux_file_get(fd))) {
        return -VIBEOS_EBADF;
    }
    r = f->ops->pwrite ? f->ops->pwrite(f, buf, len, off) : -VIBEOS_ESPIPE;
    vibeos_file_put(f);
    return r;
}

/* preadv and pwritev, and their "2" forms: the vector calls at an offset. An
 * offset of -1 in the "2" forms means the description's own position, which is
 * readv and writev; any RWF_ flag is refused rather than ignored - each one
 * promises something (no blocking, durability) this kernel would not keep. */
static long linux_rw_vec_at(uint64_t fd, uint64_t iov_uptr, uint64_t iovcnt, uint64_t off,
                            int write) {
    long total = 0;
    uint64_t i;

    if (iovcnt > 1024u) {
        return -VIBEOS_EINVAL;
    }
    if ((int64_t)off < 0) {
        return -VIBEOS_EINVAL;
    }
    for (i = 0; i < iovcnt; i++) {
        linux_iovec_t v;
        long n;
        /* Copied in before base and len are read (H-020), as readv does. */
        if (vibeos_uaccess_copy(&v, (const void *)(uintptr_t)
                (iov_uptr + i * sizeof(linux_iovec_t)), sizeof(v)) != 0) {
            return total > 0 ? total : -VIBEOS_EFAULT;
        }
        if (v.iov_len == 0u) {
            continue;
        }
        if (!linux_user_ok(v.iov_base, v.iov_len, write ? 0 : 1)) {
            return total > 0 ? total : -VIBEOS_EFAULT;
        }
        n = write ? linux_sys_pwrite64(fd, v.iov_base, v.iov_len, off + (uint64_t)total)
                  : linux_sys_pread64(fd, v.iov_base, v.iov_len, off + (uint64_t)total);
        if (n < 0) {
            return total > 0 ? total : n;
        }
        total += n;
        if ((uint64_t)n < v.iov_len) {
            break;
        }
    }
    return total;
}

static long linux_sys_readv(uint64_t fd, uint64_t iov_uptr, uint64_t iovcnt);
static long linux_sys_writev(uint64_t fd, uint64_t iov_uptr, uint64_t iovcnt);

static long linux_sys_preadv2(uint64_t fd, uint64_t iov, uint64_t cnt, uint64_t off, uint64_t flags) {
    if (flags != 0u) {
        return -VIBEOS_EOPNOTSUPP;
    }
    return (int64_t)off == -1 ? linux_sys_readv(fd, iov, cnt) : linux_rw_vec_at(fd, iov, cnt, off, 0);
}

static long linux_sys_pwritev2(uint64_t fd, uint64_t iov, uint64_t cnt, uint64_t off, uint64_t flags) {
    if (flags != 0u) {
        return -VIBEOS_EOPNOTSUPP;
    }
    return (int64_t)off == -1 ? linux_sys_writev(fd, iov, cnt) : linux_rw_vec_at(fd, iov, cnt, off, 1);
}

/* ---- sizes and durability (L1) ------------------------------------------------------ */

static long linux_sys_ftruncate(uint64_t fd, uint64_t len) {
    vibeos_file_t *f;
    long r;

    if ((int64_t)len < 0) {
        return -VIBEOS_EINVAL;
    }
    if (!(f = linux_file_get(fd))) {
        return -VIBEOS_EBADF;
    }
    r = f->ops->truncate ? f->ops->truncate(f, len) : -VIBEOS_EINVAL;
    vibeos_file_put(f);
    return r;
}

static long linux_sys_truncate(uint64_t path_uptr, uint64_t len) {
    vibeos_path_t w;
    long r;

    if ((int64_t)len < 0) {
        return -VIBEOS_EINVAL;
    }
    r = linux_walk_at((uint64_t)(uint32_t)LINUX_AT_FDCWD, path_uptr, 0u, &w);
    if (r != 0) {
        return r;
    }
    if (w.node.is_dir) {
        return -VIBEOS_EISDIR;
    }
    if ((w.node.mode & VIBEOS_S_IFMT) != VIBEOS_S_IFREG) {
        return -VIBEOS_EINVAL;
    }
    r = vibeos_fs_truncate(w.mnt, &w.node, len);
    if (r == -VIBEOS_EOPNOTSUPP && len == 0u) {
        /* A whole-file writer can still empty a file: write it with nothing. */
        r = vibeos_fs_write_file(w.mnt, w.tail, "", 0) >= 0 ? 0 : -VIBEOS_EIO;
    }
    return r;
}

/* fsync and fdatasync: the file's data is on the medium. One answer for both -
 * no filesystem here separates metadata from data. */
static long linux_sys_fsync(uint64_t fd) {
    vibeos_file_t *f = linux_file_get(fd);
    long r;

    if (!f) {
        return -VIBEOS_EBADF;
    }
    r = f->ops->sync ? f->ops->sync(f) : -VIBEOS_EINVAL;   /* a pipe, a socket: EINVAL */
    vibeos_file_put(f);
    return r;
}

/* sync(): every mounted filesystem. It reports nothing, as on Linux. */
static long linux_sys_sync(void) {
    uint32_t i;
    for (i = 0; i < vibeos_fs_mount_count(); i++) {
        vibeos_fsmount_t *m = vibeos_fs_mount_at(i);
        if (m) {
            (void)vibeos_fs_sync(m);
        }
    }
    return 0;
}

/* syncfs(fd): the filesystem the descriptor is on. */
static long linux_sys_syncfs(uint64_t fd) {
    vibeos_file_t *f = linux_file_get(fd);
    long r;

    if (!f) {
        return -VIBEOS_EBADF;
    }
    r = f->mnt ? vibeos_fs_sync(f->mnt) : 0;
    vibeos_file_put(f);
    return r;
}

/* fallocate(fd, mode, off, len). Mode 0 makes the file at least off + len long;
 * the space itself is not reserved - tmpfs has holes and FAT has no way to
 * promise - so what this guarantees is the size, which is what programs that
 * call it to extend a file use it for. KEEP_SIZE then has nothing to do. Hole
 * punching and the rest are refused. */
static long linux_sys_fallocate(uint64_t fd, uint64_t mode, uint64_t off, uint64_t len) {
    vibeos_file_t *f;
    vibeos_file_stat_t st;
    long r = 0;

    if ((int64_t)off < 0 || (int64_t)len <= 0) {
        return -VIBEOS_EINVAL;
    }
    if (mode & ~(uint64_t)LINUX_FALLOC_FL_KEEP_SIZE) {
        return -VIBEOS_EOPNOTSUPP;
    }
    if (!(f = linux_file_get(fd))) {
        return -VIBEOS_EBADF;
    }
    if (!f->ops->truncate || !f->ops->stat) {
        r = f->ops->pread ? -VIBEOS_EINVAL : -VIBEOS_ESPIPE;
    } else if ((f->flags & VIBEOS_O_ACCMODE) == VIBEOS_O_RDONLY) {
        r = -VIBEOS_EBADF;
    } else if (!(mode & LINUX_FALLOC_FL_KEEP_SIZE)) {
        vibeos_file_stat_clear(&st);
        (void)f->ops->stat(f, &st);
        if (st.size < off + len) {
            r = f->ops->truncate(f, off + len);
        }
    }
    vibeos_file_put(f);
    return r;
}

/* fadvise64(fd, off, len, advice): advice about a cache this kernel does not
 * tune. Accepted, because it is advice; a pipe is ESPIPE and an advice Linux
 * does not have is EINVAL, as there. */
static long linux_sys_fadvise64(uint64_t fd, uint64_t off, uint64_t len, uint64_t advice) {
    vibeos_file_t *f = linux_file_get(fd);
    long r = 0;

    (void)off;
    (void)len;
    if (!f) {
        return -VIBEOS_EBADF;
    }
    if (!f->ops->pread && !f->ops->getdents) {
        r = -VIBEOS_ESPIPE;
    } else if (VIBEOS_ARG_INT(advice) < 0 || VIBEOS_ARG_INT(advice) > LINUX_POSIX_FADV_NOREUSE) {
        r = -VIBEOS_EINVAL;
    }
    vibeos_file_put(f);
    return r;
}

/* readahead(fd, off, count): the same, for a file open for reading. */
static long linux_sys_readahead(uint64_t fd) {
    vibeos_file_t *f = linux_file_get(fd);
    long r = 0;

    if (!f) {
        return -VIBEOS_EBADF;
    }
    if ((f->flags & VIBEOS_O_ACCMODE) == VIBEOS_O_WRONLY) {
        r = -VIBEOS_EBADF;
    } else if (!f->ops->pread) {
        r = -VIBEOS_EINVAL;
    }
    vibeos_file_put(f);
    return r;
}

/* ---- kernel-side copies: sendfile and copy_file_range (L1) ----------------------------
 *
 * One loop through a kernel page. The page's address goes where a user address
 * usually goes: every file type copies through vibeos_uaccess_copy, which does
 * not care whose memory it is, so the types need no second set of operations.
 * At most a megabyte a call - a syscall runs with interrupts masked, and a
 * short count is an answer every caller of these already handles. */
#define LINUX_COPY_MAX (1024u * 1024u)

static long linux_copy_between(vibeos_file_t *in, uint64_t *in_off, vibeos_file_t *out,
                               uint64_t *out_off, uint64_t len) {
    uint8_t *page = (uint8_t *)ks_page_alloc();
    uint64_t ioff = in_off ? *in_off : in->pos;
    uint64_t done = 0;
    long r = 0;

    if (!page) {
        return -VIBEOS_ENOMEM;
    }
    if (len > LINUX_COPY_MAX) {
        len = LINUX_COPY_MAX;
    }
    while (done < len) {
        uint64_t want = len - done > 4096u ? 4096u : len - done;
        long got = in->ops->pread(in, (uint64_t)(uintptr_t)page, want, ioff);
        long put;

        if (got <= 0) {
            r = got;
            break;
        }
        put = out_off ? out->ops->pwrite(out, (uint64_t)(uintptr_t)page, (uint64_t)got,
                                         *out_off + done)
                      : out->ops->write(out, (uint64_t)(uintptr_t)page, (uint64_t)got);
        if (put <= 0) {
            r = put;
            break;
        }
        done += (uint64_t)put;
        ioff += (uint64_t)put;
        if (put < got) {
            break;
        }
    }
    ks_page_free(page, "sendfile bounce buffer");
    if (in_off) {
        *in_off = ioff;
    } else {
        in->pos = ioff;
    }
    if (out_off) {
        *out_off += done;
    }
    return done > 0u ? (long)done : r;
}

/* An optional offset argument: read in, and written back when the call ends. */
static long linux_off_in(uint64_t uptr, uint64_t *off) {
    if (vibeos_uaccess_copy(off, (const void *)(uintptr_t)uptr, sizeof(*off)) != 0) {
        return -VIBEOS_EFAULT;
    }
    return (int64_t)*off < 0 ? -VIBEOS_EINVAL : 0;
}

/* sendfile(out, in, offset, count): from a file that has positions to anything
 * that can be written. */
static long linux_sys_sendfile(uint64_t out_fd, uint64_t in_fd, uint64_t off_uptr, uint64_t count) {
    vibeos_file_t *in = linux_file_get(in_fd), *out = linux_file_get(out_fd);
    uint64_t off = 0;
    long r = 0;

    if (!in || !out) {
        r = -VIBEOS_EBADF;
    } else if ((in->flags & VIBEOS_O_ACCMODE) == VIBEOS_O_WRONLY ||
               (out->flags & VIBEOS_O_ACCMODE) == VIBEOS_O_RDONLY) {
        r = -VIBEOS_EBADF;
    } else if (!in->ops->pread || !out->ops->write) {
        r = -VIBEOS_EINVAL;
    } else if (off_uptr != 0u) {
        r = linux_off_in(off_uptr, &off);
    }
    if (r == 0) {
        r = linux_copy_between(in, off_uptr ? &off : 0, out, 0, count);
        if (r >= 0 && off_uptr != 0u &&
            vibeos_uaccess_copy((void *)(uintptr_t)off_uptr, &off, sizeof(off)) != 0) {
            r = -VIBEOS_EFAULT;
        }
    }
    if (in) {
        vibeos_file_put(in);
    }
    if (out) {
        vibeos_file_put(out);
    }
    return r;
}

/* copy_file_range(in, off_in, out, off_out, len, flags): between two regular
 * files. A range copied onto itself is EINVAL, as Linux refuses it. */
static long linux_sys_copy_file_range(uint64_t in_fd, uint64_t in_uptr, uint64_t out_fd,
                                      uint64_t out_uptr, uint64_t len, uint64_t flags) {
    vibeos_file_t *in, *out;
    uint64_t ioff = 0, ooff = 0;
    long r = 0;

    if (flags != 0u) {
        return -VIBEOS_EINVAL;
    }
    in = linux_file_get(in_fd);
    out = linux_file_get(out_fd);
    if (!in || !out) {
        r = -VIBEOS_EBADF;
    } else if (in->ops != &vibeos_fops_regular || out->ops != &vibeos_fops_regular) {
        r = (in->ops == &vibeos_fops_dir || out->ops == &vibeos_fops_dir) ? -VIBEOS_EISDIR
                                                                          : -VIBEOS_EINVAL;
    } else if ((in->flags & VIBEOS_O_ACCMODE) == VIBEOS_O_WRONLY ||
               (out->flags & VIBEOS_O_ACCMODE) == VIBEOS_O_RDONLY ||
               (out->flags & VIBEOS_O_APPEND)) {
        r = -VIBEOS_EBADF;
    }
    if (r == 0 && in_uptr != 0u) {
        r = linux_off_in(in_uptr, &ioff);
    }
    if (r == 0 && out_uptr != 0u) {
        r = linux_off_in(out_uptr, &ooff);
    }
    if (r == 0) {
        uint64_t a = in_uptr ? ioff : in->pos, b = out_uptr ? ooff : out->pos;
        if (in->mnt == out->mnt && in->node == out->node && a < b + len && b < a + len) {
            r = -VIBEOS_EINVAL;
        }
    }
    if (r == 0) {
        uint64_t opos = out->pos;
        r = linux_copy_between(in, in_uptr ? &ioff : 0, out, out_uptr ? &ooff : &opos, len);
        if (r >= 0 && !out_uptr) {
            out->pos = opos;
        }
        if (r >= 0 && in_uptr &&
            vibeos_uaccess_copy((void *)(uintptr_t)in_uptr, &ioff, sizeof(ioff)) != 0) {
            r = -VIBEOS_EFAULT;
        }
        if (r >= 0 && out_uptr &&
            vibeos_uaccess_copy((void *)(uintptr_t)out_uptr, &ooff, sizeof(ooff)) != 0) {
            r = -VIBEOS_EFAULT;
        }
    }
    if (in) {
        vibeos_file_put(in);
    }
    if (out) {
        vibeos_file_put(out);
    }
    return r;
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

    if (!ps || first > last || (flags & ~(uint64_t)(LINUX_CLOSE_RANGE_UNSHARE | LINUX_CLOSE_RANGE_CLOEXEC))) {
        return -VIBEOS_EINVAL;
    }
    if (flags & LINUX_CLOSE_RANGE_UNSHARE) {
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
        if (flags & LINUX_CLOSE_RANGE_CLOEXEC) {
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
        case LINUX_F_DUPFD:
        case LINUX_F_DUPFD_CLOEXEC:
            if (arg >= VIBEOS_FD_MAX) {
                return -VIBEOS_EINVAL;
            }
            return linux_dup_from(fd, (uint32_t)arg,
                                  VIBEOS_ARG_INT(cmd) == LINUX_F_DUPFD_CLOEXEC ? VIBEOS_FD_CLOEXEC : 0u);
        case LINUX_F_GETFD:
        case LINUX_F_SETFD:
            if (!ps) {
                return -VIBEOS_EBADF;
            }
            ks_lock(&ps->files_lock, __func__);
            if (!vibeos_fdtable_get(&ps->files, fd)) {
                r = -VIBEOS_EBADF;
            } else if (VIBEOS_ARG_INT(cmd) == LINUX_F_GETFD) {
                r = (long)vibeos_fdtable_flags(&ps->files, fd);
            } else {
                r = vibeos_fdtable_set_flags(&ps->files, fd,
                                             (uint32_t)arg & VIBEOS_FD_CLOEXEC) == 0 ? 0 : -VIBEOS_EBADF;
            }
            ks_unlock(&ps->files_lock);
            return r;
        case LINUX_F_GETFL:
            if (!(f = linux_file_get(fd))) {
                return -VIBEOS_EBADF;
            }
            r = (long)f->flags;
            vibeos_file_put(f);
            return r;
        case LINUX_F_SETFL:
            if (!(f = linux_file_get(fd))) {
                return -VIBEOS_EBADF;
            }
            /* The description's, so every descriptor naming it sees the change:
             * that is what makes O_NONBLOCK on a dup'd pipe end mean anything. */
            f->flags = (f->flags & ~LINUX_SETFL_MASK) | ((uint32_t)arg & LINUX_SETFL_MASK);
            vibeos_file_put(f);
            return 0;
        case LINUX_F_GETLK:
        case LINUX_F_SETLK:
        case LINUX_F_SETLKW:
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

/* Fill a struct stat the caller can believe. Assembled in the kernel and copied
 * out once: filling the user buffer field by field would fault in ring 0 if a
 * sibling munmaps it between the range check and any of these writes (uaccess
 * follow-up to 6a94a32, same class as H-026). */
static long linux_write_stat(uint64_t ubuf, const vibeos_file_stat_t *st) {
    linux_stat_t k;
    uint8_t *raw = (uint8_t *)&k;
    uint32_t i;

    for (i = 0; i < sizeof(k); i++) {
        raw[i] = 0;   /* padding included: it is copied out */
    }
    k.st_ino = st->ino;
    k.st_nlink = st->nlink ? st->nlink : 1u;
    k.st_mode = st->mode;
    k.st_uid = st->uid;
    k.st_gid = st->gid;
    k.st_size = (int64_t)st->size;
    k.st_blksize = 512;
    k.st_blocks = (int64_t)vibeos_ceil_div_u64(st->size, 512ull);
    k.st_atime = st->atime_ns / 1000000000ull;
    k.st_atime_nsec = st->atime_ns % 1000000000ull;
    k.st_mtime = st->mtime_ns / 1000000000ull;
    k.st_mtime_nsec = st->mtime_ns % 1000000000ull;
    k.st_ctime = st->ctime_ns / 1000000000ull;
    k.st_ctime_nsec = st->ctime_ns % 1000000000ull;
    if (vibeos_uaccess_copy((void *)(uintptr_t)ubuf, &k, sizeof(k)) != 0) {
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
    vibeos_file_stat_clear(&st);
    r = f->ops->stat ? (long)f->ops->stat(f, &st) : 0;
    vibeos_file_put(f);
    return r < 0 ? r : linux_write_stat(ubuf, &st);
}

/* newfstatat(dirfd, path, buf, flags): stat by name, or by fd when the path is
 * empty and AT_EMPTY_PATH is set. */
static long linux_sys_newfstatat(uint64_t dirfd, uint64_t path_uptr, uint64_t ubuf,
                                 uint64_t flags) {
    vibeos_path_t w;
    vibeos_file_stat_t st;
    char first = 0;
    long r;

    if (flags & ~(uint64_t)(LINUX_AT_EMPTY_PATH | LINUX_AT_SYMLINK_NOFOLLOW)) {
        return -VIBEOS_EINVAL;
    }
    if (vibeos_uaccess_copy(&first, (const void *)(uintptr_t)path_uptr, 1u) != 0) {
        return -VIBEOS_EFAULT;
    }
    if (first == 0) {
        if ((flags & LINUX_AT_EMPTY_PATH) == 0) {
            return -VIBEOS_ENOENT;
        }
        return linux_sys_fstat(dirfd, ubuf);
    }
    /* Directory or file? The answer changes what a program does, not just what
     * it prints: ls given a directory lists it and given a file names it, so
     * reporting the wrong one produces a plausible wrong result rather than an
     * error. The filesystem decides; how it decides is its business. */
    r = linux_walk_at(dirfd, path_uptr,
                      (flags & LINUX_AT_SYMLINK_NOFOLLOW) ? VIBEOS_PATH_NOFOLLOW : 0u, &w);
    if (r != 0) {
        return r;
    }
    vibeos_file_stat_from_node(&st, &w.node);
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
    vibeos_path_t w;
    long r;

    if (flags & ~(uint64_t)LINUX_AT_REMOVEDIR) {
        return -VIBEOS_EINVAL;
    }
    if (flags & LINUX_AT_REMOVEDIR) {
        return -VIBEOS_EINVAL;
    }
    /* The name itself goes, so a symbolic link is removed and not followed. */
    r = linux_walk_at(dirfd, path_uptr, VIBEOS_PATH_NOFOLLOW, &w);
    if (r != 0) {
        return r;
    }
    if (w.node.is_dir) {
        return -VIBEOS_EISDIR;
    }
    return (vibeos_fs_unlink(w.mnt, w.tail) == 0) ? 0 : -VIBEOS_EIO;
}

static long linux_sys_unlink(uint64_t path_uptr) {
    return linux_sys_unlinkat((uint64_t)(uint32_t)LINUX_AT_FDCWD, path_uptr, 0);
}

/* mkdirat(dirfd, path, mode) and mkdir(path, mode). The mode is not kept: there
 * is one user and no permission bits on these filesystems. */
static long linux_sys_mkdirat(uint64_t dirfd, uint64_t path_uptr) {
    vibeos_path_t w;
    long r;

    /* Not followed: a dangling symbolic link at the name is EEXIST, as on
     * Linux, rather than a directory made where it points. */
    r = linux_walk_at(dirfd, path_uptr, VIBEOS_PATH_CREATE | VIBEOS_PATH_NOFOLLOW, &w);
    if (r != 0) {
        return r;
    }
    if (w.exists) {
        return -VIBEOS_EEXIST;
    }
    return (vibeos_fs_mkdir(w.mnt, w.tail) == 0) ? 0 : -VIBEOS_EIO;
}

static long linux_sys_mkdir(uint64_t path_uptr) {
    return linux_sys_mkdirat((uint64_t)(uint32_t)LINUX_AT_FDCWD, path_uptr);
}

/* chdir(path) and fchdir(fd): the working directory, which relative paths start
 * from. It must be a directory that exists now; it is kept as a path, so a
 * directory removed later leaves a working directory that names nothing, and the
 * next relative lookup says ENOENT - as on Linux. */
static long linux_set_cwd(const vibeos_path_t *w) {
    vibeos_procstate_t *ps = linux_cur_ps();
    const char *abs = w->path;
    uint32_t i;

    if (!ps) {
        return -VIBEOS_EINVAL;
    }
    if (!w->node.is_dir) {
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
    vibeos_path_t w;
    long r = linux_walk_at((uint64_t)(uint32_t)LINUX_AT_FDCWD, path_uptr, 0u, &w);

    return r != 0 ? r : linux_set_cwd(&w);
}

static long linux_sys_fchdir(uint64_t fd) {
    vibeos_file_t *f = linux_file_get(fd);
    vibeos_path_t w;
    long r;

    if (!f) {
        return -VIBEOS_EBADF;
    }
    if (f->ops != &vibeos_fops_dir) {
        vibeos_file_put(f);
        return -VIBEOS_ENOTDIR;
    }
    /* Walked again from its path: the directory may have gone since it was
     * opened, and a working directory is only set to one that exists. */
    r = vibeos_path_walk("/", "/", f->path, 0u, &w);
    vibeos_file_put(f);
    return r != 0 ? r : linux_set_cwd(&w);
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

/* readlinkat(): a symbolic link's contents, from a filesystem that has them
 * (L1), and the one link a program uses to find itself, answered from what
 * execve was actually given rather than from a made-up path - there is no /proc
 * to hold it. Anything else is not a link, which is what EINVAL means. */
static long linux_sys_readlinkat(uint64_t dirfd, uint64_t path_uptr, uint64_t ubuf,
                                 uint64_t bufsz) {
    char raw[VIBEOS_PATH_MAX];
    vibeos_path_t w;
    const char *self;
    uint64_t n = 0;
    long r;

    /* /proc does not exist, so its one link is recognised from the path as
     * written, before any walk would say ENOENT for it. */
    if (!linux_is_proc_self_exe(dirfd, path_uptr)) {
        long t;
        /* Walked without following the last component: the link is the
         * question. */
        r = linux_walk_at(dirfd, path_uptr, VIBEOS_PATH_NOFOLLOW, &w);
        if (r != 0) {
            return r;
        }
        if ((w.node.mode & VIBEOS_S_IFMT) != VIBEOS_S_IFLNK) {
            return -VIBEOS_EINVAL;
        }
        t = vibeos_fs_readlink(w.mnt, *w.tail ? w.tail : "/", raw, sizeof(raw));
        if (t < 0) {
            return t;
        }
        n = (uint64_t)t < bufsz ? (uint64_t)t : bufsz;
        if (!linux_user_ok(ubuf, n, 1)) {
            return -VIBEOS_EFAULT;
        }
        if (vibeos_uaccess_copy((void *)(uintptr_t)ubuf, raw, n) != 0) {
            return -VIBEOS_EFAULT;
        }
        return (long)n;   /* not terminated, as Linux does not terminate it */
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

#define LINUX_FS_SYSCALLS(X) \
    X(0,   read,        READ,        PTRS(OUT_BUF(1, 2)), linux_sys_read(ARG(0), ARG(1), ARG(2))) \
    X(1,   write,       WRITE,       PTRS(IN_BUF(1, 2)), linux_sys_write(ARG(0), ARG(1), ARG(2))) \
    X(2,   open,        OPEN,        NOPTR, linux_sys_open(ARG(0), ARG(1), ARG(2))) \
    X(3,   close,       CLOSE,       NOPTR, linux_sys_close(ARG(0))) \
    X(5,   fstat,       FSTAT,       PTRS(OUT(1, sizeof(linux_stat_t))), linux_sys_fstat(ARG(0), ARG(1))) \
    X(8,   lseek,       LSEEK,       NOPTR, linux_sys_lseek(ARG(0), ARG(1), ARG(2))) \
    X(16,  ioctl,       IOCTL,       PTRS(OUT_IF(1, VIBEOS_IOCTL_GET_PGRP, 2, sizeof(uint32_t)), IN_IF(1, VIBEOS_IOCTL_SET_PGRP, 2, sizeof(uint32_t))), linux_sys_ioctl(ARG(0), ARG(1), ARG(2))) \
    X(19,  readv,       READV,       PTRS(IN_VEC(1, 2, sizeof(linux_iovec_t), 1024)), linux_sys_readv(ARG(0), ARG(1), ARG(2))) \
    X(20,  writev,      WRITEV,      PTRS(IN_VEC(1, 2, sizeof(linux_iovec_t), 1024)), linux_sys_writev(ARG(0), ARG(1), ARG(2))) \
    X(22,  pipe,        PIPE,        PTRS(OUT(0, 8)), linux_sys_pipe2(ARG(0), 0)) \
    X(32,  dup,         DUP,         NOPTR, linux_sys_dup(ARG(0))) \
    X(33,  dup2,        DUP2,        NOPTR, linux_sys_dup2(ARG(0), ARG(1))) \
    X(17,  pread64,     PREAD,       PTRS(OUT_BUF(1, 2)), linux_sys_pread64(ARG(0), ARG(1), ARG(2), ARG(3))) \
    X(18,  pwrite64,    PWRITE,      PTRS(IN_BUF(1, 2)), linux_sys_pwrite64(ARG(0), ARG(1), ARG(2), ARG(3))) \
    X(40,  sendfile,    SENDFILE,    PTRS(OUT_OPT(2, 8)), linux_sys_sendfile(ARG(0), ARG(1), ARG(2), ARG(3))) \
    X(74,  fsync,       FSYNC,       NOPTR, linux_sys_fsync(ARG(0))) \
    X(75,  fdatasync,   FDATASYNC,   NOPTR, linux_sys_fsync(ARG(0))) \
    X(76,  truncate,    TRUNCATE,    NOPTR, linux_sys_truncate(ARG(0), ARG(1))) \
    X(77,  ftruncate,   FTRUNCATE,   NOPTR, linux_sys_ftruncate(ARG(0), ARG(1))) \
    X(85,  creat,       CREAT,       NOPTR, linux_sys_creat(ARG(0), ARG(1))) \
    X(95,  umask,       UMASK,       NOPTR, linux_sys_umask(ARG(0))) \
    X(162, sync,        SYNC,        NOPTR, linux_sys_sync()) \
    X(187, readahead,   READAHEAD,   NOPTR, linux_sys_readahead(ARG(0))) \
    X(221, fadvise64,   FADVISE,     NOPTR, linux_sys_fadvise64(ARG(0), ARG(1), ARG(2), ARG(3))) \
    X(72,  fcntl,       FCNTL,       NOPTR, linux_sys_fcntl(ARG(0), ARG(1), ARG(2))) \
    X(79,  getcwd,      GETCWD,      NOPTR, linux_sys_getcwd(ARG(0), ARG(1))) \
    X(80,  chdir,       CHDIR,       NOPTR, linux_sys_chdir(ARG(0))) \
    X(81,  fchdir,      FCHDIR,      NOPTR, linux_sys_fchdir(ARG(0))) \
    X(83,  mkdir,       MKDIR,       NOPTR, linux_sys_mkdir(ARG(0))) \
    X(87,  unlink,      UNLINK,      NOPTR, linux_sys_unlink(ARG(0))) \
    X(217, getdents64,  GETDENTS,    PTRS(OUT_BUF(1, 2)), linux_sys_getdents64(ARG(0), ARG(1), ARG(2))) \
    X(257, openat,      OPEN_AT,     NOPTR, linux_sys_openat(ARG(0), ARG(1), ARG(2), ARG(3))) \
    X(258, mkdirat,     MKDIR_AT,    NOPTR, linux_sys_mkdirat(ARG(0), ARG(1))) \
    X(262, newfstatat,  STAT_AT,     PTRS(OUT(2, sizeof(linux_stat_t))), linux_sys_newfstatat(ARG(0), ARG(1), ARG(2), ARG(3))) \
    X(263, unlinkat,    UNLINK_AT,   NOPTR, linux_sys_unlinkat(ARG(0), ARG(1), ARG(2))) \
    X(267, readlinkat,  READLINK_AT, NOPTR, linux_sys_readlinkat(ARG(0), ARG(1), ARG(2), ARG(3))) \
    X(277, sync_file_range, SYNC_RANGE, NOPTR, linux_sys_fsync(ARG(0))) \
    X(285, fallocate,   FALLOCATE,   NOPTR, linux_sys_fallocate(ARG(0), ARG(1), ARG(2), ARG(3))) \
    X(292, dup3,        DUP3,        NOPTR, linux_sys_dup3(ARG(0), ARG(1), ARG(2))) \
    X(293, pipe2,       PIPE2,       PTRS(OUT(0, 8)), linux_sys_pipe2(ARG(0), ARG(1))) \
    X(295, preadv,      PREADV,      PTRS(IN_VEC(1, 2, sizeof(linux_iovec_t), 1024)), linux_rw_vec_at(ARG(0), ARG(1), ARG(2), ARG(3), 0)) \
    X(296, pwritev,     PWRITEV,     PTRS(IN_VEC(1, 2, sizeof(linux_iovec_t), 1024)), linux_rw_vec_at(ARG(0), ARG(1), ARG(2), ARG(3), 1)) \
    X(306, syncfs,      SYNCFS,      NOPTR, linux_sys_syncfs(ARG(0))) \
    X(326, copy_file_range, COPY_RANGE, PTRS(OUT_OPT(1, 8), OUT_OPT(3, 8)), linux_sys_copy_file_range(ARG(0), ARG(1), ARG(2), ARG(3), ARG(4), ARG(5))) \
    X(327, preadv2,     PREADV2,     PTRS(IN_VEC(1, 2, sizeof(linux_iovec_t), 1024)), linux_sys_preadv2(ARG(0), ARG(1), ARG(2), ARG(3), ARG(5))) \
    X(328, pwritev2,    PWRITEV2,    PTRS(IN_VEC(1, 2, sizeof(linux_iovec_t), 1024)), linux_sys_pwritev2(ARG(0), ARG(1), ARG(2), ARG(3), ARG(5))) \
    X(436, close_range, CLOSE_RANGE, NOPTR, linux_sys_close_range(ARG(0), ARG(1), ARG(2)))

LINUX_DEFINE_SYSCALLS(fs, LINUX_FS_SYSCALLS)
