/* NT personality: sections (docs/abi/windows.md W2, W3 step 5).
 *
 * How a Windows loader brings a DLL in: NtOpenFile the file, NtCreateSection
 * with SEC_IMAGE over it, NtMapViewOfSection into the process. The mapping is
 * the kernel's, placing and relocation included, and it is the same placing and
 * relocation exec gives a program (kernel/arch/x86_64/pe_load.c).
 *
 * A data section (W3) is memory that views of it share. One of memory alone -
 * no file, "backed by the paging file" - is a file on /tmp that nothing names,
 * as a SysV segment is, and its views map that file's own pages, so a store
 * through one view is in every other. One of a file maps the file's bytes:
 * NtOpenFile opens for reading alone until W4, so such a view is the process's
 * own copy, read-only or copy-on-write - the two a read-only file allows - and
 * a later change to the file does not show through it, which is the gap.
 *
 * A view is a region of an allocation based at the view, so
 * NtQueryVirtualMemory reports it, NtProtectVirtualMemory changes it, and
 * NtUnmapViewOfSection finds all of it from its base. */

#include "nt_internal.h"
#include "vibeos/umem.h"

#define NT_SEC_FILE                  0x00800000u
#define NT_SEC_IMAGE                 0x01000000u
#define NT_SEC_RESERVE               0x04000000u
#define NT_SEC_COMMIT                0x08000000u
#define NT_SEC_NOCACHE               0x10000000u
#define NT_SECTION_QUERY             0x0001u
#define NT_SECTION_MAP_WRITE         0x0002u
#define NT_SECTION_MAP_READ          0x0004u
#define NT_SECTION_MAP_EXECUTE       0x0008u
#define NT_SECTION_EXTEND_SIZE       0x0010u
#define NT_SECTION_ALL_ACCESS        0x000F001Fu
#define NT_STATUS_IMAGE_NOT_AT_BASE  0x40000003u
#define NT_STATUS_INVALID_IMAGE_FORMAT VIBEOS_STATUS_INVALID_IMAGE_FORMAT
#define NT_FILE_READ_DATA            0x0001u
#define NT_FILE_WRITE_DATA           0x0002u

/* A section holds the file it was made from - a reference, so the file stays
 * while any handle to the section does - and goes with its last handle. */
static void section_release(vibeos_file_t *f) {
    nt_ob_t *o;
    vibeos_file_t *file = 0;

    nt_ob_lock();
    o = nt_ob_of(f);
    if (o) {
        file = o->u.sec.file;
        o->u.sec.file = 0;
    }
    nt_ob_unlock();
    if (file) {
        vibeos_file_put(file);   /* outside the lock: a last put may do work */
    }
    nt_ob_release(f);
}

static const nt_type_t g_section_type = {
    "Section",
    NT_SECTION_ALL_ACCESS,
    VIBEOS_READ_CONTROL | NT_SECTION_QUERY | NT_SECTION_MAP_READ,
    VIBEOS_READ_CONTROL | NT_SECTION_MAP_WRITE,
    VIBEOS_READ_CONTROL | NT_SECTION_MAP_EXECUTE,
    0,
    0,
};

const vibeos_file_ops_t g_nt_section_ops = {
    .name = "nt-section",
    .release = section_release,
    .nt_type = &g_section_type,
};

/* What a new section is made of. The section takes a reference of its own to
 * the file: whatever happens to the create after this, the caller's reference
 * is the caller's to give back. */
typedef struct {
    vibeos_file_t *file;
    uint64_t size;
    uint32_t data;        /* a data section, not an image             */
    uint32_t shared;      /* its views map the file's own pages       */
    uint32_t protect;     /* the section's page protection            */
    uint32_t attributes;  /* SEC_*, as NtQuerySection reports them    */
} section_ctx_t;

static void section_init(nt_ob_t *o, const void *ctx) {
    const section_ctx_t *c = (const section_ctx_t *)ctx;
    o->u.sec.file = c->file;
    vibeos_file_get(o->u.sec.file);
    o->u.sec.image_size = c->size;
    o->u.sec.data = c->data;
    o->u.sec.shared = c->shared;
    o->u.sec.protect = c->protect;
    o->u.sec.attributes = c->attributes;
}

