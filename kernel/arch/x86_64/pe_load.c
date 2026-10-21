/* Loading a PE program: the NT personality's half of hw_proc_create
 * (docs/abi/windows.md W1).
 *
 * What the kernel does for a Windows program is what Windows' kernel does: map
 * the executable and ntdll, build the PEB and the first thread's TEB, and start
 * that thread in ntdll. Linking the program against its DLLs is ntdll's work,
 * in user mode (user/win/ntdll/start.c).
 *
 * Both images are placed by the same page-at-a-time mapper ELF images go
 * through - the PE parser describes them in its segment form. Where its header
 * asks, if that is inside the window it is allowed; otherwise, since W2,
 * somewhere it can be, with its base relocations applied as each page is
 * filled (vibeos_pe_relocate_page). An image that cannot move - relocations
 * stripped - is still refused by name. */

#include "vibeos/pe.h"
#include "vibeos/abi_nt.h"
#include "vibeos/exec_stats.h"
#include "arch_hw_internal.h"
#include "vibeos/file.h"

/* The base-relocation directory of the image being mapped, read whole: an exec
 * holds g_exec_lock, so this is one exec's at a time. Wine's test programs
 * carry five kilobytes; a DLL that needs more is refused by name rather than
 * half relocated. */
#define HW_PE_RELOC_MAX 65536u
static uint8_t g_pe_relocs[HW_PE_RELOC_MAX];
/* The two images' descriptions, for the same reason and under the same lock.
 * They were locals - 1,368 bytes each - and in the gcc Debug build they took
 * the exec of a Windows program past the base of its eight-kilobyte kernel
 * stack (hw_kstack_check names it now). */
static vibeos_pe_image_t g_pe_exe, g_pe_dll;

/* Map one PE image from `buf` (its first `window` bytes staged) and the page
 * cache identity `id` for the rest: at its own base if that lies in [lo, hi),
 * else at `moved` (0: nowhere else) inside [moved, moved_hi). 0, or the
 * refusal's -1. */
static int hw_pe_map_one(hw_proc_t *p, hw_procstate_t *ps, const unsigned char *buf,
                         uint64_t window, uint64_t len, uint32_t id, uint64_t lo, uint64_t hi,
                         uint64_t moved, uint64_t moved_hi,
                         int want_dll, const char *path, vibeos_pe_image_t *pe,
                         uint8_t *relocs) {
    int rc = vibeos_pe_parse(buf, window, len, lo, hi, pe);
    hw_reloc_t reloc;

    if (rc == VIBEOS_PE_ERANGE && moved != 0u) {
        rc = vibeos_pe_parse_at(buf, window, len, moved, moved, moved_hi, pe);
    }
    if (rc == VIBEOS_PE_ERANGE) {
        return hw_exec_refuse(VIBEOS_EXEC_BAD_WINDOW, path, "pe_cannot_be_placed");
    }
    if (rc != VIBEOS_PE_OK) {
        return hw_exec_refuse(VIBEOS_EXEC_BAD_HEADER, path,
                              rc == VIBEOS_PE_ETOOMANY ? "pe_too_many_sections" : "pe_parse");
    }
    if (!!(pe->characteristics & VIBEOS_PE_FILE_DLL) != want_dll || pe->entry == 0u) {
        return hw_exec_refuse(VIBEOS_EXEC_BAD_HEADER, path,
                              want_dll ? "pe_not_a_dll" : "pe_not_a_program");
    }
    /* Without a cache identity the mapper fills every page from the staged
     * bytes, so a file longer than the window would be filled from whatever
     * lies past it. */
    if (id == 0u && len > window) {
        return hw_exec_refuse(VIBEOS_EXEC_NO_STAGING, path, "pe_past_window");
    }
    reloc.pe = pe;
    reloc.relocs = relocs;
    reloc.len = 0;
    if (pe->delta != 0) {
        /* The directory, out of the image as the file supplies it - through
         * the cache, or the staged bytes - then checked once, so the per-page
         * work trusts it. */
        hw_elf_cache_reader_t cr;
        uint32_t size = pe->dir_size[VIBEOS_PE_DIR_BASERELOC];
        uint64_t at = pe->image_base + pe->dir_rva[VIBEOS_PE_DIR_BASERELOC];

        if (size > HW_PE_RELOC_MAX) {
            return hw_exec_refuse(VIBEOS_EXEC_NO_STAGING, path, "pe_relocs_too_large");
        }
        cr.file_id = id;
        cr.file_len = len;
        if (id != 0u) {
            vibeos_pe_read_image(pe, hw_elf_read_cached, &cr, at, relocs, size);
        } else {
            vibeos_pe_read_image_mem(pe, buf, len, at, relocs, size);
        }
        if (vibeos_pe_check_relocs(pe, relocs, size) != VIBEOS_PE_OK) {
            return hw_exec_refuse(VIBEOS_EXEC_BAD_HEADER, path, "pe_relocs");
        }
        reloc.len = size;
    }
    if (hw_map_elf_image(&p->as, &ps->vmas, &pe->layout, buf, id, len, &reloc) != 0) {
        return hw_exec_refuse(VIBEOS_EXEC_NO_MEMORY, path, "pe_map");
    }
    return 0;
}

