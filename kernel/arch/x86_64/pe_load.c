/* Loading a PE program: the NT personality's half of hw_proc_create
 * (docs/abi/windows.md W1).
 *
 * What the kernel does for a Windows program is what Windows' kernel does: map
 * the executable and ntdll, build the PEB and the first thread's TEB, and start
 * that thread in ntdll. Linking the program against its DLLs is ntdll's work,
 * in user mode (user/win/ntdll/start.c).
 *
 * Both images are placed where their headers ask, by the same page-at-a-time
 * mapper ELF images go through - the PE parser describes them in its segment
 * form. An image that would need relocating is refused by name: base
 * relocation is the loader's next step, and placing an image without applying
 * its relocations would start a program whose every absolute address is wrong. */

#include "vibeos/pe.h"
#include "vibeos/abi_nt.h"
#include "vibeos/exec_stats.h"
#include "arch_hw_internal.h"

/* Map one PE image from `buf` (its first `window` bytes staged) and the page
 * cache identity `id` for the rest. 0, or the refusal's -1. */
static int hw_pe_map_one(hw_proc_t *p, hw_procstate_t *ps, const unsigned char *buf,
                         uint64_t window, uint64_t len, uint32_t id, uint64_t lo, uint64_t hi,
                         int want_dll, const char *path, vibeos_pe_image_t *pe) {
    int rc = vibeos_pe_parse(buf, window, len, lo, hi, pe);

    if (rc == VIBEOS_PE_ERANGE) {
        return hw_exec_refuse(VIBEOS_EXEC_BAD_WINDOW, path, "pe_needs_relocation");
    }
    if (rc != VIBEOS_PE_OK) {
        return hw_exec_refuse(VIBEOS_EXEC_BAD_HEADER, path, "pe_parse");
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
    if (hw_map_elf_image(&p->as, &ps->vmas, &pe->layout, buf, id, len) != 0) {
        return hw_exec_refuse(VIBEOS_EXEC_NO_MEMORY, path, "pe_map");
    }
    return 0;
}

int hw_pe_map_images(hw_proc_t *p, hw_procstate_t *ps, const unsigned char *hdr,
                     uint64_t len, uint64_t staged, const char *path, uint32_t file_id,
                     uint64_t *exe_base) {
    vibeos_pe_image_t exe, dll;
    uint32_t dll_id = 0;
    long n;

    /* As the ELF path does, before anything is mapped: the struct arrives
     * holding the previous program's regions. */
    ps->vmas.head = 0;
    ps->vmas.count = 0;
    if (hw_pe_map_one(p, ps, hdr, len < staged ? len : staged, len, file_id,
                      VIBEOS_HW_USER_BASE, VIBEOS_HW_PE_EXE_END, 0, path, &exe) != 0) {
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
                      (uint64_t)n, dll_id, VIBEOS_HW_PE_DLL_BASE, VIBEOS_HW_NT_BLOCK, 1,
                      VIBEOS_HW_NTDLL_PATH, &dll) != 0) {
        return -1;
    }
    /* The first thread starts in ntdll, which links the program and calls its
     * entry; the program's own entry is in its headers, where ntdll reads it. */
    p->entry = dll.entry;
    p->abi = vibeos_abi_nt();
    *exe_base = exe.image_base;

    vibeos_x86_64_serial_lock();
    vibeos_x86_64_serial_puts("[EXEC] NT program ");
    vibeos_x86_64_serial_puts(path && path[0] ? path : "-");
    vibeos_x86_64_serial_puts(" at 0x");
    vibeos_x86_64_serial_print_hex(exe.image_base);
    vibeos_x86_64_serial_puts(" ntdll at 0x");
    vibeos_x86_64_serial_print_hex(dll.image_base);
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
