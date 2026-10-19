/* NT personality: the object manager's core (docs/abi/windows.md W2).
 *
 * An NT object is a description of one of the NT types below, and a handle to
 * it is a descriptor naming that description - (fd + 1) * 4 - with the
 * handle's access mask kept in the descriptor table's entry. So a reference is
 * vibeos_file_get, closing is the table's remove and a put, and fork copies
 * handles as it copies descriptors: the machinery seven phases of Linux work
 * have tested, not a second copy of it.
 *
 * What is the object manager's own: the state of each object, in a table here
 * under its own lock; names, which map onto a description so that opening a
 * name hands out another reference to the same object; and the access masks,
 * checked for every operation as Windows checks them.
 *
 * The namespace is flat in W2: a name is a whole path ("\BaseNamedObjects\x")
 * and nothing checks that its directory exists. Directory and symbolic-link
 * objects come with ntdll:om, which tests them. */

#include "nt_internal.h"

static nt_ob_t g_ob[NT_OB_MAX];
static vibeos_lock_t g_ob_lock;

void nt_ob_lock(void) {
    ks_lock(&g_ob_lock, __func__);
}

void nt_ob_unlock(void) {
    ks_unlock(&g_ob_lock);
}

void nt_ob_reset(void) {
    uint32_t i;
    for (i = 0; i < NT_OB_MAX; i++) {
        g_ob[i].used = 0;
    }
}

nt_ob_t *nt_ob_of(const vibeos_file_t *f) {
    if (!f || !f->ops || !f->ops->nt_type || f->nt_ob < 0 || f->nt_ob >= (int)NT_OB_MAX ||
        !g_ob[f->nt_ob].used) {
        return 0;
    }
    return &g_ob[f->nt_ob];
}

/* The last reference to an object went: its slot, and with it its name. */
void nt_ob_release(vibeos_file_t *f) {
    nt_ob_lock();
    if (f->nt_ob >= 0 && f->nt_ob < (int)NT_OB_MAX) {
        g_ob[f->nt_ob].used = 0;
        g_ob[f->nt_ob].file = 0;
    }
    f->nt_ob = -1;
    nt_ob_unlock();
}

/* ---- names ----------------------------------------------------------------------- */

static uint16_t fold(uint16_t c) {
    return (c >= 'a' && c <= 'z') ? (uint16_t)(c - 'a' + 'A') : c;
}

static int name_eq(const nt_ob_t *o, const nt_name_t *n) {
    uint32_t i;

    if (o->name_len != n->len || o->name_len == 0u) {
        return 0;
    }
    for (i = 0; i < n->len; i++) {
        uint16_t a = o->name[i], b = n->chars[i];
        /* Case is ignored when either asked for it: the existing object's
         * creator or this caller - Windows' rule is the caller's attribute,
         * and every caller through kernel32 sets it. */
        if (n->insensitive || o->name_ci) {
            a = fold(a);
            b = fold(b);
        }
        if (a != b) {
            return 0;
        }
    }
    return 1;
}

/* Under the lock. */
static int find_named(const nt_name_t *n) {
    uint32_t i;
    for (i = 0; i < NT_OB_MAX; i++) {
        if (g_ob[i].used && g_ob[i].file && name_eq(&g_ob[i], n)) {
            return (int)i;
        }
    }
    return -1;
}

