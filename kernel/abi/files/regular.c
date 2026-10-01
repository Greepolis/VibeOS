/* Regular files and directories, as files (docs/abi/ A3).
 *
 * Moved from the Linux handlers' `fd >= 3` branches. What changed in moving is
 * where the state lives: the offset, the directory cursor and the unwritten bytes
 * belong to the description, so two descriptors a dup made share them, as every
 * program expects.
 *
 * Since A4 a description knows which mount it is on and its absolute path, and
 * is opened through vibeos/path.h's walk: before, every one of them was on the
 * boot volume, whatever the path said, because nothing consulted the mount
 * table. */

#include <stddef.h>

#include "files_internal.h"
/* getdents64 writes Linux's records: the one operation of this type whose
 * output format is a personality's. Another personality lists a directory
 * through vibeos_fs_list and formats its own. */
#include "vibeos/linux_layout.h"

static void (*g_on_write_back)(void);

void vibeos_files_on_write_back(void (*fn)(void)) {
    g_on_write_back = fn;
}

/* The path inside the file's mount. A mount point itself leaves nothing, and
 * drivers spell their own root "/". */
static const char *tail_of(const vibeos_file_t *f) {
    const char *t = f->path + f->tail;
    return *t ? t : "/";
}

static vibeos_fs_node_t node_of(const vibeos_file_t *f) {
    vibeos_fs_node_t n;
    n.id = f->node;
    n.size = f->size;
    n.is_dir = f->isdir;
    return n;
}

/* Through a kernel buffer, then vibeos_uaccess_copy (M-051). The filesystem used
 * to copy straight into the user's buffer: the dispatcher checks the range before
 * the handler runs, and a sibling thread's munmap between that check and the
 * driver's memcpy faulted in ring 0, outside the one instruction that can recover
 * - a panic any threaded program could cause. The same shape M-040 closed in write().
 *
 * A page, not a small stack buffer: the FAT reader walks the cluster chain from
 * the start on every call, so small chunks make a large read quadratic, and a
 * kernel stack here is 8 KiB or less. If no page is free the read still works,
 * 512 bytes at a time. */
static long regular_pread(vibeos_file_t *f, uint64_t buf, uint64_t len, uint64_t off) {
    vibeos_fs_node_t node = node_of(f);
    uint8_t small[512];
    uint8_t *bounce;
    uint32_t chunk;
    uint64_t done = 0;
    long n = 0;

    if ((f->flags & VIBEOS_O_ACCMODE) == VIBEOS_O_WRONLY) {
        return -VIBEOS_EBADF;
    }
    if (len == 0u) {
        return 0;
    }
    bounce = (uint8_t *)ks_page_alloc();
    chunk = bounce ? 4096u : (uint32_t)sizeof(small);
    if (!bounce) {
        bounce = small;
    }
    while (done < len) {
        uint64_t want = len - done;
        long got;

        if (want > chunk) {
            want = chunk;
        }
        got = vibeos_fs_read_at(f->mnt, &node, off + done, bounce, (uint32_t)want);
        if (got <= 0) {
            n = (done > 0u) ? 0 : got;   /* an error only if nothing was read */
            break;
        }
        if (vibeos_uaccess_copy((void *)(uintptr_t)(buf + done), bounce, (uint64_t)got) != 0) {
            n = (done > 0u) ? 0 : -VIBEOS_EFAULT;
            break;
        }
        done += (uint64_t)got;
        if ((uint64_t)got < want) {
            break;   /* end of file */
        }
    }
    if (bounce != small) {
        ks_page_free(bounce, "read() bounce buffer");
    }
    return done > 0u ? (long)done : n;
}

static long regular_read(vibeos_file_t *f, uint64_t buf, uint64_t len) {
    long n = regular_pread(f, buf, len, f->pos);
    if (n > 0) {
        f->pos += (uint64_t)n;
    }
    return n;
}

/* The file's size as the filesystem has it now: another description, or
 * another process, may have written since this one was opened. */
