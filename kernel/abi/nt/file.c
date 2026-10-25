/* NT personality: the file services of W1 (docs/abi/windows.md).
 *
 * NtWriteFile, synchronous and at the file's own position: what a console
 * program's output needs. The rest of the file services, and NtWriteFile's
 * offsets, events and APCs, are W4's. */

#include "nt_internal.h"
#include "vibeos/path.h"
#include "vibeos/fileops.h"
#include "vibeos/cred.h"

vibeos_file_t *nt_file_get(uint64_t handle) {
    vibeos_procstate_t *ps;
    vibeos_file_t *f;
    int me = ks_current();

    /* Multiples of four from four; anything else names nothing. */
    if (me < 0 || handle == 0u || (handle & 3u) != 0u || handle / 4u > (uint64_t)VIBEOS_FD_MAX) {
        return 0;
    }
    ps = ks_ps(me);
    if (!ps) {
        return 0;
    }
    ks_lock(&ps->files_lock, __func__);
    f = vibeos_fdtable_get(&ps->files, (uint32_t)(handle / 4u - 1u));
    vibeos_file_get(f);
    ks_unlock(&ps->files_lock);
    return f;
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
static long nt_rw(int write, uint64_t file, uint64_t event, uint64_t apc, uint64_t iosb_u,
                  uint64_t buf, uint64_t len, uint64_t offp) {
    vibeos_file_t *f;
    vibeos_nt_iosb_t iosb;
    uint64_t off = NT_OFF_CURRENT;
    long n;
    uint32_t st;

    if (apc != 0u) {
        return NT_STATUS(VIBEOS_STATUS_NOT_IMPLEMENTED);
    }
    if (offp != 0u && vibeos_uaccess_copy(&off, (const void *)(uintptr_t)offp, sizeof(off)) != 0) {
        return NT_STATUS(VIBEOS_STATUS_ACCESS_VIOLATION);
    }
    f = nt_file_get(file);
    if (!f) {
        return NT_STATUS(VIBEOS_STATUS_INVALID_HANDLE);
    }
    if (event != 0u && (st = nt_io_event(event, 0)) != VIBEOS_STATUS_SUCCESS) {
        vibeos_file_put(f);
        return NT_STATUS(st);
    }
    if (off == NT_OFF_CURRENT || !(write ? f->ops->pwrite : f->ops->pread)) {
        /* `len` is the row's NT_ARG32: 32 bits already, decided in one place. */
        if (off != NT_OFF_CURRENT && off != NT_OFF_END) {
            n = -VIBEOS_EINVAL;   /* a console or a pipe has no offsets */
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
    st = n < 0 ? nt_status_of_errno(n)
       : (!write && n == 0 && len != 0u) ? VIBEOS_STATUS_END_OF_FILE : VIBEOS_STATUS_SUCCESS;
    iosb.status = st;
    iosb.information = n < 0 ? 0u : (uint64_t)n;
    if (vibeos_uaccess_copy((void *)(uintptr_t)iosb_u, &iosb, sizeof(iosb)) != 0) {
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
 * Separators become '/', and the name is passed to the VFS as it is - FAT, where
 * the DLLs are, compares names without case, as Windows does; a case-sensitive
 * filesystem under C: is W4's question. */
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
        uint16_t c = n->chars[i];

        if (c == 0u || c >= 0x80u) {
            /* The VFS takes bytes, and W2's names are ASCII; UTF-8 for the rest
             * is W4's, with the case rules. */
            return VIBEOS_STATUS_OBJECT_NAME_INVALID;
        }
        if (o + 2u > cap) {
            return VIBEOS_STATUS_OBJECT_NAME_INVALID;
        }
        out[o++] = c == '\\' ? '/' : (char)c;
    }
    out[o] = 0;
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
                             uint32_t disposition, uint32_t options) {
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

    if (disposition > NT_FILE_OVERWRITE_IF ||
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
    f = vibeos_open_path(&w, flags, 0644u, &err);
    if (!f) {
        return NT_STATUS(nt_status_of_open(err));
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
    if (vibeos_uaccess_copy((void *)(uintptr_t)out, &h, sizeof(h)) != 0 ||
        vibeos_uaccess_copy((void *)(uintptr_t)iosb_u, &iosb, sizeof(iosb)) != 0) {
        (void)nt_close_handle(h);
        return NT_STATUS(VIBEOS_STATUS_ACCESS_VIOLATION);
    }
    return NT_STATUS(VIBEOS_STATUS_SUCCESS);
}

/* NtOpenFile(FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock,
 * ShareAccess, OpenOptions): NtCreateFile's FILE_OPEN. Share modes are W4
 * step 7's. */
static long nt_open_file(uint64_t out, uint64_t want, uint64_t oa, uint64_t iosb_u,
                         uint64_t share, uint64_t options) {
    (void)share;
    return nt_file_open_or_create(out, (uint32_t)want, oa, iosb_u, NT_FILE_OPEN, (uint32_t)options);
}

/* NtCreateFile(FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock,
 * AllocationSize, FileAttributes, ShareAccess, CreateDisposition,
 * CreateOptions, EaBuffer, EaLength). The allocation size is a hint and the
 * attributes ride on FAT's directory entry, which step 3 sets; extended
 * attributes are refused rather than dropped. */
static long nt_create_file(uint64_t out, uint64_t want, uint64_t oa, uint64_t iosb_u,
                           uint64_t disposition, uint64_t options, uint64_t ea, uint64_t ea_len) {
    if (ea != 0u && ea_len != 0u) {
        return NT_STATUS(VIBEOS_STATUS_NOT_IMPLEMENTED);
    }
    return nt_file_open_or_create(out, (uint32_t)want, oa, iosb_u, (uint32_t)disposition, (uint32_t)options);
}

#define NT_FILE_SERVICES(X) \
    X(VIBEOS_NT_NR_NtOpenFile, NtOpenFile, OPEN, \
      PTRS(OUT(0, 8), OUT(3, sizeof(vibeos_nt_iosb_t))), \
      nt_open_file(ARG(0), NT_ARG32(1), ARG(2), ARG(3), NT_ARG32(4), NT_ARG32(5))) \
    X(VIBEOS_NT_NR_NtCreateFile, NtCreateFile, OPEN, \
      PTRS(OUT(0, 8), OUT(3, sizeof(vibeos_nt_iosb_t))), \
      nt_create_file(ARG(0), NT_ARG32(1), ARG(2), ARG(3), NT_ARG32(7), NT_ARG32(8), ARG(9), NT_ARG32(10))) \
    X(VIBEOS_NT_NR_NtReadFile, NtReadFile, READ, \
      PTRS(OUT(4, sizeof(vibeos_nt_iosb_t)), NT_OUT_BUF(5, 6), IN_OPT(7, 8)), \
      nt_rw(0, ARG(0), ARG(1), ARG(2), ARG(4), ARG(5), NT_ARG32(6), ARG(7))) \
    X(VIBEOS_NT_NR_NtWriteFile, NtWriteFile, WRITE, \
      PTRS(OUT(4, sizeof(vibeos_nt_iosb_t)), NT_IN_BUF(5, 6), IN_OPT(7, 8)), \
      nt_rw(1, ARG(0), ARG(1), ARG(2), ARG(4), ARG(5), NT_ARG32(6), ARG(7)))

NT_DEFINE_SERVICES(file, NT_FILE_SERVICES)
