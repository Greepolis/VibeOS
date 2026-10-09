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

#include <stddef.h>

#include "linux_internal.h"
#include "vibeos/devfs.h"

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

/* ---- locks (L1 step 6) ---------------------------------------------------------------
 *
 * What a lock is on: the filesystem's identity for a file, or - for a pipe, a
 * socket or the console, which Linux locks too - the description's own. */
static void linux_lock_key(const vibeos_file_t *f, const void **fs, uint64_t *node) {
    if (f->mnt) {
        *fs = f->mnt;
        *node = f->node;
    } else {
        *fs = f->ops;
        *node = f->pipe >= 0 ? (uint64_t)f->pipe + 1u : (uint64_t)(uint32_t)f->sock + 0x100000u;
    }
}

static uint64_t linux_lock_owner_proc(void) {
    int me = ks_current();
    return me < 0 ? 0u : VIBEOS_FLK_OWNER_PROC(ks_id(me)->tgid);
}

/* POSIX's rule, which nobody would have chosen and every program has to live
 * with: closing *any* descriptor a process has for a file gives back every
 * record lock the process holds on that file - whichever descriptor took them.
 * Called for a description that is leaving the caller's table. */
static void linux_locks_on_close(const vibeos_file_t *f) {
    const void *fs;
    uint64_t node, owner = linux_lock_owner_proc();

    if (owner == 0u || vibeos_flk_count() == 0u) {
        return;
    }
    linux_lock_key(f, &fs, &node);
    vibeos_flk_drop_file(owner, fs, node);
}

/* A process has ended: its record locks end with it. Not called for an exec,
 * which keeps them. */
void linux_locks_exit(uint32_t tgid) {
    if (vibeos_flk_count() != 0u) {
        vibeos_flk_drop_owner(VIBEOS_FLK_OWNER_PROC(tgid));
    }
}

/* Take a number out of the table and release its description - outside the
 * table's lock, because a release wakes a pipe's reader. */
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
    linux_locks_on_close(f);
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
    if (n == 0u) {
        /* Nothing to look up, whatever it would have been looked up from:
         * Linux says ENOENT before it examines the directory descriptor, and
         * this said ENOTDIR or EBADF when that was a file or nothing (LTP's
         * fchmodat02). The calls that take an empty path on purpose ask for
         * it with AT_EMPTY_PATH and do not come this way. */
        return -VIBEOS_ENOENT;
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

    if (r == 0) {
        /* As the caller: a directory it may not search stops the walk. */
        vibeos_cred_t c;
        linux_cred(&c);
        r = vibeos_path_walk_as(root, base, raw, flags, &c, w);
    }
    return r;
}

/* The file a descriptor names, walked again from the path its description
 * remembers - what fchmod, fchown, futimens, fstatfs and every AT_EMPTY_PATH
 * work on. The last component is not followed: the description was opened on
 * what the link pointed at, and its path is that. AT_FDCWD is the working
 * directory. A pipe, a socket or the console is on no filesystem: EINVAL.
 *
 * The gap is the one a path for an identity always has: after a rename the
 * description's path names nothing, or something else. Nothing is ENOENT, as
 * it was. Something else was refused (M-080): open /a, rename it away, make a
 * new /a, and fchmod on the old descriptor changed the *new* file - the wrong
 * object, through a descriptor that still looked valid, with no race needed.
 *
 * A filesystem that can be asked by node is asked by node now: the answer is
 * the description's own file wherever it has been renamed to, as Linux's is,
 * and `by_node` sends a change there rather than to the name. -ENOENT from
 * that question is a file that is gone, which Linux would still act on and
 * this cannot: there is nothing left to ask. A filesystem without the
 * question has its path walked as before, and a different node there is
 * ESTALE - a refusal, which is better than the wrong file. Both are only as
 * good as the filesystem's node numbers: tmpfs's carry a generation; FAT's
 * are a directory slot, so a file renamed elsewhere is not found and a new
 * file in the old one's slot is taken for it. */
long linux_walk_fd(uint64_t fd, vibeos_path_t *w) {
    vibeos_file_t *f;
    long r;

    if (VIBEOS_ARG_INT(fd) == LINUX_AT_FDCWD) {
        vibeos_procstate_t *ps = linux_cur_ps();
        char cwd[VIBEOS_PATH_MAX];
        if (!ps) {
            return -VIBEOS_EINVAL;
        }
        linux_ps_path(ps, 0, cwd);
        return vibeos_path_walk("/", "/", cwd, 0u, w);
    }
    if (!(f = linux_file_get(fd))) {
        return -VIBEOS_EBADF;
    }
    if (!f->mnt) {
        vibeos_file_put(f);
        return -VIBEOS_EINVAL;
    }
    {
        vibeos_fs_node_t held;
        uint32_t i;

        held.id = f->node;
        held.size = f->size;
        held.is_dir = f->isdir;
        r = vibeos_fs_getattr(f->mnt, &held, &w->node);
        if (r != -VIBEOS_ENOSYS) {
            if (r == 0) {
                for (i = 0; f->path[i] && i + 1u < sizeof(w->path); i++) {
                    w->path[i] = f->path[i];
                }
                w->path[i] = 0;
                w->mnt = f->mnt;
                w->tail = w->path + (f->tail < i ? f->tail : i);
                w->exists = 1;
                w->trailing_slash = 0;
                w->by_node = 1;
            } else if (r != -VIBEOS_EIO) {
                r = -VIBEOS_ENOENT;
            }
            vibeos_file_put(f);
            return r;
        }
    }
    r = vibeos_path_walk("/", "/", f->path, VIBEOS_PATH_NOFOLLOW, w);
    if (r == 0 && w->exists && (w->mnt != f->mnt || w->node.id != f->node)) {
        r = -VIBEOS_ESTALE;
    }
    vibeos_file_put(f);
    return r;
}

/* A path argument that may be empty with AT_EMPTY_PATH, which makes `dirfd`
 * itself the file. Without the flag an empty path is ENOENT, as the walk says. */