/* What a view's protection may be, given the section's: never more than the
 * section allows - no store into a read-only section, no execution of one that
 * was not made executable - and copy-on-write only where a store could have
 * been allowed at all. And the handle's rights it needs. 0, or a status. */
static uint32_t view_allowed(uint32_t section, uint32_t view, uint32_t *need) {
    const uint32_t s = section & 0xFFu, v = view & 0xFFu;
    const int s_exec = (s & (VIBEOS_NT_PAGE_EXECUTE | VIBEOS_NT_PAGE_EXECUTE_READ |
                             VIBEOS_NT_PAGE_EXECUTE_READWRITE | VIBEOS_NT_PAGE_EXECUTE_WRITECOPY)) != 0u;
    const int s_write = (s & (VIBEOS_NT_PAGE_READWRITE | VIBEOS_NT_PAGE_EXECUTE_READWRITE)) != 0u;
    const int s_copy = s_write || (s & (VIBEOS_NT_PAGE_WRITECOPY | VIBEOS_NT_PAGE_EXECUTE_WRITECOPY)) != 0u;

    *need = NT_SECTION_MAP_READ;
    switch (v) {
    case VIBEOS_NT_PAGE_NOACCESS:
    case VIBEOS_NT_PAGE_READONLY:
        return 0;
    case VIBEOS_NT_PAGE_READWRITE:
        *need |= NT_SECTION_MAP_WRITE;
        return s_write ? 0u : VIBEOS_STATUS_SECTION_PROTECTION;
    case VIBEOS_NT_PAGE_WRITECOPY:
        return s_copy ? 0u : VIBEOS_STATUS_SECTION_PROTECTION;
    case VIBEOS_NT_PAGE_EXECUTE:
    case VIBEOS_NT_PAGE_EXECUTE_READ:
        *need |= NT_SECTION_MAP_EXECUTE;
        return s_exec ? 0u : VIBEOS_STATUS_SECTION_PROTECTION;
    case VIBEOS_NT_PAGE_EXECUTE_READWRITE:
        *need |= NT_SECTION_MAP_EXECUTE | NT_SECTION_MAP_WRITE;
        return s_exec && s_write ? 0u : VIBEOS_STATUS_SECTION_PROTECTION;
    case VIBEOS_NT_PAGE_EXECUTE_WRITECOPY:
        *need |= NT_SECTION_MAP_EXECUTE;
        return s_exec && s_copy ? 0u : VIBEOS_STATUS_SECTION_PROTECTION;
    default:
        return VIBEOS_STATUS_INVALID_PAGE_PROTECTION;
    }
}

/* A section's page protection: one base value, no modifiers but caching ones,
 * and not "no access" - a section nobody may read is nothing to map. */
static int section_prot_ok(uint32_t p) {
    const uint32_t base = p & 0xFFu;

    return base != 0u && (base & (base - 1u)) == 0u && base != VIBEOS_NT_PAGE_NOACCESS &&
           (p & ~(0xFFu | VIBEOS_NT_PAGE_NOCACHE | VIBEOS_NT_PAGE_WRITECOMBINE)) == 0u;
}

/* NtCreateSection for a data section: of memory alone, of the size asked for;
 * or of a file, of the file's size or the size asked for, which a read-only
 * file cannot be grown to. */