uint32_t nt_read_attributes(uint64_t oa, nt_name_t *out, uint32_t *attributes) {
    uint8_t blk[VIBEOS_OA_SIZE];
    uint8_t us[VIBEOS_USTR_SIZE];
    uint64_t root, uname, buf;
    uint32_t len, i;
    uint16_t length;

    out->len = 0;
    out->insensitive = 0;
    *attributes = 0;
    if (oa == 0u) {
        return VIBEOS_STATUS_SUCCESS;   /* no attributes: an unnamed object */
    }
    if (!nt_user_ok(oa, sizeof(blk), 0) ||
        vibeos_uaccess_copy(blk, (const void *)(uintptr_t)oa, sizeof(blk)) != 0) {
        return VIBEOS_STATUS_ACCESS_VIOLATION;
    }
    for (len = 0, i = 0; i < 4u; i++) {
        len |= (uint32_t)blk[VIBEOS_OA_LENGTH + i] << (8u * i);
    }
    for (root = 0, uname = 0, i = 0; i < 8u; i++) {
        root |= (uint64_t)blk[VIBEOS_OA_ROOT_DIRECTORY + i] << (8u * i);
        uname |= (uint64_t)blk[VIBEOS_OA_OBJECT_NAME + i] << (8u * i);
    }
    for (i = 0; i < 4u; i++) {
        *attributes |= (uint32_t)blk[VIBEOS_OA_ATTRIBUTES + i] << (8u * i);
    }
    if (len != VIBEOS_OA_SIZE || (*attributes & ~VIBEOS_OBJ_VALID_ATTRIBUTES)) {
        return VIBEOS_STATUS_INVALID_PARAMETER;
    }
    out->insensitive = (*attributes & VIBEOS_OBJ_CASE_INSENSITIVE) ? 1 : 0;
    if (uname == 0u) {
        return VIBEOS_STATUS_SUCCESS;
    }
    if (!nt_user_ok(uname, sizeof(us), 0) ||
        vibeos_uaccess_copy(us, (const void *)(uintptr_t)uname, sizeof(us)) != 0) {
        return VIBEOS_STATUS_ACCESS_VIOLATION;
    }
    length = (uint16_t)(us[VIBEOS_USTR_LENGTH] | (us[VIBEOS_USTR_LENGTH + 1] << 8));
    for (buf = 0, i = 0; i < 8u; i++) {
        buf |= (uint64_t)us[VIBEOS_USTR_BUFFER + i] << (8u * i);
    }
    if ((length & 1u) != 0u) {
        return VIBEOS_STATUS_OBJECT_NAME_INVALID;
    }
    if (length == 0u) {
        return VIBEOS_STATUS_SUCCESS;   /* an empty name is no name */
    }
    if (length / 2u > NT_NAME_MAX) {
        return VIBEOS_STATUS_OBJECT_NAME_INVALID;
    }
    if (!nt_user_ok(buf, length, 0) ||
        vibeos_uaccess_copy(out->chars, (const void *)(uintptr_t)buf, length) != 0) {
        return VIBEOS_STATUS_ACCESS_VIOLATION;
    }
    out->len = length / 2u;
    /* A directory to look in is the namespace's next step; without one, a
     * name is a path and must start at the root. */
    if (root != 0u) {
        return VIBEOS_STATUS_OBJECT_PATH_NOT_FOUND;
    }
    if (out->chars[0] != '\\') {
        return VIBEOS_STATUS_OBJECT_PATH_SYNTAX_BAD;
    }
    return VIBEOS_STATUS_SUCCESS;
}

/* ---- creating and opening ---------------------------------------------------------- */

