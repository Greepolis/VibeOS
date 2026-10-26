/* NT personality: the file services of W1 (docs/abi/windows.md).
 *
 * NtWriteFile, synchronous and at the file's own position: what a console
 * program's output needs. The rest of the file services, and NtWriteFile's
 * offsets, events and APCs, are W4's. */

#include "nt_internal.h"
#include "vibeos/path.h"
#include "vibeos/fileops.h"
#include "vibeos/cred.h"
#include "vibeos/filelock.h"

/* The file a handle names, if the handle was granted `need` - every bit of it.
 * The only way a file service reaches a file: the mask a handle was opened or
 * duplicated with is what it may do, and a service that took the description
 * straight from the table let a handle opened for its attributes read the data,
 * and a duplicate with fewer rights get them back (v7 review). `granted`, if
 * given, says what the handle may do beyond `need`. */
vibeos_file_t *nt_file_get(uint64_t handle, uint32_t need, uint32_t *granted) {
    vibeos_file_t *f = 0;

    return nt_handle_ref(handle, 0, need, &f, granted) == VIBEOS_STATUS_SUCCESS ? f : 0;
}

/* Why nt_file_get refused: asked again, for the status. */
static uint32_t nt_file_refusal(uint64_t handle, uint32_t need) {
    vibeos_file_t *f = 0;
    uint32_t st = nt_handle_ref(handle, 0, need, &f, 0);

    if (f) {
        vibeos_file_put(f);
    }
    return st == VIBEOS_STATUS_SUCCESS ? VIBEOS_STATUS_INVALID_HANDLE : st;
}

/* Who is asking: the process's credentials, which a Windows program has as a
 * Linux one does (docs/abi/ L2), read under the lock that guards them. Root
 * for a task with no process state - the kernel itself. */
static void nt_cred(vibeos_cred_t *out) {
    int me = ks_current();
    vibeos_procstate_t *ps = me >= 0 ? ks_ps(me) : 0;

    if (!ps) {
        vibeos_cred_root(out);
        return;
    }
    ks_lock(&ps->files_lock, __func__);
    *out = ps->cred;
    ks_unlock(&ps->files_lock);
}

uint64_t nt_handle_of_fd(uint32_t fd) {
    return ((uint64_t)fd + 1u) * 4u;
}

uint32_t nt_status_of_errno(long err) {
    switch (err < 0 ? -err : err) {
        case VIBEOS_EBADF:  return VIBEOS_STATUS_INVALID_HANDLE;
        case VIBEOS_EFAULT: return VIBEOS_STATUS_ACCESS_VIOLATION;
        case VIBEOS_EPIPE:  return VIBEOS_STATUS_PIPE_BROKEN;
        case VIBEOS_ENOMEM: return VIBEOS_STATUS_NO_MEMORY;
        case VIBEOS_EINVAL: return VIBEOS_STATUS_INVALID_PARAMETER;
        case VIBEOS_EACCES:
        case VIBEOS_EPERM:  return VIBEOS_STATUS_ACCESS_DENIED;
        default:            return VIBEOS_STATUS_UNSUCCESSFUL;
    }
}

/* The two offsets that are not offsets (Windows' FILE_WRITE_TO_END_OF_FILE
 * and FILE_USE_FILE_POINTER_POSITION, as LARGE_INTEGER high-low -1:-1 and
 * -1:-2). */
#define NT_OFF_END     0xFFFFFFFFFFFFFFFFull
#define NT_OFF_CURRENT 0xFFFFFFFFFFFFFFFEull

/* NtReadFile and NtWriteFile(File, Event, ApcRoutine, ApcContext,
 * IoStatusBlock, Buffer, Length, ByteOffset, Key), W4 step 2.
 *
 * At ByteOffset when one is given, at the handle's position otherwise or when
 * it says so, at the end for a write that asks; the position moves past what
 * was moved either way, as on a synchronous handle - and every handle here is
 * synchronous: the call returns complete. The event is reset as the call
 * starts and set as it completes; an APC is W5's, with threads that can be
 * alerted, and is refused rather than never called. A read at or past the end
 * is STATUS_END_OF_FILE with nothing moved. The status block is written last. */
/* Does another handle's lock refuse this transfer (W4 step 7)? Windows' locks
 * are mandatory: a write meets any lock but our own, a read an exclusive one. */
static int nt_lock_refuses(vibeos_file_t *f, int write, uint64_t at, uint64_t len) {
    vibeos_flk_info_t held;

    if (len == 0u || !f->mnt || vibeos_flk_count() == 0u || at + len - 1u < at) {
        return 0;
    }
    return vibeos_flk_test(VIBEOS_FLK_RECORD, f->mnt, f->node, VIBEOS_FLK_OWNER_FILE(f),
                           write ? VIBEOS_FLK_EXCL : VIBEOS_FLK_SHARED, at, at + len - 1u, &held) == 1;
}

static long nt_rw(int write, uint64_t file, uint64_t event, uint64_t apc, uint64_t iosb_u,
                  uint64_t buf, uint64_t len, uint64_t offp) {
    vibeos_file_t *f;
    vibeos_nt_iosb_t iosb;
    uint64_t off = NT_OFF_CURRENT;
    long n;
    uint32_t st, need;
    int refused = 0;

    if (apc != 0u) {
        return NT_STATUS(VIBEOS_STATUS_NOT_IMPLEMENTED);
    }
    if (offp != 0u && nt_copy_in(&off, offp, sizeof(off)) != 0) {
        return NT_STATUS(VIBEOS_STATUS_ACCESS_VIOLATION);
    }
    /* FILE_READ_DATA, or FILE_WRITE_DATA - FILE_APPEND_DATA for a write at
     * the end. */
    need = write ? (off == NT_OFF_END ? 0x0004u : 0x0002u) : 0x0001u;
    f = nt_file_get(file, need, 0);
    if (!f) {
        return NT_STATUS(nt_file_refusal(file, need));
    }
    if (event != 0u && (st = nt_io_event(event, 0)) != VIBEOS_STATUS_SUCCESS) {
        vibeos_file_put(f);
        return NT_STATUS(st);
    }
    if (off == NT_OFF_CURRENT || !(write ? f->ops->pwrite : f->ops->pread)) {
        /* `len` is the row's NT_ARG32: 32 bits already, decided in one place. */
        if (off != NT_OFF_CURRENT && off != NT_OFF_END) {
            n = -VIBEOS_EINVAL;   /* a console or a pipe has no offsets */
        } else if (nt_lock_refuses(f, write, f->pos, len)) {
            n = 0;
            refused = 1;
        } else if (write) {
            n = f->ops->write ? f->ops->write(f, buf, len) : -VIBEOS_EBADF;
        } else {
            n = f->ops->read ? f->ops->read(f, buf, len) : -VIBEOS_EBADF;
        }
    } else {
        if (off == NT_OFF_END) {
            vibeos_file_stat_t s;

            vibeos_file_stat_clear(&s);
            off = (f->ops->stat && f->ops->stat(f, &s) == 0) ? s.size : f->size;
        }
        if (off >= 0x8000000000000000ull) {
            n = -VIBEOS_EINVAL;
        } else if (nt_lock_refuses(f, write, off, len)) {
            n = 0;
            refused = 1;
        } else {
            n = write ? f->ops->pwrite(f, buf, len, off) : f->ops->pread(f, buf, len, off);
            if (n >= 0) {
                vibeos_file_pos_take(f);
                f->pos = off + (uint64_t)n;
                vibeos_file_pos_give(f);
            }
        }
    }
    vibeos_file_put(f);
    st = refused ? VIBEOS_STATUS_FILE_LOCK_CONFLICT
       : n < 0 ? nt_status_of_errno(n)
       : (!write && n == 0 && len != 0u) ? VIBEOS_STATUS_END_OF_FILE : VIBEOS_STATUS_SUCCESS;
    iosb.status = st;
    iosb.information = n < 0 ? 0u : (uint64_t)n;
    if (nt_copy_out(iosb_u, &iosb, sizeof(iosb)) != 0) {
        return NT_STATUS(VIBEOS_STATUS_ACCESS_VIOLATION);
    }
    if (event != 0u) {
        (void)nt_io_event(event, 1);
    }
    return NT_STATUS(st);
}

/* ---- NT paths onto the VFS (W2) --------------------------------------------------
 *
 * W2 knows one kind of NT path, the one a DOS path becomes: "\??\C:\dir\file".
 * Drive C: is the boot volume's root, provisionally, as /win is where our DLLs
 * live: the filesystem's hierarchy is the roadmap's next item and decides both.
 * Separators become '/', and a name beyond ASCII becomes UTF-8 (W4 step 6).
 *
 * Asked without case (OBJ_CASE_INSENSITIVE, which is every Win32 open), each
 * component that the filesystem does not find as written is looked for in its
 * directory by folding: FAT compares without case for itself, tmpfs and ext2
 * do not, and a program that wrote FOO.TXT expects to open foo.txt. Folding is
 * ASCII's; a byte beyond it compares exactly. A component found nowhere is
 * left as given, since it may be the one being created. */

