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

/* NtWriteFile(File, Event, ApcRoutine, ApcContext, IoStatusBlock, Buffer,
 * Length, ByteOffset, Key). The bytes go through the description's own write,
 * the same one Linux's write() reaches, so a console, a pipe or a file behaves
 * here as it does there. The status block is written last, as Windows writes
 * it on completion: STATUS_SUCCESS and the count, or the failure and zero.
 *
 * An event, an APC or an offset asks for what W4 provides - asynchronous
 * completion and positioned writes - and is answered STATUS_NOT_IMPLEMENTED
 * rather than ignored: a program that asked for its write at offset 4096 and
 * got it at the file's position would have had a different file written. */
static long nt_write_file(uint64_t file, uint64_t event, uint64_t apc, uint64_t apc_ctx,
                          uint64_t iosb_u, uint64_t buf, uint64_t len, uint64_t offset) {
    vibeos_file_t *f;
    vibeos_nt_iosb_t iosb;
    long n;
    uint32_t st;

    (void)apc_ctx;
    if (event != 0u || apc != 0u || offset != 0u) {
        return NT_STATUS(VIBEOS_STATUS_NOT_IMPLEMENTED);
    }
    f = nt_file_get(file);
    if (!f) {
        return NT_STATUS(VIBEOS_STATUS_INVALID_HANDLE);
    }
    /* `len` is the row's NT_ARG32: 32 bits already, decided in one place. */
    n = f->ops->write ? f->ops->write(f, buf, len) : -VIBEOS_EBADF;
    vibeos_file_put(f);
    st = n < 0 ? nt_status_of_errno(n) : VIBEOS_STATUS_SUCCESS;
    iosb.status = st;
    iosb.information = n < 0 ? 0u : (uint64_t)n;
    if (vibeos_uaccess_copy((void *)(uintptr_t)iosb_u, &iosb, sizeof(iosb)) != 0) {
        return NT_STATUS(VIBEOS_STATUS_ACCESS_VIOLATION);
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

/* NtOpenFile(FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock,
 * ShareAccess, OpenOptions): an existing file, to read - which is what loading
 * a DLL needs. Asking to write is W4's, and is refused as not implemented
 * rather than opened read-only behind the caller's back. */
static long nt_open_file(uint64_t out, uint64_t want, uint64_t oa, uint64_t iosb_u,
                         uint64_t share, uint64_t options) {
    nt_name_t name;
    uint32_t attributes = 0, st, access;
    char path[VIBEOS_FILE_PATH];
    vibeos_path_t w;
    vibeos_cred_t who;
    vibeos_file_t *f;
    vibeos_nt_iosb_t iosb;
    long err = 0;
    int rc;
    uint64_t h;

    (void)share;
    (void)options;
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
    access = (uint32_t)want;
    if (access & VIBEOS_GENERIC_ALL) {
        access |= NT_FILE_ALL_ACCESS;
    }
    if (access & (VIBEOS_GENERIC_WRITE | NT_FILE_WRITE_DATA | NT_FILE_APPEND_DATA |
                  NT_FILE_WRITE_EA | NT_FILE_WRITE_ATTRIBUTES | VIBEOS_DELETE)) {
        return NT_STATUS(VIBEOS_STATUS_NOT_IMPLEMENTED);
    }
    /* Walked and opened as the caller, as openat is: search permission on
     * every directory, read permission on the file, and execute permission
     * when FILE_EXECUTE is asked - the loader asks it. Walked by the kernel
     * for itself it was a way to learn whether any path existed, and to map a
     * program its owner had kept to themselves (M-105). */
    nt_cred(&who);
    rc = vibeos_path_walk_as("/", "/", path, vibeos_open_walk_flags(VIBEOS_O_RDONLY), &who, &w);
    if (rc != 0) {
        return NT_STATUS(nt_status_of_open(rc));
    }
    if (vibeos_cred_may(&who, 0, w.node.mode, w.node.uid, w.node.gid,
                        VIBEOS_MAY_READ | (((uint32_t)want & NT_FILE_EXECUTE) ? VIBEOS_MAY_EXEC : 0u)) != 0) {
        return NT_STATUS(VIBEOS_STATUS_ACCESS_DENIED);
    }
    f = vibeos_open_path(&w, VIBEOS_O_RDONLY, 0, &err);
    if (!f) {
        return NT_STATUS(nt_status_of_open(err));
    }
    /* A file's generic rights are mapped here, not by the object manager: a
     * file is the VFS's description, not one of its types. */
    access = nt_map_access_with(&g_file_type, (uint32_t)want);
    st = nt_handle_install(f, access, attributes, &h);
    if (st != VIBEOS_STATUS_SUCCESS) {
        return NT_STATUS(st);
    }
    iosb.status = VIBEOS_STATUS_SUCCESS;
    iosb.information = NT_FILE_OPENED;
    if (vibeos_uaccess_copy((void *)(uintptr_t)out, &h, sizeof(h)) != 0 ||
        vibeos_uaccess_copy((void *)(uintptr_t)iosb_u, &iosb, sizeof(iosb)) != 0) {
        (void)nt_close_handle(h);
        return NT_STATUS(VIBEOS_STATUS_ACCESS_VIOLATION);
    }
    return NT_STATUS(VIBEOS_STATUS_SUCCESS);
}

#define NT_FILE_SERVICES(X) \
    X(VIBEOS_NT_NR_NtOpenFile, NtOpenFile, OPEN, \
      PTRS(OUT(0, 8), OUT(3, sizeof(vibeos_nt_iosb_t))), \
      nt_open_file(ARG(0), NT_ARG32(1), ARG(2), ARG(3), NT_ARG32(4), NT_ARG32(5))) \
    X(VIBEOS_NT_NR_NtWriteFile, NtWriteFile, WRITE, \
      PTRS(OUT(4, sizeof(vibeos_nt_iosb_t)), NT_IN_BUF(5, 6), IN_OPT(7, 8)), \
      nt_write_file(ARG(0), ARG(1), ARG(2), ARG(3), ARG(4), ARG(5), NT_ARG32(6), ARG(7)))

NT_DEFINE_SERVICES(file, NT_FILE_SERVICES)