int hw_pe_map_images(hw_proc_t *p, hw_procstate_t *ps, const unsigned char *hdr,
                     uint64_t len, uint64_t staged, const char *path, uint32_t file_id,
                     uint64_t *exe_base) {
    vibeos_pe_image_t *exe = &g_pe_exe, *dll = &g_pe_dll;
    uint32_t dll_id = 0;
    long n;

    /* As the ELF path does, before anything is mapped: the struct arrives
     * holding the previous program's regions. */
    ps->vmas.head = 0;
    ps->vmas.count = 0;
    if (hw_pe_map_one(p, ps, hdr, len < staged ? len : staged, len, file_id,
                      VIBEOS_HW_USER_BASE, VIBEOS_HW_PE_EXE_END,
                      VIBEOS_HW_PE_EXE_MOVED, VIBEOS_HW_PE_DLL_BASE, 0, path, exe,
                      g_pe_relocs) != 0) {
        return -1;
    }
    /* Our ntdll, through the interpreter's staging buffer: an exec holds
     * g_exec_lock, so the buffer is this exec's for as long as it runs. */
    if (g_interp_elf == 0) {
        return hw_exec_refuse(VIBEOS_EXEC_NO_STAGING, VIBEOS_HW_NTDLL_PATH, "pe_staging");
    }
    n = hw_read_file_cached(VIBEOS_HW_NTDLL_PATH, g_interp_elf, g_interp_elf_cap, &dll_id);
    if (n <= 0) {
        return hw_exec_refuse(VIBEOS_EXEC_NO_INTERP, VIBEOS_HW_NTDLL_PATH, "read");
    }
    if (hw_pe_map_one(p, ps, g_interp_elf,
                      (uint64_t)n < g_interp_elf_cap ? (uint64_t)n : g_interp_elf_cap,
                      (uint64_t)n, dll_id, VIBEOS_HW_PE_DLL_BASE, VIBEOS_HW_NT_BLOCK,
                      0u, 0u, 1, VIBEOS_HW_NTDLL_PATH, dll, g_pe_relocs) != 0) {
        return -1;
    }
    /* The first thread starts in ntdll, which links the program and calls its
     * entry; the program's own entry is in its headers, where ntdll reads it. */
    p->entry = dll->entry;
    p->abi = vibeos_abi_nt();
    *exe_base = exe->image_base;

    vibeos_x86_64_serial_lock();
    vibeos_x86_64_serial_puts("[EXEC] NT program ");
    vibeos_x86_64_serial_puts(path && path[0] ? path : "-");
    vibeos_x86_64_serial_puts(" at 0x");
    vibeos_x86_64_serial_print_hex(exe->image_base);
    if (exe->delta != 0) {
        vibeos_x86_64_serial_puts(" moved from 0x");
        vibeos_x86_64_serial_print_hex(exe->preferred_base);
    }
    vibeos_x86_64_serial_puts(" ntdll at 0x");
    vibeos_x86_64_serial_print_hex(dll->image_base);
    vibeos_x86_64_serial_puts("\n");
    vibeos_x86_64_serial_unlock();
    return 0;
}