static int nt_fold_eq(const char *a, const char *b, uint32_t n) {
    uint32_t i;

    for (i = 0; i < n; i++) {
        char x = a[i], y = b[i];

        if (x >= 'a' && x <= 'z') {
            x = (char)(x - 32);
        }
        if (y >= 'a' && y <= 'z') {
            y = (char)(y - 32);
        }
        if (x != y || b[i] == 0) {
            return 0;
        }
    }
    return b[n] == 0;
}

static void nt_case_resolve(char *path) {
    uint32_t start = 1, end;

    while (path[start]) {
        vibeos_path_t w;
        char saved;

        for (end = start; path[end] && path[end] != '/'; end++) {
        }
        saved = path[end];
        path[end] = 0;
        if (vibeos_path_walk("/", "/", path, 0u, &w) != 0 || !w.exists) {
            vibeos_fsmount_t *m = 0;
            const char *tail = 0;
            char name[VIBEOS_NAME_MAX + 1u];
            uint32_t i, k, found = 0;
            uint64_t size;
            int is_dir;

            path[start - 1u] = 0;   /* the parent: "" is the root */
            if (vibeos_fs_resolve(start == 1u ? "/" : path, &m, &tail) == 0) {
                while (*tail == '/') {
                    tail++;
                }
                for (i = 0; !found && vibeos_fs_list(m, tail, i, name, sizeof(name), &size, &is_dir) == 0; i++) {
                    if (nt_fold_eq(path + start, name, end - start)) {
                        for (k = 0; k < end - start; k++) {
                            path[start + k] = name[k];
                        }
                        found = 1;
                    }
                }
            }
            /* A mount point need not be an entry of the filesystem under it. */
            for (i = 0; !found && i < vibeos_fs_mount_count(); i++) {
                const char *mp = vibeos_fs_mount_path(i);

                for (k = 0; k + 1u < start && mp[k] == path[k]; k++) {
                }
                if (k + 1u == start && mp[k] == '/' && nt_fold_eq(path + start, mp + start, end - start)) {
                    for (k = 0; k < end - start; k++) {
                        path[start + k] = mp[start + k];
                    }
                    found = 1;
                }
            }
            path[start - 1u] = '/';
            if (!found) {
                path[end] = saved;
                return;
            }
        }
        path[end] = saved;
        if (!saved) {
            return;
        }
        start = end + 1u;
    }
}

uint32_t nt_vfs_path(const nt_name_t *n, char *out, uint32_t cap) {
    static const char prefix[] = "\\??\\";
    uint32_t i, o = 0;

    if (n->len < 7u) {
        return VIBEOS_STATUS_OBJECT_NAME_INVALID;
    }
    for (i = 0; i < 4u; i++) {
        if (n->chars[i] != (uint16_t)(uint8_t)prefix[i]) {
            return VIBEOS_STATUS_OBJECT_PATH_SYNTAX_BAD;
        }
    }
    if ((n->chars[4] != 'C' && n->chars[4] != 'c') || n->chars[5] != ':') {
        return VIBEOS_STATUS_OBJECT_PATH_NOT_FOUND;   /* no other drive exists */
    }
    if (n->chars[6] != '\\') {
        return VIBEOS_STATUS_OBJECT_NAME_INVALID;
    }
    for (i = 6; i < n->len; i++) {
        uint32_t c = n->chars[i];
        uint8_t u[4];
        uint32_t k, len;

        if (c >= 0xD800u && c < 0xDC00u && i + 1u < n->len &&
            n->chars[i + 1u] >= 0xDC00u && n->chars[i + 1u] < 0xE000u) {
            c = 0x10000u + ((c - 0xD800u) << 10) + (n->chars[i + 1u] - 0xDC00u);
            i++;
        } else if (c >= 0xD800u && c < 0xE000u) {
            return VIBEOS_STATUS_OBJECT_NAME_INVALID;   /* half a pair */
        }
        if (c == 0u || c == '/') {
            return VIBEOS_STATUS_OBJECT_NAME_INVALID;   /* '/' would split a name */
        }
        if (c < 0x80u) {
            u[0] = c == '\\' ? (uint8_t)'/' : (uint8_t)c;
            len = 1;
        } else if (c < 0x800u) {
            u[0] = (uint8_t)(0xC0u | (c >> 6));
            u[1] = (uint8_t)(0x80u | (c & 0x3Fu));
            len = 2;
        } else if (c < 0x10000u) {
            u[0] = (uint8_t)(0xE0u | (c >> 12));
            u[1] = (uint8_t)(0x80u | ((c >> 6) & 0x3Fu));
            u[2] = (uint8_t)(0x80u | (c & 0x3Fu));
            len = 3;
        } else {
            u[0] = (uint8_t)(0xF0u | (c >> 18));
            u[1] = (uint8_t)(0x80u | ((c >> 12) & 0x3Fu));
            u[2] = (uint8_t)(0x80u | ((c >> 6) & 0x3Fu));
            u[3] = (uint8_t)(0x80u | (c & 0x3Fu));
            len = 4;
        }
        if (o + len + 1u > cap) {
            return VIBEOS_STATUS_OBJECT_NAME_INVALID;
        }
        for (k = 0; k < len; k++) {
            out[o++] = (char)u[k];
        }
    }
    out[o] = 0;
    if (n->insensitive) {
        nt_case_resolve(out);
    }
    return VIBEOS_STATUS_SUCCESS;
}

static uint32_t nt_status_of_open(long err) {
    switch (err < 0 ? -err : err) {
        case VIBEOS_ENOENT:  return VIBEOS_STATUS_OBJECT_NAME_NOT_FOUND;
        case VIBEOS_ENOTDIR: return VIBEOS_STATUS_OBJECT_PATH_NOT_FOUND;
        default:             return nt_status_of_errno(err);
    }
}

/* What a file handle may be asked to do (W2's part of the file rights). */
#define NT_FILE_READ_DATA      0x0001u
#define NT_FILE_WRITE_DATA     0x0002u
#define NT_FILE_APPEND_DATA    0x0004u
#define NT_FILE_WRITE_EA       0x0010u
#define NT_FILE_EXECUTE        0x0020u
#define NT_FILE_WRITE_ATTRIBUTES 0x0100u
#define NT_FILE_ALL_ACCESS     0x001F01FFu
#define NT_FILE_OPENED         1u

static const nt_type_t g_file_type = {
    "File", NT_FILE_ALL_ACCESS, 0x00120089u, 0x00120116u, 0x001200A0u, 0, 0,
};

/* NtCreateFile's dispositions and the options both services read (W4 step 1). */
#define NT_FILE_SUPERSEDE      0u
#define NT_FILE_OPEN           1u
#define NT_FILE_CREATE         2u
#define NT_FILE_OPEN_IF        3u
#define NT_FILE_OVERWRITE      4u
#define NT_FILE_OVERWRITE_IF   5u
#define NT_FILE_DIRECTORY_FILE      0x00000001u
#define NT_FILE_NON_DIRECTORY_FILE  0x00000040u
#define NT_FILE_DELETE_ON_CLOSE     0x00001000u
/* What the status block's Information says happened. */
#define NT_FILE_SUPERSEDED     0u
#define NT_FILE_CREATED        2u
#define NT_FILE_OVERWRITTEN    3u

/* The directory a walk's last component is in, as Linux's openat asks it of
 * the same walk. */
static int nt_parent_of(const vibeos_path_t *w, vibeos_fs_node_t *dir) {
    char parent[VIBEOS_PATH_MAX];
    vibeos_path_t pw;
    uint32_t cut = 0, i;

    for (i = 0; w->path[i]; i++) {
        if (w->path[i] == '/') {
            cut = i;
        }
    }
    for (i = 0; i < cut; i++) {
        parent[i] = w->path[i];
    }
    if (cut == 0u) {
        parent[cut++] = '/';
    }
    parent[cut] = 0;
    if (vibeos_path_walk("/", "/", parent, 0u, &pw) != 0) {
        return -1;
    }
    *dir = pw.node;
    return 0;
}

/* Open or make the file an NT name names, as `disposition` and `options` say,
 * and install a handle to it: NtCreateFile whole, and NtOpenFile as its OPEN.
 *
 * Decided before anything is touched, in Windows' order: the name, then
 * whether it exists and what the disposition makes of that, then whether it is
 * the kind of file the options asked for, then permission. A refusal leaves
 * the file as it was - an OVERWRITE refused for permission must not have
 * truncated anything. */