static uint64_t regular_size(vibeos_file_t *f) {
    vibeos_fs_node_t node;
    if (f->direct && vibeos_fs_lookup(f->mnt, tail_of(f), &node) == 0) {
        f->size = node.size;
    }
    return f->size;
}

/* Write at an offset, straight to the node (L1). Through a kernel page for the
 * reason reads go through one: the filesystem must not touch user memory that
 * a sibling thread can unmap under it. */
static long regular_pwrite_direct(vibeos_file_t *f, uint64_t buf, uint64_t len, uint64_t off) {
    vibeos_fs_node_t node = node_of(f);
    uint8_t small[512];
    uint8_t *bounce;
    uint32_t chunk;
    uint64_t done = 0;
    long n = 0;

    if (len == 0u) {
        return 0;
    }
    if (!f->dirty) {
        /* The volume is about to change, so a staged image may no longer match
         * the file it came from. */
        if (g_on_write_back) {
            g_on_write_back();
        }
        f->dirty = 1;
    }
    bounce = (uint8_t *)ks_page_alloc();
    chunk = bounce ? 4096u : (uint32_t)sizeof(small);
    if (!bounce) {
        bounce = small;
    }
    while (done < len) {
        uint64_t want = len - done;
        long put;

        if (want > chunk) {
            want = chunk;
        }
        if (vibeos_uaccess_copy(bounce, (const void *)(uintptr_t)(buf + done), want) != 0) {
            n = -VIBEOS_EFAULT;
            break;
        }
        put = vibeos_fs_write_at(f->mnt, &node, off + done, bounce, (uint32_t)want);
        if (put <= 0) {
            n = put < 0 ? put : -VIBEOS_ENOSPC;
            break;
        }
        done += (uint64_t)put;
        if ((uint64_t)put < want) {
            break;   /* the filesystem is full: a short write, as Linux reports it */
        }
    }
    if (bounce != small) {
        ks_page_free(bounce, "write() bounce buffer");
    }
    if (off + done > f->size) {
        f->size = off + done;
    }
    return done > 0u ? (long)done : n;
}

static long regular_pwrite(vibeos_file_t *f, uint64_t buf, uint64_t len, uint64_t off) {
    if ((f->flags & VIBEOS_O_ACCMODE) == VIBEOS_O_RDONLY) {
        return -VIBEOS_EBADF;
    }
    if (!f->direct) {
        return -VIBEOS_ESPIPE;   /* a whole-file writer has no offsets to write at */
    }
    return regular_pwrite_direct(f, buf, len, off);
}

/* Buffered, committed on release (the FAT writer stores whole files). Fault-safe:
 * the dispatcher's row validates the buffer before this runs, and that check and
 * this copy are two instants - a sibling thread can munmap the buffer in between
 * (H-010's family). */
static long regular_write(vibeos_file_t *f, uint64_t buf, uint64_t len) {
    uint64_t n = 0;

    if ((f->flags & VIBEOS_O_ACCMODE) == VIBEOS_O_RDONLY) {
        return -VIBEOS_EBADF;
    }
    if (f->direct) {
        /* O_APPEND: every write goes to the end as it is now, whoever else has
         * written since - which is what makes two processes appending to one
         * log interleave lines rather than overwrite each other. */
        uint64_t at = (f->flags & VIBEOS_O_APPEND) ? regular_size(f) : f->pos;
        long w = regular_pwrite_direct(f, buf, len, at);
        if (w > 0) {
            f->pos = at + (uint64_t)w;
        }
        return w;
    }
    if (f->wlen < VIBEOS_FILE_WBUF) {
        uint64_t room = (uint64_t)(VIBEOS_FILE_WBUF - f->wlen);
        n = (len < room) ? len : room;
        if (n > 0u &&
            vibeos_uaccess_copy(&f->wbuf[f->wlen], (const void *)(uintptr_t)buf, n) != 0) {
            return -VIBEOS_EFAULT;
        }
        f->wlen += (uint32_t)n;
    }
    f->dirty = 1;
    return (long)n;
}