uint32_t nt_ob_create(const vibeos_file_ops_t *ops, const nt_name_t *name, uint32_t attributes,
                      vibeos_file_t **out, int *existed,
                      void (*init)(nt_ob_t *o, const void *ctx), const void *ctx) {
    vibeos_file_t *f;
    int slot = -1, named, me = ks_current();
    uint32_t i, creator = me >= 0 ? ks_id(me)->tgid : 0u;

    *out = 0;
    *existed = 0;
    nt_ob_lock();
    if (name && name->len) {
        named = find_named(name);
        if (named >= 0) {
            vibeos_file_t *have = g_ob[named].file;

            /* An existing name: the same object, for a create that would
             * have made one of its type - OBJ_OPENIF or not, Windows opens it
             * and says the name existed - and a type mismatch for any other. */
            if (have->ops != ops) {
                nt_ob_unlock();
                return VIBEOS_STATUS_OBJECT_TYPE_MISMATCH;
            }
            if (!vibeos_file_try_get(have)) {
                nt_ob_unlock();
                return VIBEOS_STATUS_OBJECT_NAME_NOT_FOUND;   /* going as we look */
            }
            nt_ob_unlock();
            *out = have;
            *existed = 1;
            return VIBEOS_STATUS_OBJECT_NAME_EXISTS;
        }
    }
    {
        uint32_t mine = 0;

        for (i = 0; i < NT_OB_MAX; i++) {
            if (!g_ob[i].used && slot < 0) {
                slot = (int)i;
            }
            mine += g_ob[i].used && g_ob[i].owner == creator;
        }
        if (creator != 0u && mine >= NT_OB_PER_PROCESS) {
            nt_ob_unlock();
            return VIBEOS_STATUS_QUOTA_EXCEEDED;   /* its share (M-110) */
        }
    }
    if (slot < 0) {
        nt_ob_unlock();
        return VIBEOS_STATUS_INSUFFICIENT_RESOURCES;
    }
    f = vibeos_file_alloc(ops, 0);
    if (!f) {
        nt_ob_unlock();
        return VIBEOS_STATUS_INSUFFICIENT_RESOURCES;
    }
    {
        nt_ob_t *o = &g_ob[slot];
        uint8_t *raw = (uint8_t *)o;
        for (i = 0; i < sizeof(*o); i++) {
            raw[i] = 0;   /* the slot's last tenant leaves nothing behind */
        }
        o->used = 1;
        o->owner = creator;
        o->file = f;
        o->name_ci = (attributes & VIBEOS_OBJ_CASE_INSENSITIVE) ? 1u : 0u;
        if (name) {
            for (i = 0; i < name->len; i++) {
                o->name[i] = name->chars[i];
            }
            o->name_len = name->len;
        }
        /* Its state before the lock goes, which is when the name is there for
         * anybody to open: a mutant created owned was, for a moment, a free
         * one another process could take (M-115). */
        if (init) {
            init(o, ctx);
        }
    }
    f->nt_ob = slot;
    nt_ob_unlock();
    *out = f;
    return VIBEOS_STATUS_SUCCESS;
}

uint32_t nt_ob_open(const vibeos_file_ops_t *ops, const nt_name_t *name, vibeos_file_t **out) {
    int named;
    vibeos_file_t *have;

    *out = 0;
    if (!name || name->len == 0u) {
        return VIBEOS_STATUS_OBJECT_PATH_SYNTAX_BAD;
    }
    nt_ob_lock();
    named = find_named(name);
    if (named < 0) {
        nt_ob_unlock();
        return VIBEOS_STATUS_OBJECT_NAME_NOT_FOUND;
    }
    have = g_ob[named].file;
    if (have->ops != ops) {
        nt_ob_unlock();
        return VIBEOS_STATUS_OBJECT_TYPE_MISMATCH;
    }
    if (!vibeos_file_try_get(have)) {
        nt_ob_unlock();
        return VIBEOS_STATUS_OBJECT_NAME_NOT_FOUND;
    }
    nt_ob_unlock();
    *out = have;
    return VIBEOS_STATUS_SUCCESS;
}

/* ---- access ------------------------------------------------------------------------ */

uint32_t nt_map_access(const vibeos_file_ops_t *ops, uint32_t want) {
    return nt_map_access_with(ops ? ops->nt_type : 0, want);
}