long linux_walk_at_empty(uint64_t dirfd, uint64_t upath, uint64_t atflags, uint32_t flags,
                         vibeos_path_t *w) {
    char first = 1;

    if (atflags & LINUX_AT_EMPTY_PATH) {
        /* Asked first whose memory it is; see linux_stat_get (M-082). */
        if (!linux_user_ok(upath, 1u, 0) ||
            vibeos_uaccess_copy(&first, (const void *)(uintptr_t)upath, 1u) != 0) {
            return -VIBEOS_EFAULT;
        }
        if (first == 0) {
            return linux_walk_fd(dirfd, w);
        }
    }
    return linux_walk_at(dirfd, upath, flags, w);
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
/* A writev of several pieces that fit in a page, as one write.
 *
 * A C library that buffers a stream by lines - which it does the moment the
 * stream is a terminal (L1 step 7) - writes what was in its buffer and the
 * piece that completed the line as two elements of one writev. Written one
 * element at a time those are two writes: on the console two lines of log with
 * a prefix each, and on a pipe two chances for another writer's bytes to land
 * in between. Linux writes a small writev whole; so the pieces are gathered
 * into a kernel page and written once. The page's address goes where a user
 * address usually goes, as in sendfile.
 *
 * 0 when this did not apply and the caller should write element by element;
 * otherwise the call's result. */
static long linux_writev_gathered(vibeos_file_t *f, uint64_t iov_uptr, uint64_t iovcnt, int *did) {
    linux_iovec_t v;
    uint8_t *page;
    uint64_t i, total = 0;
    long r;

    *did = 0;
    if (iovcnt < 2u || !f->ops->write) {
        return 0;
    }
    for (i = 0; i < iovcnt; i++) {
        if (vibeos_uaccess_copy(&v, (const void *)(uintptr_t)
                (iov_uptr + i * sizeof(linux_iovec_t)), sizeof(v)) != 0) {
            return 0;   /* the element-by-element path says EFAULT where it should */
        }
        if (v.iov_len > 4096u || total + v.iov_len > 4096u) {
            return 0;
        }
        total += v.iov_len;
    }
    if (total == 0u || !(page = (uint8_t *)ks_page_alloc())) {
        return 0;
    }
    total = 0;
    for (i = 0; i < iovcnt; i++) {
        /* Read again: the lengths may have changed under a sibling thread, so
         * the bound is checked against the page once more. */
        if (vibeos_uaccess_copy(&v, (const void *)(uintptr_t)
                (iov_uptr + i * sizeof(linux_iovec_t)), sizeof(v)) != 0 ||
            total + v.iov_len > 4096u || v.iov_len > 4096u ||
            (v.iov_len != 0u && (!linux_user_ok(v.iov_base, v.iov_len, 0) ||
             vibeos_uaccess_copy(page + total, (const void *)(uintptr_t)v.iov_base, v.iov_len) != 0))) {
            ks_page_free(page, "writev gather buffer");
            return 0;
        }
        total += v.iov_len;
    }
    r = f->ops->write(f, (uint64_t)(uintptr_t)page, total);
    ks_page_free(page, "writev gather buffer");
    *did = 1;
    return r;
}

/* readv and writev on a socket (docs/abi/ L5): the vector as one message - one
 * receive, one send - as Linux makes them. Element by element, a read would wait
 * in the second element for data the first was not given. *did is 0 for a
 * descriptor that is not a socket, or a vector longer than a page of iovecs,
 * which the element-by-element path then takes. */
static long linux_socket_vec_of(uint64_t fd, uint64_t iov_uptr, uint64_t iovcnt, int write, int *did) {
    vibeos_file_t *f = linux_file_get(fd);
    vibeos_uiov_t *iov;
    uint64_t i;
    long r;

    *did = 0;
    if (!f) {
        return 0;
    }
    if (!f->ops->sockops || iovcnt * sizeof(vibeos_uiov_t) > 4096u) {
        vibeos_file_put(f);
        return 0;
    }
    *did = 1;
    if (!(iov = (vibeos_uiov_t *)ks_page_alloc())) {
        vibeos_file_put(f);
        return -VIBEOS_ENOMEM;
    }
    r = 0;
    for (i = 0; i < iovcnt && r == 0; i++) {
        linux_iovec_t v;

        if (vibeos_uaccess_copy(&v, (const void *)(uintptr_t)(iov_uptr + i * sizeof(v)), sizeof(v)) != 0 ||
            (v.iov_len != 0u && !linux_user_ok(v.iov_base, v.iov_len, !write))) {
            r = -VIBEOS_EFAULT;
        }
        iov[i].base = v.iov_base;
        iov[i].len = v.iov_len;
    }
    if (r == 0) {
        r = linux_socket_vec(f, iov, (uint32_t)iovcnt, write);
    }
    ks_page_free(iov, "socket iovecs");
    vibeos_file_put(f);
    return r;
}

/* The vector as Linux takes it before any byte moves (LTP's readv02 and
 * writev01): the descriptor first, then every element's length - one that is
 * negative as a size, or a sum past the largest, is EINVAL, whatever the
 * bases are; a base is only judged when its element is reached. */
static long linux_iov_lengths(uint64_t fd, uint64_t iov_uptr, uint64_t iovcnt) {
    vibeos_file_t *f = linux_file_get(fd);
    uint64_t i, sum = 0;

    if (!f) {
        return -VIBEOS_EBADF;
    }
    vibeos_file_put(f);
    for (i = 0; i < iovcnt; i++) {
        linux_iovec_t v;

        if (vibeos_uaccess_copy(&v, (const void *)(uintptr_t)(iov_uptr + i * sizeof(v)), sizeof(v)) != 0) {
            return -VIBEOS_EFAULT;
        }
        if ((int64_t)v.iov_len < 0 || (sum += v.iov_len) > (uint64_t)INT64_MAX) {
            return -VIBEOS_EINVAL;
        }
    }
    return 0;
}

static long linux_sys_writev(uint64_t fd, uint64_t iov_uptr, uint64_t iovcnt) {
    long total = 0;
    uint64_t i;

    if (iovcnt > 1024u) {
        return -VIBEOS_EINVAL;   /* Linux caps this at UIO_MAXIOV */
    }
    {
        long r = linux_iov_lengths(fd, iov_uptr, iovcnt);

        if (r != 0) {
            return r;
        }
    }
    {
        int did;
        long r = linux_socket_vec_of(fd, iov_uptr, iovcnt, 1, &did);

        if (did) {
            return r;
        }
    }
    {
        vibeos_file_t *f = linux_file_get(fd);
        int did = 0;
        long r;
        if (!f) {
            return -VIBEOS_EBADF;
        }
        r = linux_writev_gathered(f, iov_uptr, iovcnt, &did);
        vibeos_file_put(f);
        if (did) {
            return r;
        }
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
    {
        long r = linux_iov_lengths(fd, iov_uptr, iovcnt);

        if (r != 0) {
            return r;
        }
    }
    {
        int did;
        long r = linux_socket_vec_of(fd, iov_uptr, iovcnt, 0, &did);

        if (did) {
            return r;
        }
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
uint32_t linux_umask(void) {
    vibeos_procstate_t *ps = linux_cur_ps();
    return ps ? ps->umask : 022u;
}

/* ---- who is asking (docs/abi/ L2 step 1) ------------------------------------------- */

void linux_cred(vibeos_cred_t *out) {
    vibeos_procstate_t *ps = linux_cur_ps();

    if (!ps) {
        vibeos_cred_root(out);
        return;
    }
    ks_lock(&ps->files_lock, __func__);
    *out = ps->cred;
    ks_unlock(&ps->files_lock);
}

long linux_may(const vibeos_fs_node_t *node, uint32_t want) {
    vibeos_cred_t c;

    linux_cred(&c);
    return vibeos_cred_may(&c, 0, node->mode, node->uid, node->gid, want);
}

long linux_may_own(const vibeos_fs_node_t *node) {
    vibeos_cred_t c;

    linux_cred(&c);
    return (c.fsuid == 0u || c.fsuid == node->uid) ? 0 : -VIBEOS_EPERM;
}

/* The directory `w`'s last component is, or would be, an entry of. 0 and the
 * node; a walk's own error otherwise. Walked by the kernel for itself: the
 * caller already got this far. */
static long linux_parent_of(const vibeos_path_t *w, vibeos_fs_node_t *dir) {
    char parent[VIBEOS_PATH_MAX];
    vibeos_path_t pw;
    uint32_t n = 0, cut = 0, i;
    long r;

    for (i = 0; w->path[i]; i++) {
        if (w->path[i] == '/') {
            cut = i;
        }
        n++;
    }
    for (i = 0; i < cut; i++) {
        parent[i] = w->path[i];
    }
    if (cut == 0u) {
        parent[cut++] = '/';
    }
    parent[cut] = 0;
    (void)n;
    r = vibeos_path_walk("/", "/", parent, 0u, &pw);
    if (r == 0) {
        *dir = pw.node;
    }
    return r;
}

long linux_may_add(const vibeos_path_t *w) {
    vibeos_fs_node_t dir;
    vibeos_cred_t c;

    linux_cred(&c);
    if (c.fsuid == 0u || linux_parent_of(w, &dir) != 0) {
        return 0;
    }
    return vibeos_cred_may(&c, 0, dir.mode, dir.uid, dir.gid, VIBEOS_MAY_WRITE | VIBEOS_MAY_EXEC);
}

long linux_may_remove(const vibeos_path_t *w) {
    vibeos_fs_node_t dir;
    vibeos_cred_t c;

    linux_cred(&c);
    if (c.fsuid == 0u || linux_parent_of(w, &dir) != 0) {
        return 0;
    }
    if (vibeos_cred_may(&c, 0, dir.mode, dir.uid, dir.gid, VIBEOS_MAY_WRITE | VIBEOS_MAY_EXEC) != 0) {
        return -VIBEOS_EACCES;
    }
    /* A sticky directory - /tmp - is one everybody may write and nobody may
     * clear of other people's files: a name there goes only for the file's
     * owner or the directory's. */
    if ((dir.mode & VIBEOS_S_ISVTX) && c.fsuid != dir.uid && c.fsuid != w->node.uid) {
        return -VIBEOS_EPERM;
    }
    return 0;
}

void linux_own_new(const vibeos_path_t *w) {
    vibeos_fs_attr_t attr;
    vibeos_cred_t c;

    linux_cred(&c);
    if (c.fsuid == 0u && c.fsgid == 0u) {
        return;   /* a filesystem makes its files root's */
    }
    attr.valid = VIBEOS_ATTR_UID | VIBEOS_ATTR_GID;
    attr.uid = c.fsuid;
    attr.gid = c.fsgid;
    (void)vibeos_fs_setattr(w->mnt, *w->tail ? w->tail : "/", &attr);
}

long linux_sys_openat(uint64_t dirfd, uint64_t path_uptr, uint64_t flags, uint64_t mode) {
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
    /* Permission, by what the open would do: read, write (truncating is
     * writing), or make a name in the directory. Asked before the file is
     * touched - an open that is refused must not have truncated anything. */
    if (w.exists) {
        const uint32_t acc = (uint32_t)flags & VIBEOS_O_ACCMODE;
        uint32_t want = acc == VIBEOS_O_WRONLY ? VIBEOS_MAY_WRITE
                      : acc == VIBEOS_O_RDWR ? (VIBEOS_MAY_READ | VIBEOS_MAY_WRITE) : VIBEOS_MAY_READ;
        if (flags & VIBEOS_O_TRUNC) {
            want |= VIBEOS_MAY_WRITE;
        }
        err = linux_may(&w.node, want);
    } else {
        err = linux_may_add(&w);
    }
    if (err != 0) {
        return err;
    }
    f = vibeos_open_path(&w, (uint32_t)flags, (uint32_t)mode & ~linux_umask(), &err);
    if (!f) {
        return err;
    }
    if (!w.exists) {
        linux_own_new(&w);
    }
    return linux_fd_install(f, (flags & VIBEOS_O_CLOEXEC) ? VIBEOS_FD_CLOEXEC : 0u, 0);
}

/* memfd_create(name, flags): a file that is nobody's but the descriptor's
 * (docs/abi/ L3 step 4) - memory with a file's interface, to size with
 * ftruncate, map shared and hand to another process.
 *
 * It is a file on /tmp, which is the filesystem here that keeps its files in
 * memory and can share their pages. Linux's has no name anywhere; this one has
 * a name under /tmp for as long as it is open, because a file here is found by
 * its path, and the name goes when the last descriptor does. That it can be
 * seen there meanwhile is the difference, and it is written down.
 *
 * The name a program gives is a label for a debugger and is only checked for
 * length. MFD_ALLOW_SEALING is accepted and means nothing yet: there are no
 * seals to add (fcntl refuses F_ADD_SEALS), which the registry names. */
static uint32_t g_memfd_seq;

static long linux_sys_memfd_create(uint64_t name_uptr, uint64_t flags) {
    char label[LINUX_MFD_NAME_MAX + 2u];
    char path[40] = "/tmp/.memfd-";
    vibeos_path_t w;
    vibeos_file_t *f;
    uint32_t seq, n, k, tries;
    long err = -VIBEOS_EEXIST;

    if (ks_current() < 0 || !ks_id(ks_current())->is_user) {
        return -VIBEOS_EINVAL;
    }
    if (flags & ~(uint64_t)(LINUX_MFD_CLOEXEC | LINUX_MFD_ALLOW_SEALING)) {
        return -VIBEOS_EINVAL;
    }
    if (ks_copy_user_string(name_uptr, label, (int)sizeof(label)) != 0) {
        return -VIBEOS_EFAULT;
    }
    for (n = 0; label[n]; n++) {
    }
    if (n > LINUX_MFD_NAME_MAX) {
        return -VIBEOS_EINVAL;
    }
    for (tries = 0; tries < 8u && err == -VIBEOS_EEXIST; tries++) {
        char digits[10];

        seq = __sync_add_and_fetch(&g_memfd_seq, 1u);
        for (n = 12, k = 0; k == 0u || seq != 0u; seq /= 10u) {
            digits[k++] = (char)('0' + seq % 10u);
        }
        while (k > 0u) {
            path[n++] = digits[--k];
        }
        path[n] = 0;
        err = vibeos_path_walk("/", "/", path,
                               vibeos_open_walk_flags(VIBEOS_O_CREAT | VIBEOS_O_EXCL | VIBEOS_O_RDWR), &w);
        if (err != 0) {
            return err;
        }
        f = vibeos_open_path(&w, VIBEOS_O_CREAT | VIBEOS_O_EXCL | VIBEOS_O_RDWR, 0600u, &err);
        if (f) {
            f->unlink_on_release = 1;
            return linux_fd_install(f, (flags & LINUX_MFD_CLOEXEC) ? VIBEOS_FD_CLOEXEC : 0u, 0);
        }
    }
    return err;
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
    /* No position to read at: a pipe or a terminal is ESPIPE, a directory
     * EISDIR - it has an offset, and is not read this way (LTP's preadv02). */
    r = f->ops->pread ? f->ops->pread(f, buf, len, off)
                      : f->ops == &vibeos_fops_dir ? -VIBEOS_EISDIR : -VIBEOS_ESPIPE;
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
    r = f->ops->pwrite ? f->ops->pwrite(f, buf, len, off)
                       : f->ops == &vibeos_fops_dir ? -VIBEOS_EISDIR : -VIBEOS_ESPIPE;
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
        if ((int64_t)v.iov_len < 0) {
            /* Not a length. EINVAL, and before the range is judged: a range
             * of that size is nobody's, and EFAULT would be the answer to a
             * different question (LTP's preadv02 and pwritev02). */
            return total > 0 ? total : -VIBEOS_EINVAL;
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
    if ((r = linux_may(&w.node, VIBEOS_MAY_WRITE)) != 0) {
        return r;
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
    if ((int64_t)(off + len) < 0) {
        /* Past the largest offset a file can have. With FALLOC_FL_KEEP_SIZE
         * nothing below would have looked, and the call succeeded (LTP's
         * fallocate02). */
        return -VIBEOS_EFBIG;
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
    if (!f->ops->pread && !f->ops->readdir) {
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
    if ((flags & LINUX_CLOSE_RANGE_UNSHARE) &&
        __atomic_load_n(&ps->files_users, __ATOMIC_ACQUIRE) > 1u) {
        /* "Give this thread a table of its own first." A process with one
         * thread already has one, and the flag asks for nothing. With more,
         * it is unshare(CLONE_FILES), which needs a thread to be able to hold
         * a table apart from its process (L6); refusing is better than closing
         * descriptors in a table the other threads are still using. */
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
        linux_locks_on_close(old);
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

/* The range a struct flock names: from l_start, counted from the start of the
 * file, the description's position or the end, for l_len bytes - to the end of
 * the file wherever that goes when l_len is 0, and backwards from the start
 * when it is negative. */
static long linux_flock_range(vibeos_file_t *f, const linux_flock_t *fl, uint64_t *start,
                              uint64_t *end) {
    int64_t base = 0, s, e;

    switch (fl->l_whence) {
        case VIBEOS_SEEK_SET:
            break;
        case VIBEOS_SEEK_CUR:
            base = (int64_t)f->pos;
            break;
        case VIBEOS_SEEK_END: {
            vibeos_file_stat_t st;
            vibeos_file_stat_clear(&st);
            if (f->ops->stat) {
                (void)f->ops->stat(f, &st);
            }
            base = (int64_t)st.size;
            break;
        }
        default:
            return -VIBEOS_EINVAL;
    }
    if (fl->l_start > 0 && base > INT64_MAX - fl->l_start) {
        return -VIBEOS_EOVERFLOW;
    }
    s = base + fl->l_start;
    if (fl->l_len == 0) {
        if (s < 0) {
            return -VIBEOS_EINVAL;
        }
        *start = (uint64_t)s;
        *end = VIBEOS_FLK_END;
        return 0;
    }
    if (fl->l_len > 0) {
        if (s > INT64_MAX - (fl->l_len - 1)) {
            return -VIBEOS_EOVERFLOW;
        }
        e = s + fl->l_len - 1;
    } else {
        e = s - 1;
        s = s + fl->l_len;
    }
    if (s < 0) {
        return -VIBEOS_EINVAL;
    }
    *start = (uint64_t)s;
    *end = (uint64_t)e;
    return 0;
}

/* Take a lock, waiting for it if `wait`. This layer does the waiting - the
 * lock table only ever answers - the way a pipe's reader waits: give up the
 * core, try again, and stop for a signal. A wait that could never end is
 * refused first, for record locks a process holds (EDEADLK); Linux looks for
 * no deadlock among description-owned locks, and neither does this. */
static long linux_lock_take(uint32_t space, vibeos_file_t *f, uint64_t owner, uint32_t pid,
                            uint32_t type, uint64_t start, uint64_t end, int wait, int detect) {
    const void *fs;
    uint64_t node, blocker = 0;
    long r;

    linux_lock_key(f, &fs, &node);
    for (;;) {
        r = vibeos_flk_set(space, fs, node, owner, pid, type, start, end, &blocker);
        if (r != -VIBEOS_EAGAIN || !wait) {
            break;
        }
        if (detect && vibeos_flk_wait(owner, blocker) != 0) {
            r = -VIBEOS_EDEADLK;
            break;
        }
        if (ks_current() >= 0 && ks_signal_interrupts(ks_current())) {
            r = -VIBEOS_RESTART_CALL;
            break;
        }
        ks_block_point();
    }
    if (wait && detect) {
        vibeos_flk_wait_done(owner);
    }
    return r;
}

/* F_GETLK, F_SETLK, F_SETLKW and their open-file-description forms. A process
 * owns the first three's locks, the description the others'; both are in one
 * space, so each sees the other's. SQLite's correctness rests on these, which
 * is why they used to answer ENOLCK rather than pretend. */
static long linux_fcntl_lock(vibeos_file_t *f, int cmd, uint64_t uptr) {
    linux_flock_t fl;
    uint64_t start = 0, end = 0, owner;
    uint32_t type, pid = 0, acc = f->flags & VIBEOS_O_ACCMODE;
    int ofd = cmd == LINUX_F_OFD_GETLK || cmd == LINUX_F_OFD_SETLK || cmd == LINUX_F_OFD_SETLKW;
    int get = cmd == LINUX_F_GETLK || cmd == LINUX_F_OFD_GETLK;
    const void *fs;
    uint64_t node;
    long r;

    /* Judged here and not by the row: whether fcntl's third argument is a
     * pointer at all depends on the command, and six commands say it is - more
     * than a row has descriptors for. */
    if (!linux_user_ok(uptr, sizeof(fl), get) ||
        vibeos_uaccess_copy(&fl, (const void *)(uintptr_t)uptr, sizeof(fl)) != 0) {
        return -VIBEOS_EFAULT;
    }
    if (ofd && fl.l_pid != 0) {
        return -VIBEOS_EINVAL;   /* Linux reserves the field for these */
    }
    if (fl.l_type == LINUX_F_RDLCK) {
        type = VIBEOS_FLK_SHARED;
    } else if (fl.l_type == LINUX_F_WRLCK) {
        type = VIBEOS_FLK_EXCL;
    } else if (fl.l_type == LINUX_F_UNLCK && !get) {
        type = VIBEOS_FLK_UNLOCK;
    } else {
        return -VIBEOS_EINVAL;
    }
    r = linux_flock_range(f, &fl, &start, &end);
    if (r != 0) {
        return r;
    }
    if (ofd) {
        owner = VIBEOS_FLK_OWNER_FILE(f);
    } else {
        owner = linux_lock_owner_proc();
        pid = ks_current() >= 0 ? ks_id(ks_current())->tgid : 0u;
    }
    if (get) {
        vibeos_flk_info_t info;
        linux_lock_key(f, &fs, &node);
        if (vibeos_flk_test(VIBEOS_FLK_RECORD, fs, node, owner, type, start, end, &info)) {
            fl.l_type = info.type == VIBEOS_FLK_EXCL ? LINUX_F_WRLCK : LINUX_F_RDLCK;
            fl.l_whence = VIBEOS_SEEK_SET;
            fl.l_start = (int64_t)info.start;
            fl.l_len = info.end == VIBEOS_FLK_END ? 0 : (int64_t)(info.end - info.start + 1u);
            fl.l_pid = info.pid ? (int32_t)info.pid : -1;   /* -1: a description's, no process */
        } else {
            fl.l_type = LINUX_F_UNLCK;   /* and nothing else is touched */
        }
        return vibeos_uaccess_copy((void *)(uintptr_t)uptr, &fl, sizeof(fl)) != 0 ? -VIBEOS_EFAULT : 0;
    }
    /* A read lock needs a descriptor that can read, a write lock one that can
     * write: EBADF, as Linux says. */
    if ((type == VIBEOS_FLK_SHARED && acc == VIBEOS_O_WRONLY) ||
        (type == VIBEOS_FLK_EXCL && acc == VIBEOS_O_RDONLY)) {
        return -VIBEOS_EBADF;
    }
    return linux_lock_take(VIBEOS_FLK_RECORD, f, owner, pid, type, start, end,
                           cmd == LINUX_F_SETLKW || cmd == LINUX_F_OFD_SETLKW, !ofd);
}

/* flock(fd, op): one lock on the whole file, held by the open file description
 * - so a dup and a fork share it, and it goes when the last descriptor naming
 * the description does. It does not see fcntl's locks, nor they it. */
static long linux_sys_flock(uint64_t fd, uint64_t op) {
    vibeos_file_t *f;
    uint32_t type;
    long r;

    switch (VIBEOS_ARG_INT(op) & ~LINUX_LOCK_NB) {
        case LINUX_LOCK_SH: type = VIBEOS_FLK_SHARED; break;
        case LINUX_LOCK_EX: type = VIBEOS_FLK_EXCL; break;
        case LINUX_LOCK_UN: type = VIBEOS_FLK_UNLOCK; break;
        default: return -VIBEOS_EINVAL;
    }
    if (!(f = linux_file_get(fd))) {
        return -VIBEOS_EBADF;
    }
    r = linux_lock_take(VIBEOS_FLK_WHOLE, f, VIBEOS_FLK_OWNER_FILE(f), 0, type, 0, VIBEOS_FLK_END,
                        !(VIBEOS_ARG_INT(op) & LINUX_LOCK_NB), 0);
    vibeos_file_put(f);
    return r;   /* -EAGAIN is EWOULDBLOCK: one number on Linux */
}

/* fcntl(): descriptor flags, status flags, duplicates and record locks. */
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
        case LINUX_F_OFD_GETLK:
        case LINUX_F_OFD_SETLK:
        case LINUX_F_OFD_SETLKW:
            if (!(f = linux_file_get(fd))) {
                return -VIBEOS_EBADF;
            }
            r = linux_fcntl_lock(f, VIBEOS_ARG_INT(cmd), arg);
            vibeos_file_put(f);
            return r;
        default:
            return -VIBEOS_EINVAL;
    }
}

/* ---- metadata ---------------------------------------------------------------------- */

/* Fill a struct stat the caller can believe. Assembled in the kernel and copied
 * out once: filling the user buffer field by field would fault in ring 0 if a
 * sibling munmaps it between the range check and any of these writes (uaccess
 * follow-up to 6a94a32, same class as H-026). */
static long linux_write_stat(uint64_t ubuf, const vibeos_file_stat_t *st, uint64_t dev) {
    linux_stat_t k;
    uint8_t *raw = (uint8_t *)&k;
    uint32_t i;

    for (i = 0; i < sizeof(k); i++) {
        raw[i] = 0;   /* padding included: it is copied out */
    }
    k.st_dev = dev;
    k.st_ino = st->ino;
    k.st_nlink = st->nlink ? st->nlink : 1u;
    k.st_mode = st->mode;
    k.st_uid = st->uid;
    k.st_gid = st->gid;
    k.st_rdev = st->rdev;   /* Linux's encoding of a small number is the kernel's */
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

/* Which device a file is on, as st_dev reports it: the mount's place in the
 * table, from 1. A program that asks whether two names are one file compares
 * st_dev and st_ino - cp and mv do, to refuse copying a file onto itself - and
 * with every filesystem answering 0 a file in /tmp and one on the boot volume
 * with the same inode number were the same file. 0 is what is on no mount: a
 * pipe, a socket, the console. */
uint64_t linux_dev_of(const vibeos_fsmount_t *mnt) {
    uint32_t i;

    for (i = 0; mnt && i < vibeos_fs_mount_count(); i++) {
        if (vibeos_fs_mount_at(i) == mnt) {
            return (uint64_t)i + 1u;
        }
    }
    return 0;
}

/* What a descriptor says about itself: the type decides - a pipe is a FIFO and
 * a socket a socket, where both used to come out as whatever the table entry
 * happened to resemble. */
static long linux_stat_fd(uint64_t fd, vibeos_file_stat_t *st, uint64_t *dev) {
    vibeos_file_t *f = linux_file_get(fd);
    long r;

    if (!f) {
        return -VIBEOS_EBADF;
    }
    vibeos_file_stat_clear(st);
    r = f->ops->stat ? (long)f->ops->stat(f, st) : 0;
    *dev = linux_dev_of(f->mnt);
    vibeos_file_put(f);
    return r < 0 ? r : 0;
}

/* What stat reports for a name, or for `dirfd` itself when the path is empty
 * and AT_EMPTY_PATH is set: newfstatat's and statx's question, asked once.
 * `atflags` has been checked by the caller. */
long linux_stat_get(uint64_t dirfd, uint64_t path_uptr, uint64_t atflags,
                    vibeos_file_stat_t *st, uint64_t *dev) {
    vibeos_path_t w;
    char first = 0;
    long r;

    /* The first byte, to learn whether the path is empty - and the range is
     * judged before it is read (M-082). A path is not one of the pointers a
     * row declares, so nothing had asked whose memory this was, and the copy
     * that follows is fault-tolerant, not permission-checked: ring 0 reads a
     * page that is mapped whoever it is mapped for. So a path pointing at a
     * PROT_NONE page of the program's own, or at the kernel, was read; the
     * call then answered ENOENT if the byte was zero and EFAULT if it was not,
     * which is one bit of memory the program may not read, a call at a time.
     * Found by LTP's statx03, which expects EFAULT and got ENOENT. */
    if (!linux_user_ok(path_uptr, 1u, 0) ||
        vibeos_uaccess_copy(&first, (const void *)(uintptr_t)path_uptr, 1u) != 0) {
        return -VIBEOS_EFAULT;
    }
    if (first == 0) {
        if ((atflags & LINUX_AT_EMPTY_PATH) == 0) {
            return -VIBEOS_ENOENT;
        }
        if (VIBEOS_ARG_INT(dirfd) != LINUX_AT_FDCWD) {
            return linux_stat_fd(dirfd, st, dev);
        }
        r = linux_walk_fd(dirfd, &w);
    } else {
        /* Directory or file? The answer changes what a program does, not just
         * what it prints: ls given a directory lists it and given a file names
         * it, so reporting the wrong one produces a plausible wrong result
         * rather than an error. The filesystem decides; how it decides is its
         * business. */
        r = linux_walk_at(dirfd, path_uptr,
                          (atflags & LINUX_AT_SYMLINK_NOFOLLOW) ? VIBEOS_PATH_NOFOLLOW : 0u, &w);
    }
    if (r != 0) {
        return r;
    }
    vibeos_file_stat_from_node(st, &w.node);
    *dev = linux_dev_of(w.mnt);
    return 0;
}

static long linux_sys_fstat(uint64_t fd, uint64_t ubuf) {
    vibeos_file_stat_t st;
    uint64_t dev = 0;
    long r = linux_stat_fd(fd, &st, &dev);

    return r < 0 ? r : linux_write_stat(ubuf, &st, dev);
}

/* newfstatat(dirfd, path, buf, flags): stat by name, or by fd when the path is
 * empty and AT_EMPTY_PATH is set. */
static long linux_sys_newfstatat(uint64_t dirfd, uint64_t path_uptr, uint64_t ubuf,
                                 uint64_t flags) {
    vibeos_file_stat_t st;
    uint64_t dev = 0;
    long r;

    if (flags & ~(uint64_t)(LINUX_AT_EMPTY_PATH | LINUX_AT_SYMLINK_NOFOLLOW |
                            LINUX_AT_NO_AUTOMOUNT)) {
        return -VIBEOS_EINVAL;
    }
    r = linux_stat_get(dirfd, path_uptr, flags, &st, &dev);
    return r < 0 ? r : linux_write_stat(ubuf, &st, dev);
}

/* stat(path, buf) and lstat(path, buf): the two newfstatat spellings a program
 * built before the *at calls uses. */
static long linux_sys_stat(uint64_t path_uptr, uint64_t ubuf) {
    return linux_sys_newfstatat((uint64_t)(uint32_t)LINUX_AT_FDCWD, path_uptr, ubuf, 0);
}

static long linux_sys_lstat(uint64_t path_uptr, uint64_t ubuf) {
    return linux_sys_newfstatat((uint64_t)(uint32_t)LINUX_AT_FDCWD, path_uptr, ubuf,
                                LINUX_AT_SYMLINK_NOFOLLOW);
}

/* getdents64(fd, buf, len) and getdents, the call before it: records from the
 * directory the descriptor names, as many as fit. The file layer hands out
 * entries by position (vibeos_dirent_t); the record is Linux's and is made
 * here, in one of two shapes - getdents64 has the type after the header,
 * getdents in the record's last byte.
 *
 * Each record carries the position after it in d_off, and the description's
 * position is that - so lseek(fd, 0) starts the directory again and seekdir
 * returns to an entry telldir named. A buffer too small for even one record is
 * EINVAL, where an empty directory is 0. */
static long linux_getdents(uint64_t fd, uint64_t buf, uint64_t len, int old) {
    vibeos_file_t *f = linux_file_get(fd);
    uint64_t used = 0;
    uint32_t steps;
    long r = 0;

    if (!f) {
        return -VIBEOS_EBADF;
    }
    if (!f->ops->readdir) {
        vibeos_file_put(f);
        return -VIBEOS_ENOTDIR;
    }
    /* Bounded: a filesystem whose listing never ends must not hold a core. */
    for (steps = 0; steps < 4096u; steps++) {
        /* The record, built here and copied out whole (M-052): filling the
         * user's buffer field by field faulted in ring 0 if a sibling unmapped
         * it after the range check. Room for the longest name there is. */
        uint64_t rec[(sizeof(linux_dirent64_t) + VIBEOS_NAME_MAX + 2u + 7u) / 8u + 1u];
        vibeos_dirent_t de;
        uint32_t n = 0, k, reclen;
        int got = f->ops->readdir(f, f->pos, &de);

        if (got == VIBEOS_READDIR_END) {
            break;
        }
        if (got == VIBEOS_READDIR_SKIP) {
            f->pos++;
            continue;
        }
        if (got < 0) {
            r = got;
            break;
        }
        while (de.name[n]) {
            n++;
        }
        /* The header, the name and its NUL - and, the old way, the type's own
         * byte - rounded up to 8 as Linux does. */
        reclen = old ? (uint32_t)((offsetof(linux_dirent_t, d_name) + n + 2u + 7u) & ~7u)
                     : (uint32_t)((offsetof(linux_dirent64_t, d_name) + n + 1u + 7u) & ~7u);
        if (used + reclen > len) {
            if (used == 0u) {
                r = -VIBEOS_EINVAL;
            }
            break;
        }
        for (k = 0; k < (uint32_t)(sizeof(rec) / sizeof(rec[0])); k++) {
            rec[k] = 0;
        }
        if (old) {
            linux_dirent_t *d = (linux_dirent_t *)(void *)rec;
            d->d_ino = de.ino;
            d->d_off = f->pos + 1u;
            d->d_reclen = (uint16_t)reclen;
            for (k = 0; k < n; k++) {
                d->d_name[k] = de.name[k];
            }
            ((uint8_t *)rec)[reclen - 1u] = (uint8_t)LINUX_DT_OF(de.mode);
        } else {
            linux_dirent64_t *d = (linux_dirent64_t *)(void *)rec;
            d->d_ino = de.ino;
            d->d_off = (int64_t)(f->pos + 1u);
            d->d_reclen = (uint16_t)reclen;
            d->d_type = (uint8_t)LINUX_DT_OF(de.mode);
            for (k = 0; k < n; k++) {
                d->d_name[k] = de.name[k];
            }
        }
        if (vibeos_uaccess_copy((void *)(uintptr_t)(buf + used), rec, reclen) != 0) {
            r = -VIBEOS_EFAULT;
            break;
        }
        used += reclen;
        f->pos++;
    }
    vibeos_file_put(f);
    return used > 0u ? (long)used : r;
}

/* An ioctl's argument, when the request says it is a pointer. Judged here and
 * not by the row: which requests carry one, and how long, is a table of its
 * own - more than a row has descriptors for. */
static int linux_ioctl_arg(uint64_t arg, uint64_t n, int write) {
    return linux_user_ok(arg, n, write);
}

/* The terminal's requests, on a description that is the console. The modes
 * are kept in Linux's numbering (vibeos/tty.h), so this is a copy field by
 * field between two structures that agree, not a translation. */
static long linux_tty_ioctl(uint32_t req, uint64_t arg) {
    vibeos_tty_modes_t m;
    vibeos_tty_size_t sz;
    linux_termios_t t;
    linux_winsize_t w;
    uint8_t *raw;
    uint32_t i;

    switch (req) {
        case LINUX_TCGETS:
            if (!linux_ioctl_arg(arg, sizeof(t), 1)) {
                return -VIBEOS_EFAULT;
            }
            vibeos_tty_get(&m);
            raw = (uint8_t *)&t;
            for (i = 0; i < sizeof(t); i++) {
                raw[i] = 0;
            }
            t.c_iflag = m.iflag;
            t.c_oflag = m.oflag;
            t.c_cflag = m.cflag;
            t.c_lflag = m.lflag;
            t.c_line = m.line;
            for (i = 0; i < VIBEOS_TTY_NCC; i++) {
                t.c_cc[i] = m.cc[i];
            }
            return vibeos_uaccess_copy((void *)(uintptr_t)arg, &t, sizeof(t)) != 0 ? -VIBEOS_EFAULT : 0;
        case LINUX_TCSETS:
        case LINUX_TCSETSW:
        case LINUX_TCSETSF:
            /* "Now", "once output has drained" - which it always has: nothing
             * is queued for the console - and "drained, with unread input
             * thrown away". */
            if (!linux_ioctl_arg(arg, sizeof(t), 0) ||
                vibeos_uaccess_copy(&t, (const void *)(uintptr_t)arg, sizeof(t)) != 0) {
                return -VIBEOS_EFAULT;
            }
            m.iflag = t.c_iflag;
            m.oflag = t.c_oflag;
            m.cflag = t.c_cflag;
            m.lflag = t.c_lflag;
            m.line = t.c_line;
            for (i = 0; i < VIBEOS_TTY_NCC; i++) {
                m.cc[i] = t.c_cc[i];
            }
            vibeos_tty_set(&m, req == LINUX_TCSETSF);
            return 0;
        case LINUX_TIOCGWINSZ:
            if (!linux_ioctl_arg(arg, sizeof(w), 1)) {
                return -VIBEOS_EFAULT;
            }
            vibeos_tty_get_size(&sz);
            w.ws_row = sz.rows;
            w.ws_col = sz.cols;
            w.ws_xpixel = sz.xpixel;
            w.ws_ypixel = sz.ypixel;
            return vibeos_uaccess_copy((void *)(uintptr_t)arg, &w, sizeof(w)) != 0 ? -VIBEOS_EFAULT : 0;
        case LINUX_TIOCSWINSZ:
            if (!linux_ioctl_arg(arg, sizeof(w), 0) ||
                vibeos_uaccess_copy(&w, (const void *)(uintptr_t)arg, sizeof(w)) != 0) {
                return -VIBEOS_EFAULT;
            }
            sz.rows = w.ws_row;
            sz.cols = w.ws_col;
            sz.xpixel = w.ws_xpixel;
            sz.ypixel = w.ws_ypixel;
            vibeos_tty_set_size(&sz);
            return 0;
        default:
            return 1;   /* not one of these */
    }
}

/* ioctl(). Four requests any descriptor answers - close-on-exec on and off,
 * non-blocking on and off, how much can be read now - then the terminal's on
 * the console, then whatever the file's type answers itself (the console's
 * process group). Everything else is ENOTTY, which is the truthful answer and
 * the one a C library uses to decide a stream is not a terminal and should be
 * block buffered. */
static long linux_sys_ioctl(uint64_t fd, uint64_t req, uint64_t arg) {
    vibeos_procstate_t *ps = linux_cur_ps();
    vibeos_file_t *f = linux_file_get(fd);
    uint32_t r32 = (uint32_t)req;
    int32_t v = 0;
    long r;

    if (!f) {
        return -VIBEOS_EBADF;
    }
    switch (r32) {
        case LINUX_FIOCLEX:
        case LINUX_FIONCLEX:
            r = -VIBEOS_EBADF;
            if (ps) {
                ks_lock(&ps->files_lock, __func__);
                if (vibeos_fdtable_get(&ps->files, fd)) {
                    uint32_t fl = vibeos_fdtable_flags(&ps->files, fd) & ~VIBEOS_FD_CLOEXEC;
                    r = vibeos_fdtable_set_flags(&ps->files, fd,
                            fl | (r32 == LINUX_FIOCLEX ? VIBEOS_FD_CLOEXEC : 0u)) == 0 ? 0 : -VIBEOS_EBADF;
                }
                ks_unlock(&ps->files_lock);
            }
            break;
        case LINUX_FIONBIO:
            if (!linux_ioctl_arg(arg, sizeof(v), 0) ||
                vibeos_uaccess_copy(&v, (const void *)(uintptr_t)arg, sizeof(v)) != 0) {
                r = -VIBEOS_EFAULT;
            } else {
                /* The description's flag, as F_SETFL sets it. */
                f->flags = v ? (f->flags | VIBEOS_O_NONBLOCK) : (f->flags & ~VIBEOS_O_NONBLOCK);
                r = 0;
            }
            break;
        case LINUX_FIONREAD:
            if (f->ops == &vibeos_fops_console) {
                v = (int32_t)vibeos_tty_pending();
            } else if (f->ops == &vibeos_fops_regular) {
                vibeos_file_stat_t st;
                vibeos_file_stat_clear(&st);
                (void)f->ops->stat(f, &st);
                v = st.size > f->pos ? (int32_t)(st.size - f->pos > 0x7fffffffull ? 0x7fffffffull
                                                                                 : st.size - f->pos) : 0;
            } else if (f->ops == &vibeos_fops_pipe && !f->pipe_write) {
                v = (int32_t)vibeos_pipe_pending(f->pipe);
            } else if (f->ops == &linux_fops_inotify) {
                v = (int32_t)linux_inotify_pending(f);
            } else if (f->ops->sockops) {
                v = (int32_t)linux_socket_nread(f);   /* the type's count (L5) */
            } else {
                r = -VIBEOS_ENOTTY;
                break;
            }
            r = !linux_ioctl_arg(arg, sizeof(v), 1) ||
                vibeos_uaccess_copy((void *)(uintptr_t)arg, &v, sizeof(v)) != 0 ? -VIBEOS_EFAULT : 0;
            break;
        case LINUX_RNDGETENTCNT:
            /* The random devices' (L2 step 7, LTP's ioctl07): the same number
             * /proc/sys/kernel/random/entropy_avail prints. */
            if (f->ops != &vibeos_fops_chrdev || (f->rdev != VIBEOS_DEV_RANDOM && f->rdev != VIBEOS_DEV_URANDOM)) {
                r = -VIBEOS_ENOTTY;
                break;
            }
            v = (int32_t)linux_entropy_avail();
            r = !linux_ioctl_arg(arg, sizeof(v), 1) ||
                vibeos_uaccess_copy((void *)(uintptr_t)arg, &v, sizeof(v)) != 0 ? -VIBEOS_EFAULT : 0;
            break;
        default:
            r = f->ops == &vibeos_fops_console ? linux_tty_ioctl(r32, arg) : 1;
            if (r == 1) {
                r = f->ops->ioctl ? f->ops->ioctl(f, req, arg) : -VIBEOS_ENOTTY;
            }
            break;
    }
    vibeos_file_put(f);
    return r;
}

/* ---- paths ----------------------------------------------------------------------------- */

/* unlinkat(dirfd, path, flags) and unlink(path). A directory is EISDIR, as
 * Linux answers unlink on one; AT_REMOVEDIR asks for rmdir (names.c). */
static long linux_sys_unlinkat(uint64_t dirfd, uint64_t path_uptr, uint64_t flags) {
    vibeos_path_t w;
    long r;

    if (flags & ~(uint64_t)LINUX_AT_REMOVEDIR) {
        return -VIBEOS_EINVAL;
    }
    if (flags & LINUX_AT_REMOVEDIR) {
        return linux_rmdir_at(dirfd, path_uptr);
    }
    /* The name itself goes, so a symbolic link is removed and not followed. */
    r = linux_walk_at(dirfd, path_uptr, VIBEOS_PATH_NOFOLLOW, &w);
    if (r != 0) {
        return r;
    }
    if (w.node.is_dir) {
        return -VIBEOS_EISDIR;
    }
    if ((r = linux_may_remove(&w)) != 0) {
        return r;
    }
    return (vibeos_fs_unlink(w.mnt, w.tail) == 0) ? 0 : -VIBEOS_EIO;
}

static long linux_sys_unlink(uint64_t path_uptr) {
    return linux_sys_unlinkat((uint64_t)(uint32_t)LINUX_AT_FDCWD, path_uptr, 0);
}

/* mkdirat(dirfd, path, mode) and mkdir(path, mode). The filesystem makes the
 * directory and is then told the mode, less the umask: a filesystem with
 * nowhere to keep one (FAT, beyond its read-only bit) says so and the directory
 * stands as it made it, which is what mounting such a volume on Linux gives. */
static long linux_sys_mkdirat(uint64_t dirfd, uint64_t path_uptr, uint64_t mode) {
    vibeos_path_t w;
    vibeos_fs_attr_t attr;
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
    if ((r = linux_may_add(&w)) != 0) {
        return r;
    }
    if (vibeos_fs_mkdir(w.mnt, w.tail) != 0) {
        return vibeos_fs_writable(w.mnt) ? -VIBEOS_EIO : -VIBEOS_EROFS;
    }
    attr.valid = VIBEOS_ATTR_MODE;
    attr.mode = (uint32_t)mode & 01777u & ~linux_umask();
    (void)vibeos_fs_setattr(w.mnt, w.tail, &attr);
    linux_own_new(&w);
    return 0;
}

static long linux_sys_mkdir(uint64_t path_uptr, uint64_t mode) {
    return linux_sys_mkdirat((uint64_t)(uint32_t)LINUX_AT_FDCWD, path_uptr, mode);
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
    if (linux_may(&w->node, VIBEOS_MAY_EXEC) != 0) {
        return -VIBEOS_EACCES;   /* a directory one may not search is not one to work in */
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
 * (L1). The one a program uses to find itself, /proc/self/exe, is /proc's
 * since L2 step 6; until then it was answered here from its spelling, there
 * being no /proc to hold it. Anything else is not a link, which is what EINVAL
 * means. */
static long linux_sys_readlinkat(uint64_t dirfd, uint64_t path_uptr, uint64_t ubuf,
                                 uint64_t bufsz) {
    char raw[VIBEOS_PATH_MAX];
    vibeos_path_t w;
    uint64_t n;
    long r, t;

    /* Walked without following the last component: the link is the
     * question. */
    r = linux_walk_at(dirfd, path_uptr, VIBEOS_PATH_NOFOLLOW, &w);
    if (r != 0) {
        return r;
    }
    if ((w.node.mode & VIBEOS_S_IFMT) != VIBEOS_S_IFLNK) {
        return -VIBEOS_EINVAL;
    }
    if ((int64_t)bufsz <= 0) {
        return -VIBEOS_EINVAL;   /* as Linux answers a buffer of no size */
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

static long linux_sys_readlink(uint64_t path_uptr, uint64_t ubuf, uint64_t bufsz) {
    return linux_sys_readlinkat((uint64_t)(uint32_t)LINUX_AT_FDCWD, path_uptr, ubuf, bufsz);
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
    X(4,   stat,        STAT,        PTRS(OUT(1, sizeof(linux_stat_t))), linux_sys_stat(ARG(0), ARG(1))) \
    X(5,   fstat,       FSTAT,       PTRS(OUT(1, sizeof(linux_stat_t))), linux_sys_fstat(ARG(0), ARG(1))) \
    X(6,   lstat,       LSTAT,       PTRS(OUT(1, sizeof(linux_stat_t))), linux_sys_lstat(ARG(0), ARG(1))) \
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
    X(73,  flock,       FLOCK,       NOPTR, linux_sys_flock(ARG(0), ARG(1))) \
    X(78,  getdents,    GETDENTS_OLD, PTRS(OUT_BUF(1, 2)), linux_getdents(ARG(0), ARG(1), ARG(2), 1)) \
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
    X(83,  mkdir,       MKDIR,       NOPTR, linux_sys_mkdir(ARG(0), ARG(1))) \
    X(89,  readlink,    READLINK,    NOPTR, linux_sys_readlink(ARG(0), ARG(1), ARG(2))) \
    X(87,  unlink,      UNLINK,      NOPTR, linux_sys_unlink(ARG(0))) \
    X(217, getdents64,  GETDENTS,    PTRS(OUT_BUF(1, 2)), linux_getdents(ARG(0), ARG(1), ARG(2), 0)) \
    X(257, openat,      OPEN_AT,     NOPTR, linux_sys_openat(ARG(0), ARG(1), ARG(2), ARG(3))) \
    X(258, mkdirat,     MKDIR_AT,    NOPTR, linux_sys_mkdirat(ARG(0), ARG(1), ARG(2))) \
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
    X(319, memfd_create, MEMFD_CREATE, NOPTR, linux_sys_memfd_create(ARG(0), ARG(1))) \
    X(326, copy_file_range, COPY_RANGE, PTRS(OUT_OPT(1, 8), OUT_OPT(3, 8)), linux_sys_copy_file_range(ARG(0), ARG(1), ARG(2), ARG(3), ARG(4), ARG(5))) \
    X(327, preadv2,     PREADV2,     PTRS(IN_VEC(1, 2, sizeof(linux_iovec_t), 1024)), linux_sys_preadv2(ARG(0), ARG(1), ARG(2), ARG(3), ARG(5))) \
    X(328, pwritev2,    PWRITEV2,    PTRS(IN_VEC(1, 2, sizeof(linux_iovec_t), 1024)), linux_sys_pwritev2(ARG(0), ARG(1), ARG(2), ARG(3), ARG(5))) \
    X(436, close_range, CLOSE_RANGE, NOPTR, linux_sys_close_range(ARG(0), ARG(1), ARG(2)))

LINUX_DEFINE_SYSCALLS(fs, LINUX_FS_SYSCALLS)