static long create_data_section(uint64_t out, uint32_t want, uint64_t oa, uint64_t max_size_u,
                                uint32_t protect, uint32_t attributes, uint64_t file_h) {
    section_ctx_t ctx;
    vibeos_file_stat_t st;
    uint64_t size = 0;
    uint32_t s, granted = 0;
    long r, err = 0;

    if (!section_prot_ok(protect)) {
        return NT_STATUS(VIBEOS_STATUS_INVALID_PAGE_PROTECTION);
    }
    if (max_size_u != 0u &&
        vibeos_uaccess_copy(&size, (const void *)(uintptr_t)max_size_u, sizeof(size)) != 0) {
        return NT_STATUS(VIBEOS_STATUS_ACCESS_VIOLATION);
    }
    if ((int64_t)size < 0) {
        return NT_STATUS(VIBEOS_STATUS_INVALID_PARAMETER);
    }
    ctx.data = 1u;
    ctx.protect = protect;
    ctx.attributes = (attributes & NT_SEC_RESERVE) ? NT_SEC_RESERVE : NT_SEC_COMMIT;
    if (file_h == 0u) {
        /* Memory alone: a size is required, and the file it lives in is as
         * long as the size in whole pages - the pages its views map. */
        if (size == 0u) {
            return NT_STATUS(VIBEOS_STATUS_INVALID_PARAMETER_4);
        }
        if (size > (1ull << 40)) {
            return NT_STATUS(VIBEOS_STATUS_SECTION_TOO_BIG);
        }
        ctx.file = vibeos_umem_hidden_file("/tmp/.ntsec-", &err);
        if (!ctx.file) {
            return NT_STATUS(nt_status_of_errno(err));
        }
        if (!ctx.file->ops->truncate ||
            ctx.file->ops->truncate(ctx.file, (size + 0xFFFull) & ~0xFFFull) != 0) {
            vibeos_file_put(ctx.file);
            return NT_STATUS(VIBEOS_STATUS_NO_MEMORY);
        }
        ctx.shared = 1u;
    } else {
        s = nt_handle_ref(file_h, 0, 0, &ctx.file, &granted);
        if (s != VIBEOS_STATUS_SUCCESS) {
            return NT_STATUS(s);
        }
        if (ctx.file->ops->nt_type || ctx.file->path[0] != '/') {
            vibeos_file_put(ctx.file);
            return NT_STATUS(VIBEOS_STATUS_OBJECT_TYPE_MISMATCH);   /* not a file */
        }
        /* The handle's rights bound the section's: a file it may not write
         * cannot be the store behind a writable section. */
        if (!(granted & NT_FILE_READ_DATA) ||
            ((protect & (VIBEOS_NT_PAGE_READWRITE | VIBEOS_NT_PAGE_EXECUTE_READWRITE)) &&
             !(granted & NT_FILE_WRITE_DATA))) {
            vibeos_file_put(ctx.file);
            return NT_STATUS(VIBEOS_STATUS_ACCESS_DENIED);
        }
        vibeos_file_stat_clear(&st);
        if (!ctx.file->ops->stat || ctx.file->ops->stat(ctx.file, &st) != 0) {
            vibeos_file_put(ctx.file);
            return NT_STATUS(VIBEOS_STATUS_INVALID_PARAMETER);
        }
        if (size == 0u) {
            size = st.size;
            if (size == 0u) {
                vibeos_file_put(ctx.file);
                return NT_STATUS(VIBEOS_STATUS_MAPPED_FILE_SIZE_ZERO);
            }
        } else if (size > st.size) {
            /* Growing the file is a write, and no handle can write yet (W4). */
            vibeos_file_put(ctx.file);
            return NT_STATUS(VIBEOS_STATUS_SECTION_TOO_BIG);
        }
        ctx.shared = 0u;
        ctx.attributes |= NT_SEC_FILE;
    }
    ctx.size = size;
    r = nt_create_common(&g_nt_section_ops, out, want, oa, 0, section_init, &ctx);
    vibeos_file_put(ctx.file);
    return r;
}

/* NtCreateSection(SectionHandle, DesiredAccess, ObjectAttributes, MaximumSize,
 * SectionPageProtection, AllocationAttributes, FileHandle). */
