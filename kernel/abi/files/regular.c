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
static long regular_read(vibeos_file_t *f, uint64_t buf, uint64_t len) {
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
        got = vibeos_fs_read_at(f->mnt, &node, f->pos, bounce, (uint32_t)want);
        if (got <= 0) {
            n = (done > 0u) ? 0 : got;   /* an error only if nothing was read */
            break;
        }
        if (vibeos_uaccess_copy((void *)(uintptr_t)(buf + done), bounce, (uint64_t)got) != 0) {
            n = (done > 0u) ? 0 : -VIBEOS_EFAULT;
            break;
        }
        done += (uint64_t)got;
        f->pos += (uint64_t)got;
        if ((uint64_t)got < want) {
            break;   /* end of file */
        }
    }
    if (bounce != small) {
        ks_page_free(bounce, "read() bounce buffer");
    }
    return done > 0u ? (long)done : n;
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
        case VIBEOS_SEEK_END: base = (int64_t)f->size; break;
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
    out->size = f->isdir ? 0u : f->size;
    return 0;
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
    if ((f->flags & VIBEOS_O_ACCMODE) != VIBEOS_O_RDONLY && f->dirty) {
        if (g_on_write_back) {
            g_on_write_back();
        }
        if (vibeos_fs_write_file(f->mnt, tail_of(f), f->wbuf, f->wlen) < 0) {
            ks_log(VIBEOS_LOG_WARN, 70u, f->wlen, 0, "a file's write-back failed at its last close");
        }
    }
}

const vibeos_file_ops_t vibeos_fops_regular = {
    "file", regular_read, regular_write, regular_seek, regular_stat, 0, 0, regular_release
};

const vibeos_file_ops_t vibeos_fops_dir = {
    "dir", 0, 0, regular_seek, regular_stat, 0, dir_getdents, 0
};

/* O_WRONLY or O_CREAT, as it always was. O_RDWR on its own opens for reading:
 * a write-back replaces the whole file with the bytes written, so writing into
 * the middle of an existing file would truncate it to what was written. That is
 * a gap of the filesystem layer (L1), kept visible as EBADF. */
static int open_writing(uint32_t flags) {
    return (flags & VIBEOS_O_ACCMODE) == VIBEOS_O_WRONLY || (flags & VIBEOS_O_CREAT);
}

/* A file opened to be written is created or replaced when it is released, from
 * the bytes written - the FAT writer stores whole files - so only its directory
 * has to exist now. Anything else must exist, and every directory on the way to
 * it must be one (ENOTDIR otherwise, as Linux says). */
uint32_t vibeos_open_walk_flags(uint32_t flags) {
    return open_writing(flags) ? VIBEOS_PATH_CREATE : 0u;
}

vibeos_file_t *vibeos_open_path(const vibeos_path_t *w, uint32_t flags, long *err) {
    const char *abs = w->path;
    const char *tail = w->tail;
    vibeos_fsmount_t *mnt = w->mnt;
    vibeos_fs_node_t node;
    int writing = open_writing(flags);
    vibeos_file_t *f;
    uint32_t k;

    if (w->exists) {
        node = w->node;
    } else {
        node.id = 0;
        node.size = 0;
        node.is_dir = 0;
    }
    if (writing && w->exists && node.is_dir) {
        *err = -VIBEOS_EISDIR;
        return 0;
    }
    if (writing && !w->exists && w->trailing_slash) {
        *err = -VIBEOS_EISDIR;   /* "name/" cannot become a file */
        return 0;
    }
    if (!writing && !w->exists) {
        *err = -VIBEOS_ENOENT;
        return 0;
    }
    flags &= ~VIBEOS_O_ACCMODE;
    flags |= writing ? VIBEOS_O_WRONLY : VIBEOS_O_RDONLY;
    f = vibeos_file_alloc(node.is_dir && !writing ? &vibeos_fops_dir : &vibeos_fops_regular, flags);
    if (!f) {
        *err = -VIBEOS_ENFILE;
        return 0;
    }
    for (k = 0; k + 1u < VIBEOS_FILE_PATH && abs[k]; k++) {
        f->path[k] = abs[k];
    }
    f->path[k] = 0;
    f->mnt = mnt;
    f->tail = (uint32_t)(tail - abs);
    f->node = node.id;
    f->size = writing ? 0u : node.size;
    f->isdir = node.is_dir && !writing;
    *err = 0;
    return f;
}