static long nt_file_open_or_create(uint64_t out, uint32_t want, uint64_t oa, uint64_t iosb_u,
                             uint32_t share, uint32_t disposition, uint32_t options) {
    nt_name_t name;
    uint32_t attributes = 0, st, access, flags, info, may;
    char path[VIBEOS_FILE_PATH];
    vibeos_path_t w;
    vibeos_cred_t who;
    vibeos_file_t *f;
    vibeos_nt_iosb_t iosb;
    long err = 0;
    int rc, isdir;
    uint64_t h;
    int trunc;

    if (disposition > NT_FILE_OVERWRITE_IF || (share & ~7u) != 0u ||
        ((options & NT_FILE_DIRECTORY_FILE) && (options & NT_FILE_NON_DIRECTORY_FILE))) {
        return NT_STATUS(VIBEOS_STATUS_INVALID_PARAMETER);
    }
    /* A directory is made or opened, never superseded or overwritten. */
    if ((options & NT_FILE_DIRECTORY_FILE) && disposition != NT_FILE_CREATE &&
        disposition != NT_FILE_OPEN && disposition != NT_FILE_OPEN_IF) {
        return NT_STATUS(VIBEOS_STATUS_INVALID_PARAMETER);
    }
    st = nt_read_attributes(oa, &name, &attributes);
    if (st == VIBEOS_STATUS_SUCCESS && name.len == 0u) {
        st = VIBEOS_STATUS_OBJECT_NAME_INVALID;
    }
    if (st == VIBEOS_STATUS_SUCCESS) {
        st = nt_vfs_path(&name, path, sizeof(path));
    }
    if (st != VIBEOS_STATUS_SUCCESS) {
        return NT_STATUS(st);
    }
    access = nt_map_access_with(&g_file_type, want);
    if ((options & NT_FILE_DELETE_ON_CLOSE) && !(access & VIBEOS_DELETE)) {
        return NT_STATUS(VIBEOS_STATUS_INVALID_PARAMETER);   /* Windows asks DELETE for it */
    }
    nt_cred(&who);
    rc = vibeos_path_walk_as("/", "/", path, vibeos_open_walk_flags(VIBEOS_O_CREAT), &who, &w);
    if (rc != 0) {
        return NT_STATUS(nt_status_of_open(rc));
    }
    if (w.exists && disposition == NT_FILE_CREATE) {
        return NT_STATUS(VIBEOS_STATUS_OBJECT_NAME_COLLISION);
    }
    if (!w.exists && (disposition == NT_FILE_OPEN || disposition == NT_FILE_OVERWRITE)) {
        return NT_STATUS(VIBEOS_STATUS_OBJECT_NAME_NOT_FOUND);
    }
    isdir = w.exists ? w.node.is_dir : (options & NT_FILE_DIRECTORY_FILE) != 0u;
    if (isdir && (options & NT_FILE_NON_DIRECTORY_FILE)) {
        return NT_STATUS(VIBEOS_STATUS_FILE_IS_A_DIRECTORY);
    }
    if (!isdir && (options & NT_FILE_DIRECTORY_FILE)) {
        return NT_STATUS(VIBEOS_STATUS_NOT_A_DIRECTORY);
    }
    /* What the handle may do to the data decides the description's mode;
     * attributes alone open it to read, which reads nothing. */
    flags = (access & (NT_FILE_WRITE_DATA | NT_FILE_APPEND_DATA))
                ? ((access & NT_FILE_READ_DATA) ? VIBEOS_O_RDWR : VIBEOS_O_WRONLY)
                : VIBEOS_O_RDONLY;
    if (isdir) {
        if (flags != VIBEOS_O_RDONLY && w.exists) {
            return NT_STATUS(VIBEOS_STATUS_FILE_IS_A_DIRECTORY);
        }
        flags = VIBEOS_O_RDONLY | VIBEOS_O_DIRECTORY;
    }
    info = NT_FILE_OPENED;
    if (w.exists && (disposition == NT_FILE_SUPERSEDE || disposition == NT_FILE_OVERWRITE ||
                     disposition == NT_FILE_OVERWRITE_IF)) {
        flags |= VIBEOS_O_TRUNC;
        info = disposition == NT_FILE_SUPERSEDE ? NT_FILE_SUPERSEDED : NT_FILE_OVERWRITTEN;
    }
    if (w.exists) {
        may = ((flags & VIBEOS_O_ACCMODE) != VIBEOS_O_WRONLY && (access & NT_FILE_READ_DATA)
                   ? VIBEOS_MAY_READ : 0u) |
              ((flags & VIBEOS_O_ACCMODE) != VIBEOS_O_RDONLY || (flags & VIBEOS_O_TRUNC)
                   ? VIBEOS_MAY_WRITE : 0u) |
              ((access & NT_FILE_EXECUTE) && !isdir ? VIBEOS_MAY_EXEC : 0u);
        if (may != 0u && vibeos_cred_may(&who, 0, w.node.mode, w.node.uid, w.node.gid, may) != 0) {
            return NT_STATUS(VIBEOS_STATUS_ACCESS_DENIED);
        }
    } else {
        /* A new name in its directory: write and search permission there,
         * which the walk's parent carries. */
        vibeos_fs_node_t dir;

        if (nt_parent_of(&w, &dir) == 0 &&
            vibeos_cred_may(&who, 0, dir.mode, dir.uid, dir.gid, VIBEOS_MAY_WRITE | VIBEOS_MAY_EXEC) != 0) {
            return NT_STATUS(VIBEOS_STATUS_ACCESS_DENIED);
        }
        if (isdir) {
            if (vibeos_fs_mkdir(w.mnt, w.tail) != 0) {
                return NT_STATUS(VIBEOS_STATUS_ACCESS_DENIED);
            }
            rc = vibeos_path_walk_as("/", "/", path, vibeos_open_walk_flags(VIBEOS_O_RDONLY), &who, &w);
            if (rc != 0) {
                return NT_STATUS(nt_status_of_open(rc));
            }
        } else {
            flags |= VIBEOS_O_CREAT;
        }
        info = NT_FILE_CREATED;
    }
    /* The share check comes before the file is overwritten - a refused open
     * changes nothing - so the truncation waits for it. */
    trunc = (flags & VIBEOS_O_TRUNC) != 0u;
    f = vibeos_open_path(&w, flags & ~VIBEOS_O_TRUNC, 0644u, &err);
    if (!f) {
        return NT_STATUS(nt_status_of_open(err));
    }
    if (vibeos_file_share_claim(f, ((access & (NT_FILE_READ_DATA | NT_FILE_EXECUTE)) ? VIBEOS_SHARE_READ : 0u) |
                                       ((access & (NT_FILE_WRITE_DATA | NT_FILE_APPEND_DATA)) ? VIBEOS_SHARE_WRITE : 0u) |
                                       ((access & VIBEOS_DELETE) ? VIBEOS_SHARE_DELETE : 0u),
                                share) != 0) {
        vibeos_file_put(f);
        return NT_STATUS(VIBEOS_STATUS_SHARING_VIOLATION);
    }
    if (trunc && (!f->ops->truncate || f->ops->truncate(f, 0u) != 0)) {
        vibeos_file_put(f);
        return NT_STATUS(VIBEOS_STATUS_ACCESS_DENIED);
    }
    if (options & NT_FILE_DELETE_ON_CLOSE) {
        f->unlink_on_release = 1;   /* the name goes with the last handle */
    }
    st = nt_handle_install(f, access, attributes, &h);
    if (st != VIBEOS_STATUS_SUCCESS) {
        return NT_STATUS(st);
    }
    iosb.status = VIBEOS_STATUS_SUCCESS;
    iosb.information = info;
    if (nt_copy_out(out, &h, sizeof(h)) != 0 ||
        nt_copy_out(iosb_u, &iosb, sizeof(iosb)) != 0) {
        (void)nt_close_handle(h);
        return NT_STATUS(VIBEOS_STATUS_ACCESS_VIOLATION);
    }
    return NT_STATUS(VIBEOS_STATUS_SUCCESS);
}

/* NtOpenFile(FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock,
 * ShareAccess, OpenOptions): NtCreateFile's FILE_OPEN. */
static long nt_open_file(uint64_t out, uint64_t want, uint64_t oa, uint64_t iosb_u,
                         uint64_t share, uint64_t options) {
    return nt_file_open_or_create(out, (uint32_t)want, oa, iosb_u, (uint32_t)share, NT_FILE_OPEN,
                                  (uint32_t)options);
}

/* NtCreateFile(FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock,
 * AllocationSize, FileAttributes, ShareAccess, CreateDisposition,
 * CreateOptions, EaBuffer, EaLength). The allocation size is a hint and the
 * attributes ride on FAT's directory entry, which step 3 sets; extended
 * attributes are refused rather than dropped. */
static long nt_create_file(uint64_t out, uint64_t want, uint64_t oa, uint64_t iosb_u, uint64_t share,
                           uint64_t disposition, uint64_t options, uint64_t ea, uint64_t ea_len) {
    if (ea != 0u && ea_len != 0u) {
        return NT_STATUS(VIBEOS_STATUS_NOT_IMPLEMENTED);
    }
    return nt_file_open_or_create(out, (uint32_t)want, oa, iosb_u, (uint32_t)share, (uint32_t)disposition,
                                  (uint32_t)options);
}