int hw_pe_start_blocks(hw_proc_t *p, hw_procstate_t *ps, uint64_t exe_base,
                       const char *const *argv, const char *path) {
    uint8_t *pages[VIBEOS_NT_BLOCK_PAGES];
    vibeos_nt_start_t s;
    int cur = hw_current_task();
    uint32_t i, k, mask;

    for (i = 0; i < VIBEOS_NT_BLOCK_PAGES; i++) {
        uint8_t *page = (uint8_t *)hw_alloc_page();

        if (!page) {
            return hw_exec_refuse(VIBEOS_EXEC_NO_MEMORY, path, "nt_blocks");
        }
        for (k = 0; k < 4096u; k++) {
            page[k] = 0;
        }
        if (hw_map_page(&p->as, VIBEOS_HW_NT_BLOCK + (uint64_t)i * 4096ull,
                        (uint64_t)(uintptr_t)page,
                        PTE_PRESENT | PTE_WRITE | PTE_USER | PTE_NX) != 0) {
            hw_page_put((uint64_t)(uintptr_t)page);
            return hw_exec_refuse(VIBEOS_EXEC_NO_MEMORY, path, "nt_blocks_map");
        }
        hw_page_put((uint64_t)(uintptr_t)page);   /* the mapping owns it now */
        pages[i] = page;   /* still reachable through the identity map */
    }
    /* One region for the five, as the stack has one: munmap, mprotect and fork
     * decide by regions. */
    if (vibeos_vma_insert(&ps->vmas, VIBEOS_HW_NT_BLOCK,
                          (uint64_t)VIBEOS_NT_BLOCK_PAGES * 4096ull,
                          (vibeos_prot_t)(VIBEOS_PROT_READ | VIBEOS_PROT_WRITE |
                                          VIBEOS_PROT_USER),
                          VIBEOS_BACKING_ANON, 0, 0) != 0) {
        return hw_exec_refuse(VIBEOS_EXEC_NO_MEMORY, path, "nt_blocks_region");
    }

    for (i = 0; i < (uint32_t)sizeof(s); i++) {
        ((uint8_t *)(void *)&s)[i] = 0;
    }
    mask = ks_cpus_online_mask();
    for (i = 0; i < 32u; i++) {
        s.processors += (mask >> i) & 1u;
    }
    s.image_base = exe_base;
    s.stack_base = VIBEOS_HW_USER_STACK_TOP;
    s.stack_limit = VIBEOS_HW_USER_STACK_FLOOR;
    /* exec keeps the task's identity: these are the ids the program has. */
    s.pid = cur >= 0 ? g_tasks[cur].id.tgid : 0u;
    s.tid = cur >= 0 ? g_tasks[cur].id.pid : 0u;
    /* The standard handles name descriptors 0, 1 and 2 (W1's handles are
     * descriptors; kernel/abi/nt/nt_internal.h). */
    s.std_handle[0] = 4u;
    s.std_handle[1] = 8u;
    s.std_handle[2] = 12u;
    s.image_path = path;
    s.argv = argv;
    if (vibeos_nt_build_blocks(pages, VIBEOS_HW_NT_BLOCK, &s) != 0) {
        return hw_exec_refuse(VIBEOS_EXEC_ARGS_TOO_LARGE, path, "nt_parameters");
    }
    p->start_arg = VIBEOS_HW_NT_BLOCK + VIBEOS_NT_BLOCK_PEB;
    p->gs_base = VIBEOS_HW_NT_BLOCK + VIBEOS_NT_BLOCK_TEB;
    /* Entered as a function is: the return address at rsp (zero - there is
     * nothing to return to) and rsp eight past a sixteen-byte boundary, with the
     * thirty-two bytes above it the callee's to spill its arguments into, as
     * Windows' convention has every call. */
    p->user_sp = VIBEOS_HW_USER_STACK_TOP - 0x38u;
    return 0;
}

/* ---- an image mapped into a running process (NtMapViewOfSection, W2) ----------------
 *
 * The same placing and relocating as exec's, into the calling process's own
 * address space, with buffers of its own: exec's are g_exec_lock's, which a
 * system call of another personality does not hold. A preemptible lock, since
 * reading an image goes to the disk. */

#define HW_NTMAP_WINDOW 65536u
static vibeos_lock_t g_ntmap_lock;
static uint8_t *g_ntmap_hdr;
static uint8_t g_ntmap_relocs[HW_PE_RELOC_MAX];
static vibeos_pe_image_t g_ntmap_pe;   /* off the stack, as exec's are */

/* Is [base, base + len) clear of every region the process has? The list is
 * sorted and never overlaps, so one pass answers. */
static int hw_range_free(const vibeos_vma_list_t *l, uint64_t base, uint64_t len) {
    const vibeos_vma_t *v;

    for (v = l->head; v; v = v->next) {
        if (v->base < base + len && base < v->base + v->len) {
            return 0;
        }
    }
    return 1;
}

/* The image's headers into the staging buffer: its length, and its cache
 * identity in *id; or a negated errno. Under g_ntmap_lock. */
/* The image a description holds, by its node (M-106): whatever its path names
 * now, these are the bytes of the file that was opened. */