static long nt_create_section(uint64_t out, uint64_t want, uint64_t oa, uint64_t max_size,
                              uint64_t protection, uint64_t attributes, uint64_t file_h) {
    vibeos_file_t *file;
    section_ctx_t ctx;
    uint64_t size = 0;
    uint32_t st, granted = 0;
    long r;
    int pr;

    if ((attributes & NT_SEC_IMAGE) &&
        (attributes & (NT_SEC_COMMIT | NT_SEC_RESERVE | NT_SEC_NOCACHE))) {
        return NT_STATUS(VIBEOS_STATUS_INVALID_PARAMETER_6);
    }
    if (!(attributes & NT_SEC_IMAGE)) {
        return create_data_section(out, (uint32_t)want, oa, max_size, (uint32_t)protection,
                                   (uint32_t)attributes, file_h);
    }
    if (file_h == 0u) {
        return NT_STATUS(VIBEOS_STATUS_INVALID_FILE_FOR_SECTION);   /* an image is a file's */
    }
    /* The file must be readable through the handle - FILE_READ_DATA - and be
     * an image this kernel can place: refused at creation, as Windows does,
     * rather than at the first map. */
    st = nt_handle_ref(file_h, 0, 0, &file, &granted);
    if (st != VIBEOS_STATUS_SUCCESS) {
        return NT_STATUS(st);
    }
    /* The type before the rights, as Windows checks a handle. */
    if (file->ops->nt_type || file->path[0] != '/') {
        vibeos_file_put(file);
        return NT_STATUS(VIBEOS_STATUS_OBJECT_TYPE_MISMATCH);   /* not a file */
    }
    if (!(granted & NT_FILE_READ_DATA)) {
        vibeos_file_put(file);
        return NT_STATUS(VIBEOS_STATUS_ACCESS_DENIED);
    }
    /* The kernel reads an image by its path, so the path has to name the file
     * the handle holds - a rename since the open would have the section made
     * of another file (M-106). Asked again at every map, below. */
    if (!vibeos_file_still_at_path(file)) {
        vibeos_file_put(file);
        return NT_STATUS(VIBEOS_STATUS_FILE_RENAMED);
    }
    pr = ks_pe_probe(file, &size);
    if (pr != 0) {
        vibeos_file_put(file);
        return NT_STATUS(pr == -VIBEOS_ENOENT ? VIBEOS_STATUS_OBJECT_NAME_NOT_FOUND
                                              : NT_STATUS_INVALID_IMAGE_FORMAT);
    }
    ctx.file = file;
    ctx.size = size;
    ctx.data = 0u;
    ctx.shared = 0u;
    ctx.protect = VIBEOS_NT_PAGE_EXECUTE_WRITECOPY;
    ctx.attributes = NT_SEC_IMAGE | NT_SEC_FILE;
    r = nt_create_common(&g_nt_section_ops, out, (uint32_t)want, oa, 0, section_init, &ctx);
    vibeos_file_put(file);
    return r;
}

/* NtOpenSection(SectionHandle, DesiredAccess, ObjectAttributes): a section
 * somebody made with a name. */
static long nt_open_section(uint64_t out, uint64_t want, uint64_t oa) {
    return nt_create_common(&g_nt_section_ops, out, (uint32_t)want, oa, 1, 0, 0);
}

/* A view of a data section: [offset, offset + size) of it, at the address
 * given (a multiple of 64 KiB) or wherever there is room. */
typedef struct {
    uint64_t size;
    uint32_t data, shared, protect;
} section_view_t;