/* ---- a file's information (W4 step 3) ---------------------------------------------
 *
 * The classes a program asks most, laid out as ntifs.h lays them out - the
 * offsets are written beside each, and Wine's tests read every one. Times are
 * Windows', 100 ns since 1601; the attributes are what the mode says: a
 * directory, or a file that is read-only when nobody may write it. */
#define NT_EPOCH_1601_100NS 116444736000000000ull
#define NT_ATTR_READONLY  0x01u
#define NT_ATTR_DIRECTORY 0x10u
#define NT_ATTR_ARCHIVE   0x20u

enum {
    NT_FI_BASIC = 4, NT_FI_STANDARD = 5, NT_FI_INTERNAL = 6, NT_FI_NAME = 9, NT_FI_RENAME = 10,
    NT_FI_DISPOSITION = 13, NT_FI_POSITION = 14, NT_FI_ALL = 18, NT_FI_ALLOCATION = 19,
    NT_FI_END_OF_FILE = 20, NT_FI_NETWORK_OPEN = 34, NT_FI_ATTRIBUTE_TAG = 35
};

static uint64_t nt_time_of(uint64_t ns) {
    return ns == 0u ? 0u : ns / 100u + NT_EPOCH_1601_100NS;
}

static void put64(uint8_t *b, uint32_t at, uint64_t v) {
    uint32_t i;
    for (i = 0; i < 8u; i++) {
        b[at + i] = (uint8_t)(v >> (8u * i));
    }
}

static void put32(uint8_t *b, uint32_t at, uint32_t v) {
    uint32_t i;
    for (i = 0; i < 4u; i++) {
        b[at + i] = (uint8_t)(v >> (8u * i));
    }
}

static uint64_t get64(const uint8_t *b, uint32_t at) {
    uint64_t v = 0;
    uint32_t i;
    for (i = 0; i < 8u; i++) {
        v |= (uint64_t)b[at + i] << (8u * i);
    }
    return v;
}

static uint32_t nt_attrs_of(const vibeos_file_stat_t *s) {
    uint32_t a = (s->mode & 0170000u) == 0040000u ? NT_ATTR_DIRECTORY : NT_ATTR_ARCHIVE;
    return (s->mode & 0222u) == 0u ? a | NT_ATTR_READONLY : a;
}

/* FILE_BASIC_INFORMATION: creation, access, write, change; attributes at 32. */
static void nt_basic(uint8_t *b, uint32_t at, const vibeos_file_stat_t *s) {
    put64(b, at + 0u, nt_time_of(s->ctime_ns));
    put64(b, at + 8u, nt_time_of(s->atime_ns));
    put64(b, at + 16u, nt_time_of(s->mtime_ns));
    put64(b, at + 24u, nt_time_of(s->ctime_ns));
    put32(b, at + 32u, nt_attrs_of(s));
}

/* FILE_STANDARD_INFORMATION: allocation, end of file, links, delete pending,
 * directory. */
static void nt_standard(uint8_t *b, uint32_t at, const vibeos_file_stat_t *s, const vibeos_file_t *f) {
    int dir = (s->mode & 0170000u) == 0040000u;
    put64(b, at + 0u, dir ? 0u : (s->size + 4095u) & ~4095ull);
    put64(b, at + 8u, dir ? 0u : s->size);
    put32(b, at + 16u, s->nlink ? s->nlink : 1u);
    b[at + 20u] = f->unlink_on_release ? 1u : 0u;
    b[at + 21u] = dir ? 1u : 0u;
}

/* FILE_NAME_INFORMATION at `at`: the length in bytes, then the name from the
 * volume's root in UTF-16 - "\\dir\\file" - as much as fits. The bytes it
 * needed, whole. */
/* A VFS name as UTF-16 at b + at, as much as fits below `cap`, '/' as '\'
 * (W4 step 6): the number of UTF-16 units the whole name takes. A byte that
 * does not start a sequence UTF-8 allows is taken as itself, so a name some
 * other system wrote in Latin-1 still comes back as something to open. */
static uint32_t nt_utf16_put(const char *s, uint8_t *b, uint32_t at, uint32_t cap) {
    const uint8_t *p = (const uint8_t *)s;
    uint32_t units = 0;

    while (*p) {
        uint32_t c = *p, more = 0, k;
        uint16_t u[2];
        uint32_t nu = 1;

        if (c >= 0xF0u && c < 0xF5u) {
            c &= 0x07u;
            more = 3;
        } else if (c >= 0xE0u && c < 0xF0u) {
            c &= 0x0Fu;
            more = 2;
        } else if (c >= 0xC2u && c < 0xE0u) {
            c &= 0x1Fu;
            more = 1;
        }
        for (k = 1; k <= more; k++) {
            if ((p[k] & 0xC0u) != 0x80u) {
                break;
            }
        }
        if (k <= more) {
            c = *p;   /* not UTF-8 after all */
            more = 0;
        } else {
            for (k = 1; k <= more; k++) {
                c = (c << 6) | (p[k] & 0x3Fu);
            }
        }
        p += 1u + more;
        if (c == '/') {
            c = '\\';
        }
        if (c >= 0x10000u) {
            u[0] = (uint16_t)(0xD800u + ((c - 0x10000u) >> 10));
            u[1] = (uint16_t)(0xDC00u + ((c - 0x10000u) & 0x3FFu));
            nu = 2;
        } else {
            u[0] = (uint16_t)c;
        }
        for (k = 0; k < nu; k++, units++) {
            if (at + 2u * units + 2u <= cap) {
                b[at + 2u * units] = (uint8_t)u[k];
                b[at + 2u * units + 1u] = (uint8_t)(u[k] >> 8);
            }
        }
    }
    return units;
}

static uint32_t nt_name_info(uint8_t *b, uint32_t at, uint32_t cap, const vibeos_file_t *f) {
    uint32_t n = nt_utf16_put(f->path, b, at + 4u, cap);

    if (cap >= at + 4u) {
        put32(b, at, 2u * n);
    }
    return 4u + 2u * n;
}

/* NtQueryInformationFile(File, IoStatusBlock, Information, Length, Class). */
static long nt_query_information_file(uint64_t file, uint64_t iosb_u, uint64_t out, uint64_t len,
                                      uint64_t cls) {
    uint8_t b[512];
    uint32_t need, done, st = VIBEOS_STATUS_SUCCESS, i;
    vibeos_file_stat_t s;
    vibeos_nt_iosb_t iosb;
    vibeos_file_t *f;

    switch ((uint32_t)cls) {
        case NT_FI_BASIC: need = 40u; break;
        case NT_FI_STANDARD: need = 24u; break;
        case NT_FI_INTERNAL: case NT_FI_POSITION: case NT_FI_ATTRIBUTE_TAG: need = 8u; break;
        case NT_FI_NAME: need = 4u; break;
        case NT_FI_NETWORK_OPEN: need = 56u; break;
        case NT_FI_ALL: need = 100u; break;
        default: return NT_STATUS(VIBEOS_STATUS_INVALID_INFO_CLASS);
    }
    if (len < need) {
        return NT_STATUS(VIBEOS_STATUS_INFO_LENGTH_MISMATCH);
    }
    f = nt_file_get(file, ((uint32_t)cls == 4u || (uint32_t)cls == 18u || (uint32_t)cls == 34u || (uint32_t)cls == 35u ? 0x80u : 0u), 0);
    if (!f) {
        return NT_STATUS(nt_file_refusal(file, ((uint32_t)cls == 4u || (uint32_t)cls == 18u || (uint32_t)cls == 34u || (uint32_t)cls == 35u ? 0x80u : 0u)));
    }
    for (i = 0; i < sizeof(b); i++) {
        b[i] = 0;
    }
    vibeos_file_stat_clear(&s);
    if (f->ops->stat) {
        (void)f->ops->stat(f, &s);
    }
    done = need;
    switch ((uint32_t)cls) {
        case NT_FI_BASIC:
            nt_basic(b, 0, &s);
            break;
        case NT_FI_STANDARD:
            nt_standard(b, 0, &s, f);
            break;
        case NT_FI_INTERNAL:
            put64(b, 0, s.ino);
            break;
        case NT_FI_POSITION:
            put64(b, 0, f->pos);
            break;
        case NT_FI_ATTRIBUTE_TAG:
            put32(b, 0, nt_attrs_of(&s));
            break;
        case NT_FI_NETWORK_OPEN:
            nt_basic(b, 0, &s);       /* the four times, then sizes, then attributes */
            put64(b, 32u, (s.size + 4095u) & ~4095ull);
            put64(b, 40u, s.size);
            put32(b, 48u, nt_attrs_of(&s));
            break;
        default: {
            /* NAME alone, or ALL: basic 0, standard 40, internal 64, EA 72,
             * access 76, position 80, mode 88, alignment 92, name 96. */
            uint32_t at = (uint32_t)cls == NT_FI_NAME ? 0u : 96u;
            uint32_t cap = len < sizeof(b) ? (uint32_t)len : (uint32_t)sizeof(b);
            uint32_t whole;

            if ((uint32_t)cls == NT_FI_ALL) {
                nt_basic(b, 0, &s);
                nt_standard(b, 40u, &s, f);
                put64(b, 64u, s.ino);
                put64(b, 80u, f->pos);
            }
            whole = at + nt_name_info(b, at, cap, f);
            done = whole <= cap ? whole : cap;
            if (whole > cap) {
                st = VIBEOS_STATUS_BUFFER_OVERFLOW;   /* what fitted was written */
            }
            break;
        }
    }
    vibeos_file_put(f);
    if (nt_copy_out(out, b, done) != 0) {
        return NT_STATUS(VIBEOS_STATUS_ACCESS_VIOLATION);
    }
    iosb.status = st;
    iosb.information = done;
    if (nt_copy_out(iosb_u, &iosb, sizeof(iosb)) != 0) {
        return NT_STATUS(VIBEOS_STATUS_ACCESS_VIOLATION);
    }
    return NT_STATUS(st);
}