static long hw_ntmap_read(const vibeos_file_t *f, uint32_t *id) {
    vibeos_fs_node_t node;
    long n;

    if (!g_ntmap_hdr) {
        g_ntmap_hdr = (uint8_t *)hw_alloc_pages_contig(HW_NTMAP_WINDOW / 4096u);
        if (!g_ntmap_hdr) {
            return -VIBEOS_ENOMEM;
        }
    }
    if (vibeos_file_node(f, &node) != 0) {
        return -VIBEOS_ENOENT;
    }
    n = hw_read_file_node(f->mnt, f->path, &node, g_ntmap_hdr, HW_NTMAP_WINDOW, id);
    return n > 0 ? n : -VIBEOS_ENOENT;
}

int hw_pe_probe(const vibeos_file_t *f, uint64_t *image_size) {
    vibeos_pe_image_t *pe = &g_ntmap_pe;
    uint32_t id = 0;
    long n;
    int rc = 0;

    ks_lock_preemptible(&g_ntmap_lock);
    n = hw_ntmap_read(f, &id);
    if (n < 0) {
        rc = (int)n;
    } else if (vibeos_pe_parse(g_ntmap_hdr, (uint64_t)n < HW_NTMAP_WINDOW ? (uint64_t)n
                                                                         : HW_NTMAP_WINDOW,
                               (uint64_t)n, 0, ~0ull, pe) != VIBEOS_PE_OK) {
        rc = -VIBEOS_ENOEXEC;
    } else {
        *image_size = pe->layout.image_span;
    }
    ks_unlock_preemptible(&g_ntmap_lock);
    return rc;
}

int hw_map_pe_image(int slot, const vibeos_file_t *f, uint64_t *base, uint64_t *size) {
    hw_task_t *t = &g_tasks[slot];
    hw_procstate_t *ps = t->ps;
    vibeos_pe_image_t *pe = &g_ntmap_pe;
    uint32_t id = 0;
    uint64_t window, place = 0, b, span, image_base;
    int64_t delta;
    long n;
    int rc;

    if (!ps) {
        return -VIBEOS_EINVAL;
    }
    ks_lock_preemptible(&g_ntmap_lock);
    n = hw_ntmap_read(f, &id);
    if (n < 0) {
        ks_unlock_preemptible(&g_ntmap_lock);
        return (int)n;
    }
    window = (uint64_t)n < HW_NTMAP_WINDOW ? (uint64_t)n : HW_NTMAP_WINDOW;
    if (vibeos_pe_parse(g_ntmap_hdr, window, (uint64_t)n, 0, ~0ull, pe) != VIBEOS_PE_OK) {
        ks_unlock_preemptible(&g_ntmap_lock);
        return -VIBEOS_ENOEXEC;
    }
    ks_mm_lock(ps);
    /* Where it asks, if that is inside the DLLs' window and free; otherwise the
     * lowest free place there, at the 64 KiB granularity Windows allocates at. */
    span = pe->layout.image_span;
    if (pe->preferred_base >= VIBEOS_HW_PE_DLL_BASE &&
        pe->preferred_base + span <= VIBEOS_HW_NT_BLOCK &&
        hw_range_free(&ps->vmas, pe->preferred_base, span)) {
        place = pe->preferred_base;
    }
    for (b = VIBEOS_HW_PE_DLL_BASE; place == 0u && b + span <= VIBEOS_HW_NT_BLOCK;
         b += 0x10000u) {
        if (hw_range_free(&ps->vmas, b, span)) {
            place = b;
        }
    }
    if (place == 0u) {
        ks_mm_unlock(ps);
        ks_unlock_preemptible(&g_ntmap_lock);
        return -VIBEOS_ENOMEM;
    }
    rc = hw_pe_map_one(&t->proc, ps, g_ntmap_hdr, window, (uint64_t)n, id,
                       place, place + span, place, place + span,
                       (pe->characteristics & VIBEOS_PE_FILE_DLL) ? 1 : 0, f->path, pe,
                       g_ntmap_relocs);
    /* Read out before the lock goes: the next caller reuses the description. */
    image_base = pe->image_base;
    span = pe->layout.image_span;
    delta = pe->delta;
    ks_mm_unlock(ps);
    ks_unlock_preemptible(&g_ntmap_lock);
    if (rc != 0) {
        return -VIBEOS_ENOEXEC;
    }
    *base = image_base;
    *size = span;
    return delta != 0 ? 1 : 0;
}
