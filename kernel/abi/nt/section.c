/* NT personality: image sections (docs/abi/windows.md W2).
 *
 * How a Windows loader brings a DLL in: NtOpenFile the file, NtCreateSection
 * with SEC_IMAGE over it, NtMapViewOfSection into the process. The mapping is
 * the kernel's, placing and relocation included, and it is the same placing and
 * relocation exec gives a program (kernel/arch/x86_64/pe_load.c).
 *
 * W2 has image sections only. A section of a file's bytes, or of memory alone,
 * is W3's with the rest of the memory services; asking for one is refused as
 * not implemented rather than answered with an image. */

#include "nt_internal.h"

#define NT_SEC_IMAGE                 0x01000000u
#define NT_SECTION_QUERY             0x0001u
#define NT_SECTION_MAP_WRITE         0x0002u
#define NT_SECTION_MAP_READ          0x0004u
#define NT_SECTION_MAP_EXECUTE       0x0008u
#define NT_SECTION_ALL_ACCESS        0x000F001Fu
#define NT_STATUS_IMAGE_NOT_AT_BASE  0x40000003u
#define NT_STATUS_INVALID_IMAGE_FORMAT VIBEOS_STATUS_INVALID_IMAGE_FORMAT

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

/* The section takes a reference of its own: whatever happens to the create
 * after this, the caller's reference is the caller's to give back. */
typedef struct {
    vibeos_file_t *file;
    uint64_t size;
} section_ctx_t;

static void section_init(nt_ob_t *o, const void *ctx) {
    const section_ctx_t *c = (const section_ctx_t *)ctx;
    o->u.sec.file = c->file;
    vibeos_file_get(o->u.sec.file);
    o->u.sec.image_size = c->size;
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

    (void)max_size;
    (void)protection;
    if (!(attributes & NT_SEC_IMAGE) || file_h == 0u) {
        return NT_STATUS(VIBEOS_STATUS_NOT_IMPLEMENTED);   /* W3's sections */
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
    if (!(granted & 0x0001u)) {   /* FILE_READ_DATA */
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
    pr = ks_pe_probe(file->path, &size);
    if (pr != 0) {
        vibeos_file_put(file);
        return NT_STATUS(pr == -VIBEOS_ENOENT ? VIBEOS_STATUS_OBJECT_NAME_NOT_FOUND
                                              : NT_STATUS_INVALID_IMAGE_FORMAT);
    }
    ctx.file = file;
    ctx.size = size;
    r = nt_create_common(&g_nt_section_ops, out, (uint32_t)want, oa, 0, section_init, &ctx);
    vibeos_file_put(file);
    return r;
}

/* NtMapViewOfSection(SectionHandle, ProcessHandle, BaseAddress, ZeroBits,
 * CommitSize, SectionOffset, ViewSize, InheritDisposition, AllocationType,
 * Win32Protect), for an image section into the calling process. The kernel
 * chooses the address; a caller that names one is asking for W3's placement. */
static long nt_map_view_of_section(uint64_t sec_h, uint64_t process, uint64_t base_u,
                                   uint64_t zero_bits, uint64_t commit, uint64_t offset_u,
                                   uint64_t size_u) {
    vibeos_file_t *f, *file;
    uint64_t want_base = 0, base = 0, size = 0;
    uint32_t st;
    nt_ob_t *o;
    int rc, me = ks_current();

    (void)zero_bits;
    (void)commit;
    (void)offset_u;
    if (process != VIBEOS_NT_CURRENT_PROCESS) {
        return NT_STATUS(VIBEOS_STATUS_INVALID_HANDLE);   /* another process: W6 */
    }
    if (vibeos_uaccess_copy(&want_base, (const void *)(uintptr_t)base_u, sizeof(want_base)) != 0) {
        return NT_STATUS(VIBEOS_STATUS_ACCESS_VIOLATION);
    }
    if (want_base != 0u) {
        return NT_STATUS(VIBEOS_STATUS_NOT_IMPLEMENTED);
    }
    st = nt_handle_ref(sec_h, &g_nt_section_ops, NT_SECTION_MAP_READ, &f, 0);
    if (st != VIBEOS_STATUS_SUCCESS) {
        return NT_STATUS(st);
    }
    nt_ob_lock();
    o = nt_ob_of(f);
    file = o ? o->u.sec.file : 0;
    vibeos_file_get(file);
    nt_ob_unlock();
    vibeos_file_put(f);
    if (!file || me < 0) {
        vibeos_file_put(file);
        return NT_STATUS(VIBEOS_STATUS_INVALID_HANDLE);
    }
    /* The same question as at creation, and here it matters most: a section
     * can be held for ever before it is mapped, and a file renamed over its
     * name meanwhile would be the one mapped (M-106). What remains is the
     * moment between this look and the mapper's own lookup, inside one call;
     * mapping from the description itself closes that, with a page cache
     * that knows files by node rather than by name. */
    if (!vibeos_file_still_at_path(file)) {
        vibeos_file_put(file);
        return NT_STATUS(VIBEOS_STATUS_FILE_RENAMED);
    }
    rc = ks_map_pe_image(me, file->path, &base, &size);
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

#define NT_SECTION_SERVICES(X) \
    X(VIBEOS_NT_NR_NtCreateSection, NtCreateSection, MAP, PTRS(OUT(0, 8)), \
      nt_create_section(ARG(0), NT_ARG32(1), ARG(2), ARG(3), NT_ARG32(4), NT_ARG32(5), ARG(6))) \
    X(VIBEOS_NT_NR_NtMapViewOfSection, NtMapViewOfSection, MAP, \
      PTRS(OUT(2, 8), OUT(6, 8)), \
      nt_map_view_of_section(ARG(0), ARG(1), ARG(2), ARG(3), ARG(4), ARG(5), ARG(6)))

NT_DEFINE_SERVICES(section, NT_SECTION_SERVICES)