/* NtSetInformationFile(File, IoStatusBlock, Information, Length, Class). */
static long nt_set_information_file(uint64_t file, uint64_t iosb_u, uint64_t in, uint64_t len,
                                    uint64_t cls) {
    uint8_t b[8u + 8u + 4u + 2u * VIBEOS_FILE_PATH];
    uint32_t need, st = VIBEOS_STATUS_SUCCESS;
    vibeos_nt_iosb_t iosb;
    vibeos_file_t *f;

    switch ((uint32_t)cls) {
        case NT_FI_BASIC: need = 40u; break;
        case NT_FI_POSITION: case NT_FI_END_OF_FILE: case NT_FI_ALLOCATION: need = 8u; break;
        case NT_FI_DISPOSITION: need = 1u; break;
        case NT_FI_RENAME: need = 20u; break;
        default: return NT_STATUS(VIBEOS_STATUS_INVALID_INFO_CLASS);
    }
    if (len < need) {
        return NT_STATUS(VIBEOS_STATUS_INFO_LENGTH_MISMATCH);
    }
    if (len > sizeof(b)) {
        len = sizeof(b);
    }
    if (nt_copy_in(b, in, (uint32_t)len) != 0) {
        return NT_STATUS(VIBEOS_STATUS_ACCESS_VIOLATION);
    }
    f = nt_file_get(file, ((uint32_t)cls == 4u ? 0x100u : ((uint32_t)cls == 10u || (uint32_t)cls == 13u) ? VIBEOS_DELETE : ((uint32_t)cls == 19u || (uint32_t)cls == 20u) ? 0x2u : 0u), 0);
    if (!f) {
        return NT_STATUS(nt_file_refusal(file, ((uint32_t)cls == 4u ? 0x100u : ((uint32_t)cls == 10u || (uint32_t)cls == 13u) ? VIBEOS_DELETE : ((uint32_t)cls == 19u || (uint32_t)cls == 20u) ? 0x2u : 0u)));
    }
    switch ((uint32_t)cls) {
        case NT_FI_POSITION:
            vibeos_file_pos_take(f);
            f->pos = get64(b, 0);
            vibeos_file_pos_give(f);
            break;
        case NT_FI_END_OF_FILE:
            st = !f->ops->truncate ? VIBEOS_STATUS_INVALID_PARAMETER
               : f->ops->truncate(f, get64(b, 0)) == 0 ? VIBEOS_STATUS_SUCCESS
               : VIBEOS_STATUS_ACCESS_DENIED;
            break;
        case NT_FI_ALLOCATION:
            break;   /* a reservation hint: space is not reserved (#17) */
        case NT_FI_DISPOSITION:
            f->unlink_on_release = b[0] ? 1 : 0;   /* the name goes with the last handle */
            break;
        case NT_FI_BASIC: {
            /* Times that are neither 0 (unchanged) nor -1 (do not update),
             * as the access and write times setattr keeps. */
            vibeos_fsmount_t *m = 0;
            const char *tail = 0;
            vibeos_fs_attr_t a;
            uint64_t at = get64(b, 8), wt = get64(b, 16);

            a.valid = 0;
            a.atime_ns = a.mtime_ns = 0;
            if (at != 0u && at != ~0ull && at >= NT_EPOCH_1601_100NS) {
                a.valid |= VIBEOS_ATTR_ATIME;
                a.atime_ns = (at - NT_EPOCH_1601_100NS) * 100u;
            }
            if (wt != 0u && wt != ~0ull && wt >= NT_EPOCH_1601_100NS) {
                a.valid |= VIBEOS_ATTR_MTIME;
                a.mtime_ns = (wt - NT_EPOCH_1601_100NS) * 100u;
            }
            if (a.valid != 0u && (vibeos_fs_resolve(f->path, &m, &tail) != 0 ||
                                  vibeos_fs_setattr(m, *tail ? tail : "/", &a) != 0)) {
                st = VIBEOS_STATUS_ACCESS_DENIED;
            }
            break;
        }
        default: {
            /* FILE_RENAME_INFORMATION: replace if exists at 0, root directory
             * at 8 (refused unless 0), name length at 16, name at 20 - an NT
             * path, onto the VFS as NtCreateFile's is. */
            nt_name_t name;
            uint32_t nl = (uint32_t)b[16] | (uint32_t)b[17] << 8 | (uint32_t)b[18] << 16 |
                          (uint32_t)b[19] << 24, i;
            char to[VIBEOS_FILE_PATH];
            vibeos_fsmount_t *m1 = 0, *m2 = 0;
            const char *t1 = 0, *t2 = 0;

            if (get64(b, 8) != 0u || nl == 0u || (nl & 1u) || 20u + nl > (uint32_t)len ||
                nl / 2u > sizeof(name.chars) / sizeof(name.chars[0])) {
                st = VIBEOS_STATUS_INVALID_PARAMETER;
                break;
            }
            name.len = nl / 2u;
            for (i = 0; i < name.len; i++) {
                name.chars[i] = (uint16_t)(b[20u + 2u * i] | (uint16_t)b[21u + 2u * i] << 8);
            }
            st = nt_vfs_path(&name, to, sizeof(to));
            if (st != VIBEOS_STATUS_SUCCESS) {
                break;
            }
            if (vibeos_fs_resolve(f->path, &m1, &t1) != 0 || vibeos_fs_resolve(to, &m2, &t2) != 0) {
                st = VIBEOS_STATUS_OBJECT_PATH_NOT_FOUND;
            } else if (m1 != m2) {
                st = VIBEOS_STATUS_NOT_SAME_DEVICE;
            } else {
                int r = vibeos_fs_rename(m1, t1, t2, b[0] ? 0u : VIBEOS_RENAME_NOREPLACE);
                st = r == 0 ? VIBEOS_STATUS_SUCCESS
                   : r == -VIBEOS_EEXIST ? VIBEOS_STATUS_OBJECT_NAME_COLLISION
                   : nt_status_of_open(r);
                if (r == 0) {
                    vibeos_file_paths_moved(f->path, to);
                }
            }
            break;
        }
    }
    vibeos_file_put(f);
    iosb.status = st;
    iosb.information = 0;
    if (nt_copy_out(iosb_u, &iosb, sizeof(iosb)) != 0) {
        return NT_STATUS(VIBEOS_STATUS_ACCESS_VIOLATION);
    }
    return NT_STATUS(st);
}

/* ---- the small services (W4 step 4) ------------------------------------------------- */

/* The name an object-attributes block names, walked as the caller: 0 and the
 * walk, or the status. */
static uint32_t nt_walk_attributes(uint64_t oa, vibeos_path_t *w) {
    nt_name_t name;
    uint32_t attributes = 0, st;
    char path[VIBEOS_FILE_PATH];
    vibeos_cred_t who;
    int rc;

    st = nt_read_attributes(oa, &name, &attributes);
    if (st == VIBEOS_STATUS_SUCCESS && name.len == 0u) {
        st = VIBEOS_STATUS_OBJECT_NAME_INVALID;
    }
    if (st == VIBEOS_STATUS_SUCCESS) {
        st = nt_vfs_path(&name, path, sizeof(path));
    }
    if (st != VIBEOS_STATUS_SUCCESS) {
        return st;
    }
    nt_cred(&who);
    rc = vibeos_path_walk_as("/", "/", path, vibeos_open_walk_flags(VIBEOS_O_RDONLY), &who, w);
    return rc != 0 ? nt_status_of_open(rc) : VIBEOS_STATUS_SUCCESS;
}