static long map_data_view(const section_view_t *sec, vibeos_file_t *file, uint32_t granted,
                          uint64_t base_u, uint64_t offset_u, uint64_t size_u, uint32_t protect) {
    vibeos_procstate_t *ps;
    vibeos_vma_t t;
    vibeos_prot_t access;
    uint64_t want_base = 0, want_size = 0, off = 0, base, len;
    uint32_t st, need = 0;
    long rc;
    int me = ks_current();

    if (me < 0 || !ks_id(me)->is_user) {
        return NT_STATUS(VIBEOS_STATUS_INVALID_HANDLE);
    }
    ps = ks_ps(me);
    st = view_allowed(sec->protect, protect, &need);
    if (st == 0u && (protect & ~(0xFFu | VIBEOS_NT_PAGE_NOCACHE | VIBEOS_NT_PAGE_WRITECOMBINE))) {
        st = VIBEOS_STATUS_INVALID_PAGE_PROTECTION;
    }
    if (st != 0u) {
        return NT_STATUS(st);
    }
    if ((granted & need) != need) {
        return NT_STATUS(VIBEOS_STATUS_ACCESS_DENIED);
    }
    if (vibeos_uaccess_copy(&want_base, (const void *)(uintptr_t)base_u, sizeof(want_base)) != 0 ||
        vibeos_uaccess_copy(&want_size, (const void *)(uintptr_t)size_u, sizeof(want_size)) != 0 ||
        (offset_u != 0u &&
         vibeos_uaccess_copy(&off, (const void *)(uintptr_t)offset_u, sizeof(off)) != 0)) {
        return NT_STATUS(VIBEOS_STATUS_ACCESS_VIOLATION);
    }
    if ((off & (VIBEOS_NT_ALLOCATION_GRANULARITY - 1u)) != 0u ||
        (want_base & (VIBEOS_NT_ALLOCATION_GRANULARITY - 1u)) != 0u) {
        return NT_STATUS(VIBEOS_STATUS_MAPPED_ALIGNMENT);
    }
    if (off >= sec->size) {
        return NT_STATUS(VIBEOS_STATUS_INVALID_VIEW_SIZE);
    }
    if (want_size == 0u) {
        want_size = sec->size - off;   /* to the end of the section */
    } else if (want_size > sec->size - off) {
        return NT_STATUS(VIBEOS_STATUS_INVALID_VIEW_SIZE);
    }
    len = (want_size + 0xFFFull) & ~0xFFFull;
    (void)nt_prot_to_region(protect, 1, &access);
    t.prot = access;
    t.backing_id = 0;
    t.backing_offset = 0;
    t.user_prot = protect;
    t.alloc_prot = protect;

    ks_mm_lock(ps);
    if (want_base != 0u) {
        base = want_base;
        if (!ks_user_fixed_ok(base, len)) {
            st = VIBEOS_STATUS_INVALID_PARAMETER_3;
        } else if (vibeos_umem_region_in(ps, base, len)) {
            st = VIBEOS_STATUS_CONFLICTING_ADDRESSES;
        }
    } else {
        base = vibeos_umem_place(ps, len, VIBEOS_NT_ALLOCATION_GRANULARITY);
        if (base == 0u) {
            st = VIBEOS_STATUS_NO_MEMORY;
        }
    }
    if (st == 0u && !vibeos_umem_as_room(ps, len)) {
        st = VIBEOS_STATUS_NO_MEMORY;
    }
    if (st == 0u) {
        /* A copy-on-write view of a shared section is the process's own copy:
         * its stores reach nobody else. */
        int shared = sec->shared &&
                     (protect & 0xFFu) != VIBEOS_NT_PAGE_WRITECOPY &&
                     (protect & 0xFFu) != VIBEOS_NT_PAGE_EXECUTE_WRITECOPY;

        /* A shared view that may not be written now may never be: through a
         * handle without SECTION_MAP_WRITE, or of a section that is not
         * writable, NtProtectVirtualMemory must not make it so (H-033, this
         * personality's). A copy-on-write view stores into its own pages. */
        if (shared && (!(granted & NT_SECTION_MAP_WRITE) ||
                       !(sec->protect & (VIBEOS_NT_PAGE_READWRITE | VIBEOS_NT_PAGE_EXECUTE_READWRITE)))) {
            t.prot = (vibeos_prot_t)(t.prot | VIBEOS_PROT_NOWRITE);
        }
        t.alloc_base = base;
        rc = vibeos_umem_map_file(me, ps, base, len, file, off, shared, &t);
        if (rc != 0) {
            st = rc == -VIBEOS_ENOMEM ? VIBEOS_STATUS_NO_MEMORY : nt_status_of_errno(rc);
        }
    }
    ks_mm_unlock(ps);
    if (st != 0u) {
        return NT_STATUS(st);
    }
    if (vibeos_uaccess_copy((void *)(uintptr_t)base_u, &base, sizeof(base)) != 0 ||
        vibeos_uaccess_copy((void *)(uintptr_t)size_u, &len, sizeof(len)) != 0) {
        return NT_STATUS(VIBEOS_STATUS_ACCESS_VIOLATION);
    }
    return NT_STATUS(VIBEOS_STATUS_SUCCESS);
}

/* NtMapViewOfSection(SectionHandle, ProcessHandle, BaseAddress, ZeroBits,
 * CommitSize, SectionOffset, ViewSize, InheritDisposition, AllocationType,
 * Win32Protect), into the calling process. An image is placed by the kernel; a
 * caller that names an address for one is asking for what W2 did not do. */
