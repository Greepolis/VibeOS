/* NT personality: the file services of W1 (docs/abi/windows.md).
 *
 * NtWriteFile, synchronous and at the file's own position: what a console
 * program's output needs. The rest of the file services, and NtWriteFile's
 * offsets, events and APCs, are W4's. */

#include "nt_internal.h"

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

#define NT_FILE_SERVICES(X) \
    X(VIBEOS_NT_NR_NtWriteFile, NtWriteFile, WRITE, \
      PTRS(OUT(4, sizeof(vibeos_nt_iosb_t)), NT_IN_BUF(5, 6), IN_OPT(7, 8)), \
      nt_write_file(ARG(0), ARG(1), ARG(2), ARG(3), ARG(4), ARG(5), NT_ARG32(6), ARG(7)))

NT_DEFINE_SERVICES(file, NT_FILE_SERVICES)