/* NtQueryAttributesFile(ObjectAttributes, FILE_BASIC_INFORMATION) and
 * NtQueryFullAttributesFile(ObjectAttributes, FILE_NETWORK_OPEN_INFORMATION):
 * what a handle would say, without opening one. */
static long nt_query_attributes(uint64_t oa, uint64_t out, int full) {
    vibeos_path_t w;
    vibeos_file_stat_t s;
    uint8_t b[56];
    uint32_t i, st = nt_walk_attributes(oa, &w);

    if (st != VIBEOS_STATUS_SUCCESS) {
        return NT_STATUS(st);
    }
    for (i = 0; i < sizeof(b); i++) {
        b[i] = 0;
    }
    vibeos_file_stat_from_node(&s, &w.node);
    nt_basic(b, 0, &s);
    if (full) {
        put64(b, 32u, (s.size + 4095u) & ~4095ull);
        put64(b, 40u, s.size);
        put32(b, 48u, nt_attrs_of(&s));
    }
    if (nt_copy_out(out, b, full ? 56u : 40u) != 0) {
        return NT_STATUS(VIBEOS_STATUS_ACCESS_VIOLATION);
    }
    return NT_STATUS(VIBEOS_STATUS_SUCCESS);
}

/* NtDeleteFile(ObjectAttributes): the name goes now - a file unlinked, an
 * empty directory removed. */
static long nt_delete_file(uint64_t oa) {
    vibeos_path_t w;
    vibeos_cred_t who;
    vibeos_fs_node_t dir;
    uint32_t st = nt_walk_attributes(oa, &w);
    int r;

    if (st != VIBEOS_STATUS_SUCCESS) {
        return NT_STATUS(st);
    }
    nt_cred(&who);
    if (nt_parent_of(&w, &dir) == 0 &&
        vibeos_cred_may(&who, 0, dir.mode, dir.uid, dir.gid, VIBEOS_MAY_WRITE | VIBEOS_MAY_EXEC) != 0) {
        return NT_STATUS(VIBEOS_STATUS_ACCESS_DENIED);
    }
    r = w.node.is_dir ? vibeos_fs_rmdir(w.mnt, w.tail) : vibeos_fs_unlink(w.mnt, w.tail);
    if (r == -VIBEOS_ENOTEMPTY) {
        return NT_STATUS(VIBEOS_STATUS_DIRECTORY_NOT_EMPTY);
    }
    return NT_STATUS(r == 0 ? VIBEOS_STATUS_SUCCESS
                     : r == -VIBEOS_EBUSY || r == -VIBEOS_EPERM ? VIBEOS_STATUS_CANNOT_DELETE
                     : nt_status_of_open(r));
}

/* A status block saying `st` and `n`; the status, or an access violation. */
static long nt_iosb_done(uint64_t iosb_u, uint32_t st, uint64_t n) {
    vibeos_nt_iosb_t iosb;

    iosb.status = st;
    iosb.information = n;
    if (nt_copy_out(iosb_u, &iosb, sizeof(iosb)) != 0) {
        return NT_STATUS(VIBEOS_STATUS_ACCESS_VIOLATION);
    }
    return NT_STATUS(st);
}

/* NtFlushBuffersFile(File, IoStatusBlock): the description's sync. */
static long nt_flush_buffers_file(uint64_t file, uint64_t iosb_u) {
    vibeos_file_t *f = nt_file_get(file, 0x2u, 0);
    int r;

    if (!f) {
        return NT_STATUS(nt_file_refusal(file, 0x2u));
    }
    r = f->ops->sync ? f->ops->sync(f) : 0;
    vibeos_file_put(f);
    return nt_iosb_done(iosb_u, r == 0 ? VIBEOS_STATUS_SUCCESS : nt_status_of_errno(r), 0);
}

/* The cancels: nothing is ever pending here - every I/O completes before its
 * call returns - so NtCancelIoFile has cancelled all there was, and the two
 * that name one request have found none. */
static long nt_cancel_io_file(uint64_t file, uint64_t iosb_u) {
    vibeos_file_t *f = nt_file_get(file, 0u, 0);

    if (!f) {
        return NT_STATUS(nt_file_refusal(file, 0u));
    }
    vibeos_file_put(f);
    return nt_iosb_done(iosb_u, VIBEOS_STATUS_SUCCESS, 0);
}

static long nt_cancel_io_file_ex(uint64_t file) {
    vibeos_file_t *f = nt_file_get(file, 0u, 0);

    if (!f) {
        return NT_STATUS(nt_file_refusal(file, 0u));
    }
    vibeos_file_put(f);
    return NT_STATUS(VIBEOS_STATUS_NOT_FOUND);
}

/* NtQueryVolumeInformationFile(File, IoStatusBlock, Information, Length,
 * Class): Volume (1), Size (3), Device (4), Attribute (5), FullSize (7). */
static long nt_query_volume_information_file(uint64_t file, uint64_t iosb_u, uint64_t out,
                                             uint64_t len, uint64_t cls) {
    uint8_t b[64];
    uint32_t need, done, i, st = VIBEOS_STATUS_SUCCESS;
    vibeos_fsmount_t *m = 0;
    const char *tail = 0, *type;
    vibeos_fs_statfs_t sf;
    vibeos_file_t *f;

    switch ((uint32_t)cls) {
        case 1: need = 18u; break;
        case 3: need = 24u; break;
        case 4: need = 8u; break;
        case 5: need = 12u; break;
        case 7: need = 32u; break;
        default: return NT_STATUS(VIBEOS_STATUS_INVALID_INFO_CLASS);
    }
    if (len < need) {
        return NT_STATUS(VIBEOS_STATUS_INFO_LENGTH_MISMATCH);
    }
    f = nt_file_get(file, 0u, 0);
    if (!f) {
        return NT_STATUS(nt_file_refusal(file, 0u));
    }
    for (i = 0; i < sizeof(b); i++) {
        b[i] = 0;
    }
    sf.block_size = 512u;
    sf.blocks = sf.blocks_free = 0;
    type = "VibeFS";
    if (f->path[0] == '/' && vibeos_fs_resolve(f->path, &m, &tail) == 0 && m) {
        (void)vibeos_fs_statfs(m, &sf);
        type = (m->type && m->type[0] == 'f') ? "FAT" : "NTFS";
    }
    if (sf.block_size == 0u) {
        sf.block_size = 512u;
    }
    vibeos_file_put(f);
    done = need;
    switch ((uint32_t)cls) {
        case 1:
            break;   /* no creation time, serial 0, no label */
        case 3:
            put64(b, 0, sf.blocks);
            put64(b, 8, sf.blocks_free);
            put32(b, 16, 1u);
            put32(b, 20, sf.block_size);
            break;
        case 7:
            put64(b, 0, sf.blocks);
            put64(b, 8, sf.blocks_free);
            put64(b, 16, sf.blocks_free);
            put32(b, 24, 1u);
            put32(b, 28, sf.block_size);
            break;
        case 4:
            put32(b, 0, 7u);   /* FILE_DEVICE_DISK */
            break;
        default: {
            /* Case-preserved names, Unicode on disk; the name in UTF-16. */
            uint32_t n = 0, cap = len < sizeof(b) ? (uint32_t)len : (uint32_t)sizeof(b);

            put32(b, 0, 0x2u | 0x4u);
            put32(b, 4, 255u);
            while (type[n]) {
                n++;
            }
            put32(b, 8, 2u * n);
            for (i = 0; i < n && 12u + 2u * i + 2u <= cap; i++) {
                b[12u + 2u * i] = (uint8_t)type[i];
            }
            done = 12u + 2u * n;
            if (done > cap) {
                done = cap;
                st = VIBEOS_STATUS_BUFFER_OVERFLOW;
            }
            break;
        }
    }
    if (nt_copy_out(out, b, done) != 0) {
        return NT_STATUS(VIBEOS_STATUS_ACCESS_VIOLATION);
    }
    return nt_iosb_done(iosb_u, st, done);
}

/* ---- directories (W4 step 5) ---------------------------------------------------------- */

static char nt_fold(char c) {
    return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
}

/* A mask with '*' (any run) and '?' (any one), without case, as Windows
 * matches names: "*" and "*.*" match everything, a dot included or not. */
static int nt_mask_match(const char *m, const char *n) {
    if (m[0] == 0) {
        return 1;
    }
    if (m[0] == '*' && m[1] == '.' && m[2] == '*' && m[3] == 0) {
        return 1;
    }
    while (*m) {
        if (*m == '*') {
            m++;
            if (*m == 0) {
                return 1;
            }
            for (; *n; n++) {
                if (nt_mask_match(m, n)) {
                    return 1;
                }
            }
            return 0;
        }
        if (*n == 0 || (*m != '?' && nt_fold(*m) != nt_fold(*n))) {
            return 0;
        }
        m++;
        n++;
    }
    return *n == 0;
}

/* Where each class puts the name, and how much is before it: Directory (1)
 * 64, FullDirectory (2) 68, BothDirectory (3) 94, Names (12) 12,
 * IdBothDirectory (37) 104, IdFullDirectory (38) 80. 0: not a class here. */