static long nt_map_view_of_section(uint64_t sec_h, uint64_t process, uint64_t base_u,
                                   uint64_t zero_bits, uint64_t commit, uint64_t offset_u,
                                   uint64_t size_u, uint64_t protect) {
    vibeos_file_t *f, *file;
    section_view_t sec = { 0, 0, 0, 0 };
    uint64_t want_base = 0, base = 0, size = 0;
    uint32_t st, granted = 0;
    nt_ob_t *o;
    int rc, me = ks_current();

    (void)commit;
    if (process != VIBEOS_NT_CURRENT_PROCESS) {
        return NT_STATUS(VIBEOS_STATUS_INVALID_HANDLE);   /* another process: W6 */
    }
    if (zero_bits > 21u && zero_bits < 32u) {
        return NT_STATUS(VIBEOS_STATUS_INVALID_PARAMETER_4);
    }
    st = nt_handle_ref(sec_h, &g_nt_section_ops, NT_SECTION_MAP_READ, &f, &granted);
    if (st != VIBEOS_STATUS_SUCCESS) {
        return NT_STATUS(st);
    }
    nt_ob_lock();
    o = nt_ob_of(f);
    file = o ? o->u.sec.file : 0;
    vibeos_file_get(file);
    if (o) {
        sec.size = o->u.sec.image_size;
        sec.data = o->u.sec.data;
        sec.shared = o->u.sec.shared;
        sec.protect = o->u.sec.protect;
    }
    nt_ob_unlock();
    vibeos_file_put(f);
    if (!file || me < 0) {
        vibeos_file_put(file);
        return NT_STATUS(VIBEOS_STATUS_INVALID_HANDLE);
    }
    if (sec.data) {
        long r = map_data_view(&sec, file, granted, base_u, offset_u, size_u, (uint32_t)protect);

        vibeos_file_put(file);
        return r;
    }
    if (vibeos_uaccess_copy(&want_base, (const void *)(uintptr_t)base_u, sizeof(want_base)) != 0) {
        vibeos_file_put(file);
        return NT_STATUS(VIBEOS_STATUS_ACCESS_VIOLATION);
    }
    if (want_base != 0u) {
        vibeos_file_put(file);
        return NT_STATUS(VIBEOS_STATUS_NOT_IMPLEMENTED);
    }
    /* The same question as at creation. The mapper reads the description's
     * node, not the path, so a rename after this look changes nothing it
     * reads (M-106); what the look still guards is FAT, where a node is a
     * directory slot, and a file unlinked and replaced leaves the slot naming
     * another file - so a section whose name has moved is refused. */
    if (!vibeos_file_still_at_path(file)) {
        vibeos_file_put(file);
        return NT_STATUS(VIBEOS_STATUS_FILE_RENAMED);
    }
    rc = ks_map_pe_image(me, file, &base, &size);
    vibeos_file_put(file);
    if (rc < 0) {
        return NT_STATUS(rc == -VIBEOS_ENOMEM ? VIBEOS_STATUS_NO_MEMORY
                                              : NT_STATUS_INVALID_IMAGE_FORMAT);
    }
    if (vibeos_uaccess_copy((void *)(uintptr_t)base_u, &base, sizeof(base)) != 0 ||
        vibeos_uaccess_copy((void *)(uintptr_t)size_u, &size, sizeof(size)) != 0) {
        return NT_STATUS(VIBEOS_STATUS_ACCESS_VIOLATION);
    }
    /* Moved is a success with its own number: the loader then knows the
     * image's absolute addresses were fixed for where it is. */
    return NT_STATUS(rc ? NT_STATUS_IMAGE_NOT_AT_BASE : VIBEOS_STATUS_SUCCESS);
}

/* NtUnmapViewOfSection(ProcessHandle, BaseAddress): the view this address is
 * in - all of it, every region of its allocation, wherever in it the address
 * falls, as Wine's does - out of the address space.
 * An image's view is the loader's to keep: its pages carry no allocation, and
 * no DLL is unloaded yet. */
static long nt_unmap_view_of_section(uint64_t process, uint64_t base) {
    vibeos_procstate_t *ps;
    const vibeos_vma_t *r;
    uint64_t end;
    uint32_t st = VIBEOS_STATUS_SUCCESS;
    int me = ks_current();

    if (process != VIBEOS_NT_CURRENT_PROCESS || me < 0 || !ks_id(me)->is_user) {
        return NT_STATUS(VIBEOS_STATUS_INVALID_HANDLE);
    }
    ps = ks_ps(me);
    if (!ps) {
        return NT_STATUS(VIBEOS_STATUS_INVALID_HANDLE);   /* a user task has one (code scanning #316) */
    }
    ks_mm_lock(ps);
    r = vibeos_vma_find(&ps->mm->vmas, base);
    if (!r || r->alloc_base == 0u || r->backing == VIBEOS_BACKING_ANON) {
        st = VIBEOS_STATUS_NOT_MAPPED_VIEW;   /* no view there, or private memory */
    } else {
        const uint64_t ab = r->alloc_base;

        for (end = ab; (r = vibeos_vma_find(&ps->mm->vmas, end)) != 0 && r->alloc_base == ab;) {
            end = r->base + r->len;
        }
        if (vibeos_umem_unmap(me, ps, ab, end) != 0) {
            st = VIBEOS_STATUS_NO_MEMORY;
        }
    }
    ks_mm_unlock(ps);
    return NT_STATUS(st);
}