uint32_t nt_map_access_with(const nt_type_t *t, uint32_t want) {
    uint32_t all = t ? t->all_access : VIBEOS_STANDARD_RIGHTS_REQUIRED | VIBEOS_SYNCHRONIZE;
    uint32_t got = want & ~(VIBEOS_GENERIC_READ | VIBEOS_GENERIC_WRITE | VIBEOS_GENERIC_EXECUTE |
                            VIBEOS_GENERIC_ALL | VIBEOS_MAXIMUM_ALLOWED);

    if (want & VIBEOS_GENERIC_READ) {
        got |= t ? t->generic_read : 0u;
    }
    if (want & VIBEOS_GENERIC_WRITE) {
        got |= t ? t->generic_write : 0u;
    }
    if (want & VIBEOS_GENERIC_EXECUTE) {
        got |= t ? t->generic_execute : 0u;
    }
    if (want & (VIBEOS_GENERIC_ALL | VIBEOS_MAXIMUM_ALLOWED)) {
        /* Nothing here limits what a process may have of an object it can
         * name: security descriptors are the security plan's (SEC). */
        got |= all;
    }
    return got & all;
}

/* ---- handles ------------------------------------------------------------------------ */

uint32_t nt_handle_install(vibeos_file_t *f, uint32_t access, uint32_t attributes,
                           uint64_t *handle) {
    vibeos_procstate_t *ps;
    int me = ks_current();
    int fd;

    *handle = 0;
    ps = me >= 0 ? ks_ps(me) : 0;
    if (!ps) {
        vibeos_file_put(f);
        return VIBEOS_STATUS_INVALID_HANDLE;
    }
    ks_lock(&ps->files_lock, __func__);
    /* Not inherited unless asked: an NT handle crosses into a child only with
     * OBJ_INHERIT, where a Linux descriptor crosses unless it is marked. */
    fd = vibeos_fdtable_install(&ps->files, f,
                                (attributes & VIBEOS_OBJ_INHERIT) ? 0u : VIBEOS_FD_CLOEXEC, 0);
    if (fd >= 0) {
        (void)vibeos_fdtable_set_access(&ps->files, (uint64_t)fd, access);
    }
    ks_unlock(&ps->files_lock);
    if (fd < 0) {
        vibeos_file_put(f);
        return VIBEOS_STATUS_INSUFFICIENT_RESOURCES;
    }
    *handle = nt_handle_of_fd((uint32_t)fd);
    return VIBEOS_STATUS_SUCCESS;
}

static int fd_of_handle(uint64_t h, uint32_t *fd) {
    if (h == 0u || (h & 3u) != 0u || h / 4u > (uint64_t)VIBEOS_FD_MAX) {
        return 0;
    }
    *fd = (uint32_t)(h / 4u - 1u);
    return 1;
}

uint32_t nt_handle_ref(uint64_t h, const vibeos_file_ops_t *ops, uint32_t need,
                       vibeos_file_t **out, uint32_t *granted) {
    vibeos_procstate_t *ps;
    vibeos_file_t *f;
    uint32_t fd, access;
    int me = ks_current();

    *out = 0;
    if (!fd_of_handle(h, &fd) || me < 0 || (ps = ks_ps(me)) == 0) {
        return VIBEOS_STATUS_INVALID_HANDLE;
    }
    ks_lock(&ps->files_lock, __func__);
    f = vibeos_fdtable_get(&ps->files, fd);
    access = vibeos_fdtable_access(&ps->files, fd);
    vibeos_file_get(f);
    ks_unlock(&ps->files_lock);
    if (!f) {
        return VIBEOS_STATUS_INVALID_HANDLE;
    }
    if (ops && f->ops != ops) {
        vibeos_file_put(f);
        return VIBEOS_STATUS_OBJECT_TYPE_MISMATCH;
    }
    /* A descriptor the Linux side installed - a Windows program's standard
     * handles, inherited from its shell - carries no mask: its rights are the
     * ones its open mode gives, FILE_GENERIC_READ and FILE_GENERIC_WRITE. */
    if (access == 0u && !f->ops->nt_type) {
        uint32_t mode = f->flags & 3u;   /* O_RDONLY 0, O_WRONLY 1, O_RDWR 2 */
        access = (mode != 1u ? 0x00120089u : 0u) | (mode != 0u ? 0x00120116u : 0u);
    }
    if ((access & need) != need) {
        vibeos_file_put(f);
        return VIBEOS_STATUS_ACCESS_DENIED;
    }
    if (granted) {
        *granted = access;
    }
    *out = f;
    return VIBEOS_STATUS_SUCCESS;
}