static uint32_t nt_dir_name_at(uint32_t cls) {
    switch (cls) {
        case 1: return 64u;
        case 2: return 68u;
        case 3: return 94u;
        case 12: return 12u;
        case 37: return 104u;
        case 38: return 80u;
        default: return 0u;
    }
}

/* NtQueryDirectoryFile(File, Event, ApcRoutine, ApcContext, IoStatusBlock,
 * FileInformation, Length, FileInformationClass, ReturnSingleEntry, FileName,
 * RestartScan): the entries after the handle's position that match its mask,
 * as many as fit, each 8-aligned and chained by NextEntryOffset. The mask is
 * taken from FileName on the first call or a restart and kept. Nothing at all
 * on a first call is NO_SUCH_FILE; nothing more after that NO_MORE_FILES. */
static long nt_query_directory_file(uint64_t file, uint64_t event, uint64_t apc, uint64_t iosb_u,
                                    uint64_t out, uint64_t len, uint64_t cls, uint64_t single,
                                    uint64_t mask_u, uint64_t restart) {
    uint8_t b[4096];
    uint32_t name_at = nt_dir_name_at((uint32_t)cls), used = 0, last = 0, st, cap, entries = 0;
    vibeos_file_t *f;
    int first, short_of = 0;

    if (apc != 0u) {
        return NT_STATUS(VIBEOS_STATUS_NOT_IMPLEMENTED);   /* W5's */
    }
    if (name_at == 0u) {
        return NT_STATUS(VIBEOS_STATUS_INVALID_INFO_CLASS);
    }
    if (len < name_at) {
        return NT_STATUS(VIBEOS_STATUS_INFO_LENGTH_MISMATCH);
    }
    f = nt_file_get(file, 0x1u, 0);
    if (!f) {
        return NT_STATUS(nt_file_refusal(file, 0x1u));
    }
    if (!f->ops->readdir) {
        vibeos_file_put(f);
        return NT_STATUS(VIBEOS_STATUS_INVALID_PARAMETER);   /* not a directory */
    }
    if (event != 0u && (st = nt_io_event(event, 0)) != VIBEOS_STATUS_SUCCESS) {
        vibeos_file_put(f);
        return NT_STATUS(st);
    }
    vibeos_file_pos_take(f);
    if (restart) {
        f->pos = 0;
    }
    first = f->pos == 0u;
    if (first && mask_u != 0u) {
        /* UNICODE_STRING: Length (bytes) at 0, Buffer at 8. */
        uint8_t us[16];
        uint16_t chars[63];
        uint32_t n, k;

        f->nt_mask[0] = 0;
        if (nt_copy_in(us, mask_u, sizeof(us)) == 0) {
            n = ((uint32_t)us[0] | (uint32_t)us[1] << 8) / 2u;
            n = n < 63u ? n : 63u;
            if (n != 0u && nt_copy_in(chars, get64(us, 8), 2u * n) == 0) {
                for (k = 0; k < n; k++) {
                    f->nt_mask[k] = chars[k] < 0x80u ? (char)chars[k] : '?';
                }
                f->nt_mask[n] = 0;
            }
        }
    } else if (first) {
        f->nt_mask[0] = 0;
    }
    cap = len < sizeof(b) ? (uint32_t)len : (uint32_t)sizeof(b);
    for (;;) {
        vibeos_dirent_t de;
        vibeos_file_stat_t s;
        uint32_t nl = 0, rec, k, at;
        int got = f->ops->readdir(f, f->pos, &de);

        if (got == VIBEOS_READDIR_END) {
            break;
        }
        if (got == VIBEOS_READDIR_SKIP || !nt_mask_match(f->nt_mask, de.name)) {
            f->pos++;
            continue;
        }
        nl = nt_utf16_put(de.name, b, 0u, 0u);   /* its length in UTF-16 units */
        rec = name_at + 2u * nl;
        at = (used + 7u) & ~7u;
        if (at + rec > cap) {
            if (entries != 0u) {
                break;
            }
            /* Not even one fits: as much of it as fits - its fixed part is
             * always there, the length check saw to that - and the warning;
             * the position stays, so the next call gets it whole. */
            rec = cap - at;
            short_of = 1;
        }
        for (k = at; k < at + rec; k++) {
            b[k] = 0;
        }
        vibeos_file_stat_clear(&s);
        s.mode = de.mode;
        s.ino = de.ino;
        {
            /* What stat would say of it: its size and times, by its path. */
            char child[VIBEOS_FILE_PATH];
            uint32_t p = 0, q = 0;
            vibeos_path_t w;

            while (f->path[p] && p + 1u < sizeof(child)) {
                child[p] = f->path[p];
                p++;
            }
            if (p > 1u && p + 1u < sizeof(child)) {
                child[p++] = '/';
            }
            while (de.name[q] && p + 1u < sizeof(child)) {
                child[p++] = de.name[q++];
            }
            child[p] = 0;
            if (vibeos_path_walk("/", "/", child, 0u, &w) == 0) {
                vibeos_file_stat_from_node(&s, &w.node);
            }
        }
        if ((uint32_t)cls == 12u) {
            put32(b, at + 4u, (uint32_t)f->pos);
            put32(b, at + 8u, 2u * nl);
        } else {
            put32(b, at + 4u, (uint32_t)f->pos);
            nt_basic(b, at + 8u, &s);   /* the four times; its attributes word lands on EndOfFile and is overwritten */
            put64(b, at + 40u, (s.mode & 0170000u) == 0040000u ? 0u : s.size);
            put64(b, at + 48u, (s.mode & 0170000u) == 0040000u ? 0u : (s.size + 4095u) & ~4095ull);
            put32(b, at + 56u, nt_attrs_of(&s));
            put32(b, at + 60u, 2u * nl);
            if ((uint32_t)cls == 37u) {
                put64(b, at + 96u, s.ino);
            } else if ((uint32_t)cls == 38u) {
                put64(b, at + 72u, s.ino);
            }
        }
        (void)nt_utf16_put(de.name, b, at + name_at, at + rec);
        if (short_of) {
            used = at + rec;
            break;
        }
        if (entries != 0u) {
            put32(b, last, at - last);   /* the previous entry's NextEntryOffset */
        }
        last = at;
        used = at + rec;
        entries++;
        f->pos++;
        if (single) {
            break;
        }
    }
    st = short_of ? VIBEOS_STATUS_BUFFER_OVERFLOW
       : entries != 0u ? VIBEOS_STATUS_SUCCESS
       : first ? VIBEOS_STATUS_NO_SUCH_FILE : VIBEOS_STATUS_NO_MORE_FILES;
    vibeos_file_pos_give(f);
    vibeos_file_put(f);
    if (used != 0u && nt_copy_out(out, b, used) != 0) {
        return NT_STATUS(VIBEOS_STATUS_ACCESS_VIOLATION);
    }
    if (event != 0u) {
        (void)nt_io_event(event, 1);
    }
    return nt_iosb_done(iosb_u, st, used);
}

/* ---- byte-range locks (W4 step 7) ----------------------------------------------------
 *
 * Windows' locks belong to the handle's file object and are mandatory: a read
 * or write through another handle that meets one is refused, not merely
 * advised against. They live in the record-lock table under the description
 * as owner - the OFD rule, which is Windows' - and go when it does. The Key
 * argument, which lets one handle hold locks under several names, is not
 * kept: every lock of a handle is the handle's. */

static uint32_t nt_lock_range(uint64_t off_u, uint64_t len_u, uint64_t *start, uint64_t *end) {
    uint64_t off, len;

    if (nt_copy_in(&off, off_u, 8u) != 0 ||
        nt_copy_in(&len, len_u, 8u) != 0) {
        return VIBEOS_STATUS_ACCESS_VIOLATION;
    }
    if (len == 0u || off + len - 1u < off) {
        return VIBEOS_STATUS_INVALID_PARAMETER;   /* nothing, or past the end of numbers */
    }
    *start = off;
    *end = off + len - 1u;
    return VIBEOS_STATUS_SUCCESS;
}

/* NtLockFile(File, Event, ApcRoutine, ApcContext, IoStatusBlock, ByteOffset,
 * Length, Key, FailImmediately, ExclusiveLock). A lock that cannot be had
 * now is LOCK_NOT_GRANTED whether or not the caller would wait: waiting is
 * W5's, with the APCs it completes through. */