static long regular_seek(vibeos_file_t *f, int64_t off, int whence) {
    int64_t base;

    switch (whence) {
        case VIBEOS_SEEK_SET: base = 0; break;
        case VIBEOS_SEEK_CUR: base = (int64_t)f->pos; break;
        case VIBEOS_SEEK_END: base = (int64_t)regular_size(f); break;
        default: return -VIBEOS_EINVAL;
    }
    if ((off > 0 && base > INT64_MAX - off) || base + off < 0) {
        return -VIBEOS_EINVAL;   /* Linux refuses a negative result */
    }
    f->pos = (uint64_t)(base + off);
    return (long)f->pos;
}

/* The mode matters more than it looks: a libc decides how to buffer a stream from
 * it, and a program decides whether to recurse from it. Reporting a regular file
 * for a directory does not fail here - it fails later, inside the program, doing
 * something that made sense given what it was told.
 *
 * Asked of the filesystem again (L1): owner, mode and times change under an
 * open file - chmod, a write through another description - and the node the
 * description was opened on is a snapshot. The description's own size wins,
 * because its unwritten bytes are part of the file this descriptor sees. */
static int regular_stat(vibeos_file_t *f, vibeos_file_stat_t *out) {
    vibeos_fs_node_t node;

    if (vibeos_fs_lookup(f->mnt, tail_of(f), &node) == 0) {
        vibeos_file_stat_from_node(out, &node);
    } else {
        vibeos_file_stat_clear(out);
        out->mode = f->isdir ? (VIBEOS_S_IFDIR | 0755u) : (VIBEOS_S_IFREG | 0644u);
        out->ino = f->node ? f->node : 2u;
    }
    if (!f->direct) {
        out->size = f->isdir ? 0u : f->size;
    }
    return 0;
}

static int regular_truncate(vibeos_file_t *f, uint64_t size) {
    vibeos_fs_node_t node = node_of(f);
    int r;

    if ((f->flags & VIBEOS_O_ACCMODE) == VIBEOS_O_RDONLY) {
        return -VIBEOS_EINVAL;   /* Linux: not open for writing */
    }
    if (!f->direct) {
        /* The buffer is the file: cutting it is all a whole-file writer can
         * do, and it cannot grow one. */
        if (size > f->wlen) {
            return -VIBEOS_EINVAL;
        }
        f->wlen = (uint32_t)size;
        f->dirty = 1;
        return 0;
    }
    r = vibeos_fs_truncate(f->mnt, &node, size);
    if (r == 0) {
        f->size = size;
    }
    return r;
}

static void regular_write_back(vibeos_file_t *f) {
    if (g_on_write_back) {
        g_on_write_back();
    }
    if (vibeos_fs_write_file(f->mnt, tail_of(f), f->wbuf, f->wlen) < 0) {
        ks_log(VIBEOS_LOG_WARN, 70u, f->wlen, 0, "a file's write-back failed at its last close");
    }
}

static int regular_sync(vibeos_file_t *f) {
    if (!f->direct && f->dirty && (f->flags & VIBEOS_O_ACCMODE) != VIBEOS_O_RDONLY) {
        regular_write_back(f);   /* what was written so far, now */
    }
    return vibeos_fs_sync(f->mnt);
}

/* dirent64 records from the directory the description was opened on. The cursor
 * is the description's, like the offset of a file. */