/* NtQuerySection(SectionHandle, Class, Buffer, Length, ResultLength):
 * SectionBasicInformation - the attributes and the size. An image's
 * SectionImageInformation is the loader's question, and not asked yet. */
static long nt_query_section(uint64_t sec_h, uint64_t cls, uint64_t buf, uint64_t len,
                             uint64_t ret_len) {
    vibeos_nt_section_basic_t sbi;
    vibeos_file_t *f;
    uint64_t got = sizeof(sbi);
    uint32_t st, i;
    nt_ob_t *o;

    st = nt_handle_ref(sec_h, &g_nt_section_ops, NT_SECTION_QUERY, &f, 0);
    if (st != VIBEOS_STATUS_SUCCESS) {
        return NT_STATUS(st);
    }
    for (i = 0; i < sizeof(sbi); i++) {
        ((uint8_t *)&sbi)[i] = 0;
    }
    nt_ob_lock();
    o = nt_ob_of(f);
    if (o) {
        sbi.attributes = o->u.sec.attributes;
        sbi.size = o->u.sec.image_size;
        st = (uint32_t)cls == VIBEOS_NT_SECTION_BASIC_INFORMATION ? 0u
             : (uint32_t)cls == 1u && o->u.sec.data ? VIBEOS_STATUS_SECTION_NOT_IMAGE
             : VIBEOS_STATUS_INVALID_INFO_CLASS;
    } else {
        st = VIBEOS_STATUS_INVALID_HANDLE;
    }
    nt_ob_unlock();
    vibeos_file_put(f);
    if (st != 0u) {
        return NT_STATUS(st);
    }
    if (len < sizeof(sbi)) {
        return NT_STATUS(VIBEOS_STATUS_INFO_LENGTH_MISMATCH);
    }
    if (vibeos_uaccess_copy((void *)(uintptr_t)buf, &sbi, sizeof(sbi)) != 0 ||
        (ret_len != 0u &&
         vibeos_uaccess_copy((void *)(uintptr_t)ret_len, &got, sizeof(got)) != 0)) {
        return NT_STATUS(VIBEOS_STATUS_ACCESS_VIOLATION);
    }
    return NT_STATUS(VIBEOS_STATUS_SUCCESS);
}

#define NT_SECTION_SERVICES(X) \
    X(VIBEOS_NT_NR_NtCreateSection, NtCreateSection, MAP, PTRS(OUT(0, 8)), \
      nt_create_section(ARG(0), NT_ARG32(1), ARG(2), ARG(3), NT_ARG32(4), NT_ARG32(5), ARG(6))) \
    X(VIBEOS_NT_NR_NtOpenSection, NtOpenSection, MAP, PTRS(OUT(0, 8)), \
      nt_open_section(ARG(0), NT_ARG32(1), ARG(2))) \
    X(VIBEOS_NT_NR_NtMapViewOfSection, NtMapViewOfSection, MAP, \
      PTRS(OUT(2, 8), OUT(6, 8)), \
      nt_map_view_of_section(ARG(0), ARG(1), ARG(2), ARG(3), ARG(4), ARG(5), ARG(6), \
                             NT_ARG32(9))) \
    X(VIBEOS_NT_NR_NtUnmapViewOfSection, NtUnmapViewOfSection, UNMAP, NOPTR, \
      nt_unmap_view_of_section(ARG(0), ARG(1))) \
    X(VIBEOS_NT_NR_NtQuerySection, NtQuerySection, MINCORE, \
      PTRS(OUT(2, sizeof(vibeos_nt_section_basic_t)), OUT_OPT(4, 8)), \
      nt_query_section(ARG(0), NT_ARG32(1), ARG(2), ARG(3), ARG(4)))

NT_DEFINE_SERVICES(section, NT_SECTION_SERVICES)