static long nt_lock_file(uint64_t file, uint64_t event, uint64_t apc, uint64_t iosb_u, uint64_t off_u,
                         uint64_t len_u, uint64_t exclusive) {
    uint64_t start = 0, end = 0;
    uint32_t st, granted = 0;
    vibeos_file_t *f;
    int r;

    if (apc != 0u) {
        return NT_STATUS(VIBEOS_STATUS_NOT_IMPLEMENTED);
    }
    st = nt_lock_range(off_u, len_u, &start, &end);
    if (st != VIBEOS_STATUS_SUCCESS) {
        return NT_STATUS(st);
    }
    f = nt_file_get(file, 0u, &granted);
    if (!f) {
        return NT_STATUS(nt_file_refusal(file, 0u));
    }
    if ((granted & 0x3u) == 0u) {   /* FILE_READ_DATA or FILE_WRITE_DATA, as Windows asks */
        vibeos_file_put(f);
        return NT_STATUS(VIBEOS_STATUS_ACCESS_DENIED);
    }
    if (!f->mnt || f->isdir) {
        vibeos_file_put(f);
        return NT_STATUS(VIBEOS_STATUS_INVALID_PARAMETER);
    }
    if (event != 0u && (st = nt_io_event(event, 0)) != VIBEOS_STATUS_SUCCESS) {
        vibeos_file_put(f);
        return NT_STATUS(st);
    }
    r = vibeos_flk_set(VIBEOS_FLK_RECORD, f->mnt, f->node, VIBEOS_FLK_OWNER_FILE(f), 0u,
                       exclusive ? VIBEOS_FLK_EXCL : VIBEOS_FLK_SHARED, start, end, 0);
    vibeos_file_put(f);
    st = r == 0 ? VIBEOS_STATUS_SUCCESS
       : VIBEOS_STATUS_LOCK_NOT_GRANTED;   /* in the way, or no room: either way not granted */
    if (event != 0u && st == VIBEOS_STATUS_SUCCESS) {
        (void)nt_io_event(event, 1);
    }
    return nt_iosb_done(iosb_u, st, 0u);
}

/* NtUnlockFile(File, IoStatusBlock, ByteOffset, Length, Key): exactly a range
 * this handle locked. */
static long nt_unlock_file(uint64_t file, uint64_t iosb_u, uint64_t off_u, uint64_t len_u) {
    uint64_t start = 0, end = 0;
    vibeos_flk_info_t held;
    uint32_t st, granted = 0;
    vibeos_file_t *f;

    st = nt_lock_range(off_u, len_u, &start, &end);
    if (st != VIBEOS_STATUS_SUCCESS) {
        return NT_STATUS(st);
    }
    f = nt_file_get(file, 0u, &granted);
    if (!f) {
        return NT_STATUS(nt_file_refusal(file, 0u));
    }
    if ((granted & 0x3u) == 0u) {   /* FILE_READ_DATA or FILE_WRITE_DATA, as Windows asks */
        vibeos_file_put(f);
        return NT_STATUS(VIBEOS_STATUS_ACCESS_DENIED);
    }
    /* Is it ours? Asked as somebody else: our own lock is then in the way,
     * and only a lock that starts and ends where this one does is a lock
     * Windows would give back. */
    if (!f->mnt ||
        vibeos_flk_test(VIBEOS_FLK_RECORD, f->mnt, f->node, 0u, VIBEOS_FLK_EXCL, start, end, &held) != 1 ||
        held.owner != VIBEOS_FLK_OWNER_FILE(f) || held.start != start || held.end != end) {
        vibeos_file_put(f);
        return NT_STATUS(VIBEOS_STATUS_RANGE_NOT_LOCKED);
    }
    (void)vibeos_flk_set(VIBEOS_FLK_RECORD, f->mnt, f->node, VIBEOS_FLK_OWNER_FILE(f), 0u, VIBEOS_FLK_UNLOCK,
                         start, end, 0);
    vibeos_file_put(f);
    return nt_iosb_done(iosb_u, VIBEOS_STATUS_SUCCESS, 0u);
}

#define NT_FILE_SERVICES(X) \
    X(VIBEOS_NT_NR_NtOpenFile, NtOpenFile, OPEN, \
      PTRS(OUT(0, 8), OUT(3, sizeof(vibeos_nt_iosb_t))), \
      nt_open_file(ARG(0), NT_ARG32(1), ARG(2), ARG(3), NT_ARG32(4), NT_ARG32(5))) \
    X(VIBEOS_NT_NR_NtCreateFile, NtCreateFile, OPEN, \
      PTRS(OUT(0, 8), OUT(3, sizeof(vibeos_nt_iosb_t))), \
      nt_create_file(ARG(0), NT_ARG32(1), ARG(2), ARG(3), NT_ARG32(6), NT_ARG32(7), NT_ARG32(8), ARG(9), \
                     NT_ARG32(10))) \
    X(VIBEOS_NT_NR_NtQueryInformationFile, NtQueryInformationFile, FSTAT, \
      PTRS(OUT(1, sizeof(vibeos_nt_iosb_t)), NT_OUT_BUF(2, 3)), \
      nt_query_information_file(ARG(0), ARG(1), ARG(2), NT_ARG32(3), NT_ARG32(4))) \
    X(VIBEOS_NT_NR_NtSetInformationFile, NtSetInformationFile, FSTAT, \
      PTRS(OUT(1, sizeof(vibeos_nt_iosb_t)), NT_IN_BUF(2, 3)), \
      nt_set_information_file(ARG(0), ARG(1), ARG(2), NT_ARG32(3), NT_ARG32(4))) \
    X(VIBEOS_NT_NR_NtQueryAttributesFile, NtQueryAttributesFile, STAT, \
      PTRS(OUT(1, 40)), nt_query_attributes(ARG(0), ARG(1), 0)) \
    X(VIBEOS_NT_NR_NtQueryFullAttributesFile, NtQueryFullAttributesFile, STAT, \
      PTRS(OUT(1, 56)), nt_query_attributes(ARG(0), ARG(1), 1)) \
    X(VIBEOS_NT_NR_NtDeleteFile, NtDeleteFile, OPEN, NOPTR, nt_delete_file(ARG(0))) \
    X(VIBEOS_NT_NR_NtFlushBuffersFile, NtFlushBuffersFile, FSTAT, \
      PTRS(OUT(1, sizeof(vibeos_nt_iosb_t))), nt_flush_buffers_file(ARG(0), ARG(1))) \
    X(VIBEOS_NT_NR_NtCancelIoFile, NtCancelIoFile, FSTAT, \
      PTRS(OUT(1, sizeof(vibeos_nt_iosb_t))), nt_cancel_io_file(ARG(0), ARG(1))) \
    X(VIBEOS_NT_NR_NtCancelIoFileEx, NtCancelIoFileEx, FSTAT, NOPTR, nt_cancel_io_file_ex(ARG(0))) \
    X(VIBEOS_NT_NR_NtQueryVolumeInformationFile, NtQueryVolumeInformationFile, FSTAT, \
      PTRS(OUT(1, sizeof(vibeos_nt_iosb_t)), NT_OUT_BUF(2, 3)), \
      nt_query_volume_information_file(ARG(0), ARG(1), ARG(2), NT_ARG32(3), NT_ARG32(4))) \
    X(VIBEOS_NT_NR_NtQueryDirectoryFile, NtQueryDirectoryFile, GETDENTS, \
      PTRS(OUT(4, sizeof(vibeos_nt_iosb_t)), NT_OUT_BUF(5, 6), IN_OPT(9, 16)), \
      nt_query_directory_file(ARG(0), ARG(1), ARG(2), ARG(4), ARG(5), NT_ARG32(6), NT_ARG32(7), \
                              ARG(8) & 0xFFu, ARG(9), ARG(10) & 0xFFu)) \
    X(VIBEOS_NT_NR_NtLockFile, NtLockFile, FCNTL, \
      PTRS(OUT(4, sizeof(vibeos_nt_iosb_t)), IN(5, 8), IN(6, 8)), \
      nt_lock_file(ARG(0), ARG(1), ARG(2), ARG(4), ARG(5), ARG(6), ARG(9) & 0xFFu)) \
    X(VIBEOS_NT_NR_NtUnlockFile, NtUnlockFile, FCNTL, \
      PTRS(OUT(1, sizeof(vibeos_nt_iosb_t)), IN(2, 8), IN(3, 8)), \
      nt_unlock_file(ARG(0), ARG(1), ARG(2), ARG(3))) \
    X(VIBEOS_NT_NR_NtReadFile, NtReadFile, READ, \
      PTRS(OUT(4, sizeof(vibeos_nt_iosb_t)), NT_OUT_BUF(5, 6), IN_OPT(7, 8)), \
      nt_rw(0, ARG(0), ARG(1), ARG(2), ARG(4), ARG(5), NT_ARG32(6), ARG(7))) \
    X(VIBEOS_NT_NR_NtWriteFile, NtWriteFile, WRITE, \
      PTRS(OUT(4, sizeof(vibeos_nt_iosb_t)), NT_IN_BUF(5, 6), IN_OPT(7, 8)), \
      nt_rw(1, ARG(0), ARG(1), ARG(2), ARG(4), ARG(5), NT_ARG32(6), ARG(7)))

NT_DEFINE_SERVICES(file, NT_FILE_SERVICES)