static long dir_getdents(vibeos_file_t *f, uint64_t buf, uint64_t len) {
    uint64_t used = 0;
    uint32_t records = 0;

    /* A bounded syscall must not spin forever if a filesystem backend returns
     * a cyclic directory stream or fails to advance its cursor. */
    while (records < 256u && f->dir_index < 4096u) {
        char name[16];
        int is_dir = 0, n = 0;
        uint16_t reclen;
        uint64_t entry_size = 0;

        if (vibeos_fs_list(f->mnt, tail_of(f), f->dir_index, name,
                           sizeof(name), &entry_size, &is_dir) != 0) {
            break; /* end of directory */
        }
        while (name[n]) {
            n++;
        }
        /* The header, the name and its NUL, rounded up to 8 as Linux does. */
        reclen = (uint16_t)((offsetof(linux_dirent64_t, d_name) + (uint32_t)n + 1u + 7u) & ~7u);
        if (used + reclen > len) {
            break;
        }
        {
            /* Built here and copied out whole (M-052): filling the user's
             * buffer byte by byte faulted in ring 0 if a sibling unmapped it
             * after the range check. Room for the longest name `name` holds. */
            uint64_t rec[(sizeof(linux_dirent64_t) + sizeof(name) + 1u + 7u) / 8u];
            linux_dirent64_t *d = (linux_dirent64_t *)(void *)rec;
            int k;
            for (k = 0; k < (int)(sizeof(rec) / sizeof(rec[0])); k++) {
                rec[k] = 0;
            }
            d->d_reclen = reclen;
            d->d_type = is_dir ? LINUX_DT_DIR : LINUX_DT_REG;
            for (k = 0; k < n; k++) {
                d->d_name[k] = name[k];
            }
            if (vibeos_uaccess_copy((void *)(uintptr_t)(buf + used), rec, reclen) != 0) {
                return (used > 0u) ? (long)used : -VIBEOS_EFAULT;
            }
        }
        used += reclen;
        f->dir_index++;
        records++;
    }
    return (long)used;
}

/* The last descriptor has gone: commit what was written. The volume changed, so
 * a staged image may no longer match the file it came from - dropping it is the
 * whole basis for trusting that cache. A failed write-back has nobody left to be
 * told; close reports it only when it was the last reference, as Linux's close
 * reports what the release said. */
static void regular_release(vibeos_file_t *f) {
    if (!f->direct && (f->flags & VIBEOS_O_ACCMODE) != VIBEOS_O_RDONLY && f->dirty) {
        regular_write_back(f);
    }
}

const vibeos_file_ops_t vibeos_fops_regular = {
    .name = "file",
    .read = regular_read,
    .write = regular_write,
    .seek = regular_seek,
    .stat = regular_stat,
    .release = regular_release,
    .pread = regular_pread,
    .pwrite = regular_pwrite,
    .truncate = regular_truncate,
    .sync = regular_sync,
};

static int dir_sync(vibeos_file_t *f) {
    return vibeos_fs_sync(f->mnt);   /* fsync on a directory is how a rename is made durable */
}

const vibeos_file_ops_t vibeos_fops_dir = {
    .name = "dir",
    .seek = regular_seek,
    .stat = regular_stat,
    .getdents = dir_getdents,
    .sync = dir_sync,
};

/* What vibeos_open_path needs the walk to have done. A file may be missing only
 * if the caller asked for it to be made; and with O_EXCL beside O_CREAT, or
 * O_NOFOLLOW, a symbolic link at the name is the answer rather than what it
 * points at - Linux refuses both rather than following. */
uint32_t vibeos_open_walk_flags(uint32_t flags) {
    uint32_t w = 0;
    if (flags & VIBEOS_O_CREAT) {
        w |= VIBEOS_PATH_CREATE;
    }
    if ((flags & VIBEOS_O_NOFOLLOW) ||
        ((flags & VIBEOS_O_CREAT) && (flags & VIBEOS_O_EXCL))) {
        w |= VIBEOS_PATH_NOFOLLOW;
    }
    return w;
}