/* NtClose(Handle). The pseudo-handles name the caller and close nothing. */
uint32_t nt_close_handle(uint64_t h) {
    vibeos_procstate_t *ps;
    vibeos_file_t *f;
    uint32_t fd;
    int me = ks_current();

    if (h == VIBEOS_NT_CURRENT_PROCESS || h == VIBEOS_NT_CURRENT_THREAD) {
        return VIBEOS_STATUS_SUCCESS;
    }
    if (!fd_of_handle(h, &fd) || me < 0 || (ps = ks_ps(me)) == 0) {
        return VIBEOS_STATUS_INVALID_HANDLE;
    }
    ks_lock(&ps->files_lock, __func__);
    f = vibeos_fdtable_remove(&ps->files, fd);
    ks_unlock(&ps->files_lock);
    if (!f) {
        return VIBEOS_STATUS_INVALID_HANDLE;
    }
    vibeos_file_put(f);   /* after the lock: the last reference may do work */
    return VIBEOS_STATUS_SUCCESS;
}

static long nt_close(uint64_t h) {
    return NT_STATUS(nt_close_handle(h));
}

/* NtDuplicateObject(SourceProcess, SourceHandle, TargetProcess, TargetHandle,
 * DesiredAccess, HandleAttributes, Options), within the calling process: the
 * other processes a handle can be duplicated into are W6's. */
static long nt_duplicate_object(uint64_t src_proc, uint64_t src, uint64_t dst_proc,
                                uint64_t dst_u, uint64_t want, uint64_t attributes,
                                uint64_t options) {
    vibeos_file_t *f;
    uint32_t granted = 0, st;
    uint64_t h = 0;

    if (src_proc != VIBEOS_NT_CURRENT_PROCESS ||
        (dst_proc != VIBEOS_NT_CURRENT_PROCESS && dst_u != 0u)) {
        return NT_STATUS(VIBEOS_STATUS_INVALID_HANDLE);
    }
    st = nt_handle_ref(src, 0, 0, &f, &granted);
    if (st != VIBEOS_STATUS_SUCCESS) {
        return NT_STATUS(st);
    }
    if (dst_u != 0u) {
        uint32_t access = (options & VIBEOS_DUPLICATE_SAME_ACCESS)
                              ? granted : nt_map_access(f->ops, (uint32_t)want);

        vibeos_file_get(f);   /* the new handle's own */
        st = nt_handle_install(f, access, (uint32_t)attributes, &h);
        if (st == VIBEOS_STATUS_SUCCESS &&
            vibeos_uaccess_copy((void *)(uintptr_t)dst_u, &h, sizeof(h)) != 0) {
            (void)nt_close_handle(h);
            st = VIBEOS_STATUS_ACCESS_VIOLATION;
        }
    } else {
        st = VIBEOS_STATUS_SUCCESS;   /* nowhere to put it: only the close below */
    }
    vibeos_file_put(f);
    if (st == VIBEOS_STATUS_SUCCESS && (options & VIBEOS_DUPLICATE_CLOSE_SOURCE)) {
        (void)nt_close_handle(src);
    }
    return NT_STATUS(st);
}

#define NT_OBJECT_SERVICES(X) \
    X(VIBEOS_NT_NR_NtClose, NtClose, CLOSE, NOPTR, nt_close(ARG(0))) \
    X(VIBEOS_NT_NR_NtDuplicateObject, NtDuplicateObject, DUP, \
      PTRS(OUT_OPT(3, 8)), \
      nt_duplicate_object(ARG(0), ARG(1), ARG(2), ARG(3), NT_ARG32(4), NT_ARG32(5), NT_ARG32(6)))

NT_DEFINE_SERVICES(object, NT_OBJECT_SERVICES)