vibeos_file_t *vibeos_open_path(const vibeos_path_t *w, uint32_t flags, uint32_t mode,
                                long *err) {
    const char *abs = w->path;
    const char *tail = *w->tail ? w->tail : "/";
    vibeos_fsmount_t *mnt = w->mnt;
    vibeos_fs_node_t node;
    uint32_t acc = flags & VIBEOS_O_ACCMODE;
    int wants_write = acc != VIBEOS_O_RDONLY;
    int direct = 0, legacy_write = 0;
    vibeos_file_t *f;
    uint32_t k;
    long r;

    node.id = 0;
    node.size = 0;
    node.is_dir = 0;
    node.mode = 0;
    if (acc == VIBEOS_O_ACCMODE) {
        *err = -VIBEOS_EINVAL;   /* 3 is not an access mode */
        return 0;
    }
    if (w->exists) {
        node = w->node;
        if ((flags & VIBEOS_O_CREAT) && (flags & VIBEOS_O_EXCL)) {
            *err = -VIBEOS_EEXIST;
            return 0;
        }
        if ((node.mode & VIBEOS_S_IFMT) == VIBEOS_S_IFLNK) {
            *err = -VIBEOS_ELOOP;   /* O_NOFOLLOW met a link */
            return 0;
        }
        if (node.is_dir && (wants_write || (flags & VIBEOS_O_CREAT))) {
            *err = -VIBEOS_EISDIR;
            return 0;
        }
        if (!node.is_dir && (flags & VIBEOS_O_DIRECTORY)) {
            *err = -VIBEOS_ENOTDIR;
            return 0;
        }
        if (!node.is_dir) {
            /* Can this filesystem write into the file where it stands? A
             * zero-length write asks without changing anything. The answer is
             * the filesystem's, not this descriptor's: a read-only descriptor
             * on such a file is "direct" too, so its fstat and SEEK_END see the
             * file as it is now. The first version asked only when opening to
             * write, and a reader's size stayed what it was at open while
             * another descriptor grew the file. */
            r = vibeos_fs_write_at(mnt, &node, 0, 0, 0);
            if (r == 0) {
                direct = 1;
                if (wants_write && (flags & VIBEOS_O_TRUNC)) {
                    r = vibeos_fs_truncate(mnt, &node, 0);
                    node.size = 0;
                }
            } else if (r == -VIBEOS_EROFS && !wants_write) {
                r = 0;   /* reading from a read-only filesystem is what it is for */
            } else if (r == -VIBEOS_EOPNOTSUPP) {
                /* A whole-file writer (FAT, for now). O_WRONLY or O_CREAT opens
                 * to replace the file on release; O_RDWR on its own opens for
                 * reading, because writing into the middle would truncate the
                 * file to what was written. The gap stays visible as EBADF. */
                legacy_write = (acc == VIBEOS_O_WRONLY || (flags & VIBEOS_O_CREAT)) &&
                               vibeos_fs_writable(mnt);
                r = 0;
            }
            if (r != 0) {
                *err = r;
                return 0;
            }
        }
    } else {
        /* The walk only lets a missing name through for O_CREAT. */
        if (w->trailing_slash) {
            *err = -VIBEOS_EISDIR;   /* "name/" cannot become a file */
            return 0;
        }
        r = vibeos_fs_create(mnt, tail, mode & 07777u, &node);
        if (r == 0) {
            direct = 1;
        } else if (r == -VIBEOS_EOPNOTSUPP) {
            legacy_write = 1;   /* made on release, from the bytes written */
        } else {
            *err = r;
            return 0;
        }
    }
    if (legacy_write) {
        flags = (flags & ~VIBEOS_O_ACCMODE) | VIBEOS_O_WRONLY;
    } else if (!direct) {
        flags = (flags & ~VIBEOS_O_ACCMODE) | VIBEOS_O_RDONLY;
    }
    f = vibeos_file_alloc(node.is_dir ? &vibeos_fops_dir : &vibeos_fops_regular,
                          flags & ~(VIBEOS_O_CREAT | VIBEOS_O_EXCL | VIBEOS_O_TRUNC |
                                    VIBEOS_O_DIRECTORY | VIBEOS_O_NOFOLLOW));
    if (!f) {
        *err = -VIBEOS_ENFILE;
        return 0;
    }
    for (k = 0; k + 1u < VIBEOS_FILE_PATH && abs[k]; k++) {
        f->path[k] = abs[k];
    }
    f->path[k] = 0;
    f->mnt = mnt;
    f->tail = (uint32_t)(w->tail - abs);
    f->node = node.id;
    f->size = legacy_write ? 0u : node.size;
    f->isdir = node.is_dir;
    f->direct = direct;
    *err = 0;
    return f;
}
