/* The task lifecycle: creating, describing, signalling and ending tasks.
 *
 * Lifted out of arch_hw.c (C5). Nothing here is new: it sat beside the context switch
 * and named a task's identity in the middle of code that has nothing to do with
 * switching. What the context switch needs of it, and it of the switch, is declared in
 * arch_hw_internal.h. */

#include "arch_hw_internal.h"
#include "vibeos/crash.h"

#define VIBEOS_HW_KERNEL_DS 0x10u

#define VIBEOS_HW_USER_STACK_TOP (VIBEOS_HW_USER_BASE + 0x00400000ull) /* +4 MiB */

/* The free list used to live here, threaded through the first word of every
 * reclaimed page. It is in kernel/mm/frame.c now, threaded through the frame
 * descriptors instead - which is what lets a freed page be poison from end to
 * end rather than poison with a pointer at the front, and lets the poison check
 * read every word instead of skipping the two the list needed.
 *
 * Both sides of the page allocator are shared by every core (a task can fork or
 * exit on any of them), so they take the memory lock. */
/* Written into every page as it is freed.
 *
 * Read as a pointer this is non-canonical - the top sixteen bits are neither
 * all zeroes nor all ones - so dereferencing it faults on the spot instead of
 * quietly reaching some other page. That is the entire point: a freed page
 * used to keep its old contents, so a use-after-free read plausible data and
 * the program carried on, and every memory bug found in this kernel surfaced
 * a long way from its cause. In one case a musl program tripped over its own
 * malloc free list; in another init printed a pointer where a pid belonged.
 * Both were the same page, freed early, still in use. */
/* Must equal FRAME_POISON in kernel/mm/frame.c, which is what actually writes
 * it now. This copy exists because the crash reporter recognises the pattern in
 * a faulting task's stack and says so in words - "this is the kernel free-page
 * poison: the page was reclaimed while still mapped here" - which is the
 * sentence that finally named the munmap defect. */
#define HW_PAGE_POISON 0xDEAD0000DEAD0000ull

static int hw_streq_n(const char *a, const char *b) {
    uint32_t i;
    for (i = 0; a[i] || b[i]; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

/* The identity of a path, or 0 if it cannot be given one. Ids start at 1
 * because the cache uses 0 to mark an empty slot. */
static uint32_t hw_file_id(const char *path) {
    uint32_t i;
    vibeos_fs_node_t node;

    if (!path) {
        return 0;
    }
    for (i = 0; i < g_cached_file_count; i++) {
        if (hw_streq_n(g_cached_files[i].path, path)) {
            return i + 1u;
        }
    }
    if (g_cached_file_count >= VIBEOS_HW_CACHE_FILES) {
        return 0;   /* out of identities: read uncached rather than guess */
    }
    if (vibeos_fs_lookup(&g_rootfs, path, &node) != 0) {
        return 0;
    }
    for (i = 0; i + 1u < sizeof(g_cached_files[0].path) && path[i]; i++) {
        g_cached_files[g_cached_file_count].path[i] = path[i];
    }
    g_cached_files[g_cached_file_count].path[i] = 0;
    g_cached_files[g_cached_file_count].node = node;
    g_cached_file_count++;
    return g_cached_file_count;
}

/* Read a whole file, a page at a time, through the cache.
 *
 * Falls back to the uncached path whenever anything is unusual - no identity
 * left, a file too large for the buffer - because a cache that refuses is a
 * slow machine and a cache that guesses is a wrong one. */
/* `out_id` reports the cache identity these bytes actually came from, or 0 if
 * they did not come from the cache at all. It is not a convenience.
 *
 * A caller that wants to map the file's own pages instead of copying them has
 * to ask the cache for pages of *the same file this buffer was filled from*.
 * Recomputing hw_file_id(path) at the mapping site looked equivalent and is
 * not: this function falls back to an uncached read whenever anything is
 * unusual - no identity left in the table, a file too large for the buffer, a
 * cache lookup that failed - and in every one of those cases the bytes and the
 * pages would have come from different places. Two sources of truth for "which
 * file is this" is how a process ends up with the pages of one file and the
 * headers of another. */
/* Read a file into the cache, and as much of it as fits into `buf`.
 *
 * Returns the length of the *file*, which is not the number of bytes staged.
 * Those were one number until I3 of docs/io/, and separating them is what lets
 * execve keep a header window instead of a buffer large enough for any program
 * it might ever be asked to run.
 *
 * A caller that needs the whole file in the buffer compares the return value
 * against its capacity. A caller that only needs the headers does not have to
 * care, because everything past its window is in the cache and reachable a
 * page at a time. */
long hw_read_file_cached(const char *path, void *buf, uint32_t cap,
                                uint32_t *out_id) {
    uint32_t id = hw_file_id(path);
    uint64_t size, off;
    uint8_t *out = (uint8_t *)buf;

    if (out_id) {
        *out_id = 0u;   /* nothing is cache-backed until this function says so */
    }
    if (id == 0u) {
        return vibeos_fs_read_file(&g_rootfs, path, buf, cap);
    }
    size = g_cached_files[id - 1u].node.size;

    /* The whole file goes into the cache; only what fits goes into the buffer.
     *
     * These were one thing and the conflation is what this phase exists to
     * end. `size > cap` used to refuse the read outright - a refusal about the
     * *buffer*, applied to a file the cache did not need a buffer for. That is
     * why execve carried a four-megabyte staging area: not because anything
     * needed the bytes contiguously, but because this function would not
     * proceed without room for all of them.
     *
     * The loop still walks every page, because that is what populates the
     * cache, and the cache is what the page fill and the interpreter path read
     * through. What changed is that the copy stops at the caller's window and
     * the return value keeps describing the file. */
    for (off = 0; off < size; off += 4096ull) {
        uint64_t phys = 0;
        uint64_t take = size - off < 4096ull ? size - off : 4096ull;
        const uint8_t *src;
        uint64_t k;

        /* With a reference, held across the copy below: another core under
         * memory pressure may evict this page the instant the lookup returns. */
        if (vibeos_cache_get_ref(id, off, &phys) != 0) {
            /* Whatever went wrong, the file itself is still readable the old
             * way. Falling back is the difference between a slower boot and a
             * machine that cannot exec - and out_id stays 0, so a caller that
             * was going to map cache pages knows not to.
             *
             * The fallback needs the whole file in the buffer, so it can only
             * be taken when the buffer could hold it. A window-sized caller
             * that gets here is told no rather than handed a prefix. */
            if (size > (uint64_t)cap) {
                return -1;
            }
            return vibeos_fs_read_file(&g_rootfs, path, buf, cap);
        }
        if (off >= (uint64_t)cap) {
            (void)vibeos_frame_put(phys);
            continue;   /* cached, and past the caller's window */
        }
        if (off + take > (uint64_t)cap) {
            take = (uint64_t)cap - off;
        }
        src = (const uint8_t *)(uintptr_t)phys;
        for (k = 0; k < take; k++) {
            out[off + k] = src[k];
        }
        (void)vibeos_frame_put(phys);
    }
    if (out_id) {
        *out_id = id;   /* every page above came from this file's cache */
    }
    return (long)size;
}

/* A fresh address space: a private PML4 that shares the supervisor-only kernel
 * identity mapping, so ring 0 (syscalls, interrupts) keeps working while running
 * on a process's CR3, but ring 3 cannot touch kernel memory. */
/* Exact string equality. Only used to recognise the one interpreter path this
 * kernel knows how to substitute, so it is a comparison and not a library. */
static int hw_streq(const char *a, const char *b) {
    uint32_t i;
    for (i = 0; a[i] != 0 && b[i] != 0; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return a[i] == b[i];
}

/* Say no, out loud, once. Phase X-P0 of docs/exec/.
 *
 * The loader had fourteen `return -1` sites and one message between them, so
 * every way of failing to start a program produced the same sentence. "Cannot
 * load" is not a diagnosis: the file being absent, the file being present and
 * short, and the file being perfect with no memory left are three unrelated
 * investigations, and the log could not tell them apart.
 *
 * One line, one call, because a message assembled from six serial_puts is six
 * critical sections with three other cores writing into the gaps - and a line
 * cut in half has already cost this project two investigations into crashes
 * that never happened.
 *
 * The counter is portable and separate so a host test can assert it too. */
int hw_exec_refuse(vibeos_exec_fail_t why, const char *path,
                          const char *detail) {
    const char *name = vibeos_exec_refuse(why);

    vibeos_x86_64_serial_lock();
    vibeos_x86_64_serial_puts("[EXEC] refused reason=");
    vibeos_x86_64_serial_puts(name);
    vibeos_x86_64_serial_puts(" path=");
    vibeos_x86_64_serial_puts(path && path[0] ? path : "-");
    if (detail && detail[0]) {
        vibeos_x86_64_serial_puts(" at=");
        vibeos_x86_64_serial_puts(detail);
    }
    vibeos_x86_64_serial_puts("\n");
    vibeos_x86_64_serial_unlock();
    return -1;
}

/* Translate the interpreter path a file asks for into one this boot volume can
 * open. **This is a stand-in for a filesystem layout, not a feature.**
 *
 * A dynamic program asks for /lib/ld-musl-x86_64.so.1; the boot volume is FAT,
 * which has neither that directory nor a name that long, so the loader lives
 * beside the other programs under a name FAT can hold.
 *
 * It is one function on purpose, and `scripts/dev/check-exec-layering.sh` fails
 * the build if any interpreter path is named anywhere else. The risk with a
 * stand-in is not that it exists - it is that it breeds. A second hard-coded
 * path somewhere else, the two disagree, and the substitution stops being
 * something anyone can find, reason about, or delete.
 *
 * If the layout ever becomes real, delete this function rather than
 * generalising it, and delete the check with it. */
static const char *hw_interp_path_substitute(const char *path) {
    if (hw_streq(path, "/lib/ld-musl-x86_64.so.1")) {
        return "EFI/BOOT/LDMUSL.SO";
    }
    return path;
}

/* `len` is how long the file is. `staged` is how many of its bytes are behind
 * `elf`. They were the same number until I3 of docs/io/, and the signature says
 * so now rather than leaving a caller to assume: one caller has the whole image
 * in memory, the other has a 64 KiB header window and the rest in the page
 * cache. */
int hw_proc_create(hw_proc_t *p, hw_procstate_t *ps,
                          const unsigned char *elf, uint64_t len,
                          uint64_t staged,
                          const char *const *argv, const char *const *envp,
                          const char *path, uint32_t file_id) {
    vibeos_elf_image_t img;
    int rc = -1;
    vibeos_elf_stack_desc_t sd;
    uint8_t at_random[16];
    uint8_t *top_page = 0;
    uint32_t i;

    /* Before anything else, and it is not tidiness.
     *
     * interp_base is written in exactly one place - the branch that maps an
     * interpreter - and read unconditionally when the startup block is built.
     * A program without an interpreter therefore shipped whatever was already
     * in this field to ring 3 as AT_BASE: uninitialised kernel stack for an
     * execve (`hw_proc_t np` is a local), or the previous tenant's value for a
     * recycled task slot.
     *
     * That is not a cosmetic leak. A static position-independent binary
     * relocates itself against AT_BASE, so musl's rcrt1 took a stray kernel
     * pointer as its load address and dereferenced it - a ring-3 read of the
     * kernel's log ring at 0x4046220, which is what the nightly reported as
     * "position_independent_binary_did_not_run". It is intermittent for the
     * honest reason that it depends on what was left on the stack.
     *
     * It is also a kernel address handed to ring 3, which is a disclosure
     * whatever the program does with it.
     *
     * Zeroed here rather than at each caller: this function owns the contract,
     * and "every caller must remember to clear a field" is the kind of rule
     * that holds until somebody adds a caller. */
    p->interp_base = 0;

    if (hw_aspace_create(&p->as) != 0) {
        return hw_exec_refuse(VIBEOS_EXEC_NO_ASPACE, path, "aspace_create");
    }
    /* The portable parser validates the file and describes it; this loop just
     * places it. Working a page at a time is what makes a page shared between
     * two segments come out right - allocated once, carrying the permissions
     * of both, holding the bytes of both. */
    /* Two windows are allowed: the one VibeOS programs are linked into, and
     * the low one a Linux executable is linked into. Parsing with the widest
     * bounds and then checking which window the image landed in is what keeps
     * a crafted file from asking to be placed between them - on top of the
     * kernel, for instance, which is linked at 64 MiB. */
    /* Position-independent executables are read twice on purpose.
     *
     * An ET_DYN image describes itself from zero, so where it goes is the
     * loader's decision, not the file's - and the span it needs is what the
     * decision is made from. The first pass asks for that span at bias zero,
     * which is the only question that can be answered without having already
     * chosen. The second pass describes the image where it will actually live,
     * so every address the rest of this function uses - segment addresses, the
     * entry point, the program headers AT_PHDR must point at - is the real one
     * rather than something to be adjusted later and forgotten in one place.
     *
     * Static ET_EXEC files take the same path and answer is_dyn = 0, so there
     * is one route through here rather than two.
     *
     * Interpreters are still refused: a PT_INTERP file needs a second image
     * fetched from the filesystem and mapped alongside this one, and pretending
     * to load it would produce a process that jumps into an empty address. */
    {
        uint64_t bias = 0;
        /* The sizing pass is deliberately not bounded by the window: an
         * ET_DYN image sits at zero until it is placed, so measuring it
         * against the address it has not been given yet rejects every
         * position-independent file. The window is enforced below, on the
         * addresses the image will really occupy, and again by the explicit
         * check after this block. */
        /* The reader is what lets `elf` be a header window rather than the
         * whole file: the interpreter path is the one thing the parser needs
         * that is not in the headers, and with a reader it comes from the page
         * cache like everything else.
         *
         * Null when the cache has no identity for this file - a program it
         * could not take - and then the buffer must cover the path, which it
         * does because that is the path this kernel took for years. */
        hw_elf_cache_reader_t hdr_reader;
        int parse_rc;

        hdr_reader.file_id = file_id;
        hdr_reader.file_len = len;

        /* The window has to cover what the parser will read from it.
         *
         * Everything the parser touches is in the first few kilobytes except
         * the interpreter path, which goes through the reader - so the check
         * is against the header extent, asked of the ELF layer rather than
         * recomputed here. Duplicating the header layout at this call site is
         * how two pieces of code come to disagree about where a program header
         * table starts.
         *
         * Refused by name. A truncation here would hand the parser a partial
         * header table and it would describe a program that does not exist,
         * which is the same shape as the short read that once made execve
         * parse the previous program's bytes. */
        {
            uint64_t extent = 0;
            uint64_t window = (len < staged) ? len : staged;

            if (vibeos_elf_header_extent(elf, window, &extent) == 0 &&
                extent > window) {
                rc = hw_exec_refuse(VIBEOS_EXEC_BAD_HEADER, path,
                                    "headers_past_window");
                goto fail;
            }
        }
        parse_rc = vibeos_elf_parse_read(elf, len, 0, 0,
                                     VIBEOS_HW_USER_STACK_TOP,
                                     VIBEOS_ELF_ALLOW_DYN |
                                     VIBEOS_ELF_ALLOW_INTERP,
                                     file_id ? hw_elf_read_cached : 0,
                                     file_id ? &hdr_reader : 0, &img);

        if (parse_rc != VIBEOS_ELF_OK) {
            { rc = hw_exec_refuse(VIBEOS_EXEC_BAD_HEADER, path, "parse"); goto fail; }
        }
        if (img.is_dyn) {
            /* The low window, the same place a Linux ET_EXEC links itself to.
             * Somewhere else would work equally well for the program, and
             * would need the whole two-window address policy re-argued. */
            bias = VIBEOS_HW_LOW_USER_BASE;
            if (img.image_span > VIBEOS_HW_LOW_USER_LIMIT - bias) {
                /* Would not fit in the window it is offered. */
                { rc = hw_exec_refuse(VIBEOS_EXEC_BAD_WINDOW, path, "pie_span"); goto fail; }
            }
            parse_rc = vibeos_elf_parse_read(elf, len, bias,
                                     VIBEOS_HW_LOW_USER_BASE,
                                     VIBEOS_HW_USER_STACK_TOP,
                                     VIBEOS_ELF_ALLOW_DYN |
                                     VIBEOS_ELF_ALLOW_INTERP,
                                     file_id ? hw_elf_read_cached : 0,
                                     file_id ? &hdr_reader : 0, &img);
            if (parse_rc != VIBEOS_ELF_OK) {
                { rc = hw_exec_refuse(VIBEOS_EXEC_BAD_HEADER, path, "parse_biased"); goto fail; }
            }
        }
    }
    if (!(img.min_vaddr >= VIBEOS_HW_USER_BASE) &&
        !(img.min_vaddr >= VIBEOS_HW_LOW_USER_BASE &&
          img.end_vaddr <= VIBEOS_HW_LOW_USER_LIMIT)) {
        { rc = hw_exec_refuse(VIBEOS_EXEC_BAD_WINDOW, path, "outside_both_windows"); goto fail; }
    }
    /* Empty before anything is mapped, not after.
     *
     * The struct is reused across execs, so it arrives holding the previous
     * program's regions - and clearing it *after* the image was mapped threw
     * away the regions just built for the new one. mprotect then refused the
     * RELRO that a C library performs on its own image during startup, which
     * is how the dynamic loader reported "RELRO protection failed". */
    ps->vmas.head = 0;
    ps->vmas.count = 0;
    if (hw_map_elf_image(&p->as, &ps->vmas, &img, elf, file_id, len) != 0) {
        { rc = hw_exec_refuse(VIBEOS_EXEC_NO_MEMORY, path, "map_image"); goto fail; }
    }
    p->entry = img.entry;

    /* A dynamic program names an interpreter, and the kernel's job is to put
     * both images in the address space and start the *interpreter*, not the
     * program. The interpreter then relocates itself, loads whatever libraries
     * the program needs, and jumps to AT_ENTRY. Starting the program directly
     * would run code whose every external call still points at an unrelocated
     * stub. */
    if (img.has_interp) {
        vibeos_elf_image_t interp;
        const char *interp_path = img.interp;
        long n;
        uint64_t bias;

        if (g_interp_elf == 0) {
            /* Say no rather than half-load. */
            { rc = hw_exec_refuse(VIBEOS_EXEC_NO_STAGING, interp_path, "interp_staging"); goto fail; }
        }

        interp_path = hw_interp_path_substitute(interp_path);

        uint32_t interp_id = 0;

        n = hw_read_file_cached(interp_path, g_interp_elf, g_interp_elf_cap,
                                &interp_id);
        if (n <= 0) {
            { rc = hw_exec_refuse(VIBEOS_EXEC_NO_INTERP, img.interp, "read"); goto fail; }
        }

        /* Above the program, with a gap. Both live in the low window, and an
         * interpreter placed immediately after the program would share a page
         * with it whenever the program's last page is partly used. */
        bias = (img.end_vaddr + 0xFFFFull + 0x10000ull) & ~0xFFFull;

        if (vibeos_elf_parse_ex(g_interp_elf, (uint64_t)n, bias,
                                VIBEOS_HW_LOW_USER_BASE,
                                VIBEOS_HW_USER_STACK_TOP,
                                VIBEOS_ELF_ALLOW_DYN, &interp) != VIBEOS_ELF_OK) {
            { rc = hw_exec_refuse(VIBEOS_EXEC_BAD_HEADER, img.interp, "interp_parse"); goto fail; }
        }
        if (interp.has_interp || interp.end_vaddr > VIBEOS_HW_LOW_USER_LIMIT) {
            /* An interpreter that needs an interpreter is not a chain this
             * kernel follows, and one that does not fit is refused before any
             * of it is mapped. */
            { rc = hw_exec_refuse(VIBEOS_EXEC_INTERP_CHAIN, img.interp, "interp_chain"); goto fail; }
        }
        if (hw_map_elf_image(&p->as, &ps->vmas, &interp, g_interp_elf,
                             interp_id, (uint64_t)n) != 0) {
            { rc = hw_exec_refuse(VIBEOS_EXEC_NO_MEMORY, img.interp, "map_interp"); goto fail; }
        }

        /* Start there, and tell it where it was put. */
        p->entry = interp.entry;
        p->interp_base = bias;

        /* Bracketed, because it was not and CI caught it:
         *
         *   [EXEC] interpreter EFI/BOOT/LDMUSL.SO[HW][SYS] write(ring3): SVC_EXIT
         *
         * Five serial_puts calls are five critical sections, and this one runs
         * inside execve while services are starting on other cores - so a
         * ring-3 write walked into the middle of it and the boot gate's
         * interleaved_lines check failed the run. The rule is already written
         * down: anything meant to be read together has to be written in one
         * call, or bracketed so it is one. */
        vibeos_x86_64_serial_lock();
        vibeos_x86_64_serial_puts("[EXEC] interpreter ");
        vibeos_x86_64_serial_puts(interp_path);
        vibeos_x86_64_serial_puts(" at 0x");
        vibeos_x86_64_serial_print_hex(bias);
        vibeos_x86_64_serial_puts("\n");
        vibeos_x86_64_serial_unlock();
    }
    for (i = 0; i < VIBEOS_HW_USER_STACK_PAGES; i++) {
        void *page = hw_alloc_page();
        /* Named apart from the image-loading loop's `va` above: two different
         * addresses in one function should not share a name. */
        uint64_t stack_va = VIBEOS_HW_USER_STACK_TOP - ((uint64_t)(i + 1u) * 4096ull);
        if (!page || hw_map_page(&p->as, stack_va, (uint64_t)(uintptr_t)page,
                                 PTE_PRESENT | PTE_WRITE | PTE_USER | PTE_NX) != 0) {
            { rc = hw_exec_refuse(VIBEOS_EXEC_NO_MEMORY, path, "user_stack"); goto fail; }
        }
        hw_page_put((uint64_t)(uintptr_t)page);   /* D9: the mapping owns it now */
        (void)vibeos_vma_insert(&ps->vmas, stack_va, 4096ull,
                                (vibeos_prot_t)(VIBEOS_PROT_READ |
                                                VIBEOS_PROT_WRITE |
                                                VIBEOS_PROT_USER),
                                VIBEOS_BACKING_ANON, 0, 0);
        if (i == 0u) {
            /* Still safe to write through: the address space holds the frame,
             * and the kernel reaches it by its physical address either way. */
            top_page = (uint8_t *)page;
        }
    }

    /* Fill the topmost stack page with the startup block. The page is still
     * identity-mapped for the kernel, so it is written here through its
     * physical address while the builder computes every pointer it stores in
     * terms of the user virtual address the program will see. */
    for (i = 0; i < sizeof(sd); i++) {
        ((uint8_t *)(void *)&sd)[i] = 0;
    }
    sd.argv = argv;
    sd.envp = envp;
    sd.entry = img.entry;
    /* AT_ENTRY is the *program's* entry even when the interpreter is what
     * starts: it is how the interpreter knows where to jump once it has
     * finished. p->entry is where the CPU begins, and the two differ exactly
     * when there is an interpreter. */
    sd.entry = img.entry;
    sd.interp_base = p->interp_base;
    sd.phdr_vaddr = img.phdr_vaddr;
    sd.phnum = img.phnum;
    sd.phentsize = img.phentsize;
    hw_seed_at_random(at_random);
    sd.random16 = at_random;
    p->user_sp = vibeos_elf_build_stack(top_page, 4096ull,
                                        VIBEOS_HW_USER_STACK_TOP, &sd);
    if (p->user_sp == 0) {
        /* Arguments too large for the stack we mapped. */
        { rc = hw_exec_refuse(VIBEOS_EXEC_ARGS_TOO_LARGE, path, "build_stack"); goto fail; }
    }
    ps->brk_cur = VIBEOS_HW_USER_HEAP_BASE;
    __atomic_store_n(&ps->mmap_cur, VIBEOS_HW_USER_MMAP_BASE, __ATOMIC_RELEASE);
    /* The denominator. A count of refusals with nothing to compare it against
     * says only that something went wrong somewhere. */
    vibeos_exec_stats()->loaded++;
    return 0;

fail:
    /* Phase X-P3 of docs/exec/: a failure leaves the caller running its old
     * image, and leaves nothing of the new one behind.
     *
     * Every refusal above this point happens after an address space exists and
     * most of them after pages are mapped and regions recorded. They all used
     * to return -1 and walk away: execve then reported ENOMEM to the program,
     * which carried on correctly, while the page tables, every frame they
     * mapped and every region descriptor stayed allocated with nothing holding
     * a name for them. A leak nobody could attribute, growing by one whole
     * address space per failed exec.
     *
     * Unwinding here rather than at the call sites is the point of the phase.
     * There is one function that builds a process, so there is one function
     * that can take it apart, and a refusal added later cannot forget to. */
    hw_aspace_destroy_why(&p->as, "proc_create_failed");
    vibeos_vma_clear(&ps->vmas);
    p->as.pml4 = 0;
    p->entry = 0;
    p->user_sp = 0;
    p->interp_base = 0;
    return rc;
}

uint64_t hw_proc_cr3(const hw_proc_t *p) {
    return (uint64_t)(uintptr_t)p->as.pml4;
}

/* What a process owns, shared by every thread in it.
 *
 * clone(CLONE_VM|CLONE_THREAD) used to copy hw_proc_t by value, so each thread
 * got a private copy of everything that belongs to the process. Four external
 * findings and one found while verifying them were that single copy: two
 * threads mapping at the same base, with the second mapping silently replacing
 * the first thread's pages; a sigaction in one thread its siblings never saw;
 * exit_group ending one thread; and two region-list heads into one pool of
 * freed nodes. The address space itself was always shared correctly - its
 * page-table pointer was copied, and hw_aspace_still_shared counts holders.
 *
 * Referenced, not copied. fork and exec create one; a thread takes a
 * reference; exit gives it back. The last reference takes the region list with
 * it, which is the same moment the address space goes: threads share both, and
 * a fork or an exec gets new ones of each.
 *
 * Sized for every task plus one exec in flight per core, because exec builds
 * the new process before it lets go of the old one. */
#define HW_PROCSTATE_SLOTS (VIBEOS_HW_MAX_TASKS + VIBEOS_HW_MAX_CPUS)

static hw_procstate_t g_procstate[HW_PROCSTATE_SLOTS];

/* A fresh process with refs = 1 and nothing in it, or 0 if the pool is full.
 * Dispositions start zeroed; the caller sets them, because spawn, fork and exec
 * each start from something different. */
hw_procstate_t *hw_procstate_new(void) {
    uint32_t i, sg;

    for (i = 0; i < HW_PROCSTATE_SLOTS; i++) {
        uint32_t zero = 0;
        hw_procstate_t *ps = &g_procstate[i];

        if (!__atomic_compare_exchange_n(&ps->refs, &zero, 1u, 0,
                                         __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
            continue;
        }
        __atomic_store_n(&ps->mm_busy, 0u, __ATOMIC_RELEASE);
        ps->brk_cur = 0;
        __atomic_store_n(&ps->mmap_cur, 0ull, __ATOMIC_RELEASE);
        ps->vmas.head = 0;
        ps->vmas.count = 0;
        for (sg = 0; sg < VIBEOS_HW_NSIG; sg++) {
            ps->sig_handler[sg] = 0;
            ps->sig_restorer[sg] = 0;
            ps->sig_flags[sg] = 0;
            ps->sig_mask[sg] = 0;
        }
        __atomic_store_n(&ps->exit_group_claimed, 0u, __ATOMIC_RELEASE);
        __atomic_store_n(&ps->exit_group, 0u, __ATOMIC_RELEASE);
        ps->exit_group_code = 0;
        return ps;
    }
    return 0;
}

/* Drop one reference. The last one empties the region list *before* the slot
 * is published free, so hw_procstate_new never has to touch a list that may
 * still be in the middle of being cleared.
 *
 * Clearing while refs is still 1 is safe because the only thing that raises a
 * count is clone, and clone is run by a live thread of this same process - and
 * the holder of the last reference is the thread that is exiting. */
void hw_procstate_put(hw_procstate_t *ps) {
    uint32_t r;

    if (!ps) {
        return;
    }
    for (;;) {
        r = __atomic_load_n(&ps->refs, __ATOMIC_ACQUIRE);
        if (r == 0u) {
            /* A reference given back that nobody held - the process-state
             * analogue of frames_double_put. Every put must match a new() or a
             * clone's increment, so this is always zero in a healthy boot; the
             * boot gate asserts it on the [TASKS] MUSTBEZERO line. Counted
             * rather than panicked so a whole boot can be judged at once. */
            vibeos_task_stats()->procstate_double_put++;
            return;   /* never taken, or already given back */
        }
        if (r == 1u) {
            vibeos_vma_clear(&ps->vmas);
        }
        if (__atomic_compare_exchange_n(&ps->refs, &r, r - 1u, 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
            return;
        }
    }
}

/* Defined near the spinlocks, which read it. */
uint32_t g_next_pid = 1;

int hw_task_describe(uint32_t slot, vibeos_task_desc_t *out) {
    const hw_task_t *t;
    uint32_t i;

    if (!out || slot >= (uint32_t)VIBEOS_HW_MAX_TASKS) {
        return -1;
    }
    t = &g_tasks[slot];
    out->slot = slot;
    out->generation = t->alloc_seq;
    out->state = (uint32_t)hw_task_state(t);
    out->state_name = hw_task_state_name(hw_task_state(t));
    out->pid = t->id.pid;
    out->tgid = t->id.tgid;
    out->ppid = t->id.ppid;
    out->is_user = t->id.is_user;
    out->is_thread = t->id.is_thread;
    out->on_cpu = t->on_cpu;
    out->cr3 = t->cr3;
    out->cr3_set_by = t->cr3_set_by;
    out->ready_by = t->ready_by;
    out->aspace_killed_by = t->aspace_killed_by;
    for (i = 0; i + 1u < sizeof(out->exe) && t->proc.exe_path[i]; i++) {
        out->exe[i] = t->proc.exe_path[i];
    }
    out->exe[i] = 0;
    return 0;
}

/* Everything a user program can ask for goes through here. Both fork and
 * clone(CLONE_THREAD) do, which is the point: the guard was on fork alone, and
 * a thread-based bomb - which is all pthread_create does in a loop - filled the
 * table including the reserve that was supposed to keep the machine
 * administrable. One entry point, so a third way to make a task cannot forget
 * to ask. */
int hw_task_alloc_for_user(const char *what) {
    vibeos_fork_verdict_t v = VIBEOS_FORK_OK;
    uint32_t pid = (g_current_task >= 0) ? g_tasks[g_current_task].id.tgid : 0u;
    int privileged = (g_current_task >= 0) && g_tasks[g_current_task].id.pid <= 1u;
    int idx = hw_task_alloc_guarded(1, privileged, pid, &v);

    if (idx < 0 && v != VIBEOS_FORK_OK) {
        vibeos_x86_64_serial_lock();
        vibeos_x86_64_serial_puts("[SCHED] ");
        vibeos_x86_64_serial_puts(what ? what : "spawn");
        vibeos_x86_64_serial_puts(" refused reason=");
        vibeos_x86_64_serial_puts(vibeos_fork_verdict_name(v));
        vibeos_x86_64_serial_puts(" pid=0x");
        vibeos_x86_64_serial_print_hex((uint64_t)pid);
        vibeos_x86_64_serial_puts("\n");
        vibeos_x86_64_serial_unlock();
    }
    return idx;
}

/* Control-C from the console.
 *
 * A real system sends this to the foreground process group of the controlling
 * terminal. There are no sessions or process groups here, so the target is the
 * most recently created live user task - which is the one the console is
 * talking to in every arrangement this system can currently produce. That is a
 * limitation of the model, not an approximation of the signal: the signal
 * itself is delivered exactly as any other. */
void vibeos_x86_64_console_interrupt(void) {
    int i, newest = -1;
    uint32_t delivered = 0;
    uint32_t best = 0;

    for (i = 0; i < VIBEOS_HW_MAX_TASKS; i++) {
        if (!g_tasks[i].id.is_user || hw_slot_state(i) == HW_TASK_FREE ||
            hw_slot_state(i) == HW_TASK_ZOMBIE ||
            (g_console_foreground_pgid != 0 &&
             g_tasks[i].id.pgid != g_console_foreground_pgid)) {
            continue;
        }
        if (newest < 0 || g_tasks[i].id.pid > best) {
            best = g_tasks[i].id.pid;
            newest = i;
        }
    }
    /* A stale foreground group must not make console input disappear during
     * early boot. Use the legacy newest-task fallback only when the group has
     * no live member, and keep it visible in diagnostics through the signal
     * path rather than silently dropping Ctrl-C. */
    if (newest < 0 && g_console_foreground_pgid != 0) {
        for (i = 0; i < VIBEOS_HW_MAX_TASKS; i++) {
            if (!g_tasks[i].id.is_user || hw_slot_state(i) == HW_TASK_FREE ||
                hw_slot_state(i) == HW_TASK_ZOMBIE ||
                (newest >= 0 && g_tasks[i].id.pid <= best)) {
                continue;
            }
            best = g_tasks[i].id.pid;
            newest = i;
        }
    }
    if (g_console_foreground_pgid != 0) {
        for (i = 0; i < VIBEOS_HW_MAX_TASKS; i++) {
            if (!g_tasks[i].id.is_user || hw_slot_state(i) == HW_TASK_FREE ||
                hw_slot_state(i) == HW_TASK_ZOMBIE ||
                g_tasks[i].id.pgid != g_console_foreground_pgid) {
                continue;
            }
            if (hw_signal_raise(i, VIBEOS_SIGINT) == 0) {
                delivered++;
            }
        }
    }
    if (delivered == 0 && newest >= 0) {
        (void)hw_signal_raise(newest, VIBEOS_SIGINT);
        delivered = 1;
    }
    if (delivered > 1) {
        vibeos_x86_64_serial_puts("[SIG] Ctrl-C delivered to foreground process group members=0x");
        vibeos_x86_64_serial_print_hex(delivered);
        vibeos_x86_64_serial_puts("\n");
    }
    /* Always say what happened, including "nothing". A gate could otherwise
     * only check that the byte was echoed - and it did exactly that: removing
     * the call to this function left the boot green, because an echo and a
     * signal look identical from outside. One line, one call: this is read
     * next to output from other cores. */
    vibeos_x86_64_serial_lock();
    vibeos_x86_64_serial_puts("[SIG] console interrupt: delivered=0x");
    vibeos_x86_64_serial_print_hex(delivered);
    vibeos_x86_64_serial_puts(" newest_pid=0x");
    vibeos_x86_64_serial_print_hex((uint64_t)best);
    vibeos_x86_64_serial_puts(" fg_pgid=0x");
    vibeos_x86_64_serial_print_hex((uint64_t)g_console_foreground_pgid);
    vibeos_x86_64_serial_puts("\n");
    vibeos_x86_64_serial_unlock();
}

void hw_keyboard_wake(void) {
    int i;
    hw_spin_lock_named(&g_sched_lock, __func__);
    for (i = 0; i < VIBEOS_HW_MAX_TASKS; i++) {
        if (hw_slot_state(i) == HW_TASK_BLOCKED && g_tasks[i].id.wait_input) {
            g_tasks[i].id.wait_input = 0;
            (void)hw_task_set_state(i, HW_TASK_READY, __func__);
            HW_TASK_MARK(i, ready_by, "keyboard_wake");
        }
    }
    hw_spin_unlock(&g_sched_lock);
}

static void hw_task_init_user_ctx(vibeos_x86_64_isr_frame_t *c, uint64_t entry, uint64_t sp) {
    uint32_t k;
    for (k = 0; k < (uint32_t)sizeof(*c); k++) {
        ((uint8_t *)(void *)c)[k] = 0;
    }
    c->rip = entry;
    c->cs = VIBEOS_HW_USER_CODE_SEL;
    c->rflags = 0x202;   /* reserved bit + IF */
    c->rsp = sp;         /* atop argc/argv/envp/auxv, 16-byte aligned */
    c->ss = VIBEOS_HW_USER_DATA_SEL;
}

/* Adopt the currently-executing kernel flow as a schedulable task, so the
 * kernel (and its serial CLI) is just another entry in the run queue rather
 * than something the scheduler has to "return to". */
int hw_task_adopt_kernel(void) {
    int i = hw_task_alloc();
    if (i < 0) {
        return -1;
    }
    (void)hw_task_set_state(i, HW_TASK_RUNNING, __func__);
    g_tasks[i].on_cpu = 1;          /* it is this CPU, right now */
    g_tasks[i].id.is_user = 0;
    g_tasks[i].id.pid = (uint32_t)__sync_fetch_and_add(&g_next_pid, 1u);
    g_tasks[i].id.tgid = g_tasks[i].id.pid;
    g_tasks[i].id.pgid = g_tasks[i].id.pid;
    g_tasks[i].id.sid = g_tasks[i].id.pid;
    g_tasks[i].kstack_top = hw_this_cpu()->syscall_kstack_top;
    g_tasks[i].cr3 = (uint64_t)(uintptr_t)&g_pml4[0];
    HW_TASK_MARK(i, cr3_set_by, "adopt_kernel");
    g_current_task = i;
    return i;
}

/* Idle task body: nothing to run on this core, so wait for the next interrupt
 * rather than burning the core in a spin. */
static void hw_idle_loop(void) {
    for (;;) {
        hw_sched_point("block");
        __asm__ __volatile__("sti; hlt" ::: "memory");
    }
}

/* Every CPU needs a task it can always fall back to, so the scheduler never has
 * to invent a context when the last runnable task blocks or exits. */
int hw_task_create_idle(hw_cpu_t *cpu) {
    int i = hw_task_alloc();
    if (i < 0) {
        return -1;
    }
    g_tasks[i].kstack_top = hw_alloc_kstack(&g_tasks[i].kstack_base, &g_tasks[i].kstack_pages);
    if (g_tasks[i].kstack_top == 0) {
        hw_task_release(i);
        return -1;
    }
    {
        vibeos_x86_64_isr_frame_t *c = &g_tasks[i].ctx;
        uint32_t k;
        for (k = 0; k < (uint32_t)sizeof(*c); k++) {
            ((uint8_t *)(void *)c)[k] = 0;
        }
        c->rip = (uint64_t)(uintptr_t)hw_idle_loop;
        c->cs = VIBEOS_HW_KERNEL_CS;
        c->ss = VIBEOS_HW_KERNEL_DS;
        c->rflags = 0x202;
        /* Leave slack below the top, and land on rsp % 16 == 8: the idle loop is
         * entered by iretq rather than by a call, so the ABI's post-call stack
         * alignment has to be reproduced by hand. */
        c->rsp = g_tasks[i].kstack_top - 56ull;
    }
    g_tasks[i].cr3 = (uint64_t)(uintptr_t)&g_pml4[0];
    HW_TASK_MARK(i, cr3_set_by, "create_idle");
    (void)hw_task_set_state(i, HW_TASK_READY, __func__);
    HW_TASK_MARK(i, ready_by, "create_idle");
    g_tasks[i].id.is_user = 0;
    g_tasks[i].id.is_idle = 1;
    /* An idle task is a class, not a flag the picker special-cases. */
    (void)vibeos_sched_policy_admit((uint32_t)i, VIBEOS_SCHED_IDLE, 19, 0u);
    g_tasks[i].id.pid = (uint32_t)__sync_fetch_and_add(&g_next_pid, 1u);
    g_tasks[i].id.tgid = g_tasks[i].id.pid;
    g_tasks[i].id.pgid = g_tasks[i].id.pid;
    g_tasks[i].id.sid = g_tasks[i].id.pid;
    cpu->idle_task = i;
    /* The queue needs to know the fallback too, or a CPU with nothing runnable
     * is told to keep running a task that has stopped. */
    vibeos_runq_set_idle(&g_runq, (uint32_t)cpu->index, i);
    return i;
}

/* Create a ring-3 task from an ELF image: private address space, mapped stack,
 * READY to be picked by the scheduler. */
int hw_task_spawn_user(const unsigned char *elf, uint64_t len,
                              const char *const *argv) {
    int i = hw_task_alloc();
    if (i < 0) {
        return -1;
    }
    /* An embedded image: no path, and no file behind it to map pages from. */
    /* The whole image is in memory here - a program built into the kernel,
     * not one read through the cache - so staged and len are the same. */
    g_tasks[i].ps = hw_procstate_new();
    if (!g_tasks[i].ps) {
        hw_task_release(i);
        return -1;
    }
    if (hw_proc_create(&g_tasks[i].proc, g_tasks[i].ps, elf, len, len, argv,
                       0, 0, 0u) != 0) {
        hw_procstate_put(g_tasks[i].ps);
        g_tasks[i].ps = 0;
        hw_task_release(i);
        return -1;
    }
    g_tasks[i].kstack_top = hw_alloc_kstack(&g_tasks[i].kstack_base, &g_tasks[i].kstack_pages);
    if (g_tasks[i].kstack_top == 0) {
        hw_procstate_put(g_tasks[i].ps);
        g_tasks[i].ps = 0;
        hw_task_release(i);
        return -1;
    }
    g_tasks[i].cr3 = hw_proc_cr3(&g_tasks[i].proc);
    HW_TASK_MARK(i, cr3_set_by, "spawn_user");
    hw_task_init_user_ctx(&g_tasks[i].ctx, g_tasks[i].proc.entry,
                          g_tasks[i].proc.user_sp);
    /* Task slots are recycled, so anything the previous occupant left has to
     * be cleared explicitly. A stale TLS base would point the new program at
     * a dead process's thread state. */
    g_tasks[i].fs_base = 0;
    {
        uint32_t sg;
        g_tasks[i].id.exit_signal = 0;
        g_tasks[i].id.sig_pending = 0;
        g_tasks[i].id.sig_blocked = 0;
        for (sg = 0; sg < VIBEOS_HW_NSIG; sg++) {
            g_tasks[i].ps->sig_handler[sg] = SIG_DFL_ADDR;
            g_tasks[i].ps->sig_restorer[sg] = 0;
            g_tasks[i].ps->sig_flags[sg] = 0;
            g_tasks[i].ps->sig_mask[sg] = 0;
        }
    }
    (void)hw_task_set_state(i, HW_TASK_READY, __func__);
    g_tasks[i].id.is_user = 1;
    g_tasks[i].id.pid = (uint32_t)__sync_fetch_and_add(&g_next_pid, 1u);
    g_tasks[i].id.tgid = g_tasks[i].id.pid;
    g_tasks[i].id.pgid = g_tasks[i].id.pid;
    g_tasks[i].id.sid = g_tasks[i].id.pid;
    g_tasks[i].id.signal_stopped = 0;
    return i;
}

/* exit(): retire the calling task and switch to another runnable one. Called
 * from the syscall path, so it enters the next task directly and never returns
 * to the caller. */
void hw_task_exit(uint64_t code) {
    /* Exit is the scheduling point that never comes back, so a lock held here
     * is held by a task that will not exist to release it. */
    hw_sched_point("exit");
    /* Whatever the previous task on this core left behind. Safe here: this
     * task is running on its own stack, which is not the parked one. */
    hw_drain_dead_kstack();
    hw_cpu_t *cpu = hw_this_cpu();
    int dying = cpu->current_task;
    int next, i;

    /* A task running on this core must be RUNNING, never READY - otherwise it is
     * a scheduling candidate while it is already executing. A futex_wait that was
     * woken in place breaks that: the waker set the task READY, and because the
     * task was still on_cpu it resumed here without going back through
     * hw_schedule, which is the only thing that would have set it RUNNING again.
     * It then reaches this exit as a READY task; the moment on_cpu is cleared
     * below, another core's hw_pick_next (or this one's, at 5745) returns it, and
     * two cores end up on one task while it is torn down - the four-worker crash,
     * whose signature was exactly ready_by=futex_wake on a slot being freed under
     * a running core. Reassert the invariant here, under the scheduler lock. */
    if (dying >= 0) {
        hw_spin_lock_named(&g_sched_lock, __func__);
        if (hw_slot_state(dying) == HW_TASK_READY) {
            (void)hw_task_set_state(dying, HW_TASK_RUNNING, __func__);
        }
        hw_spin_unlock(&g_sched_lock);
    }

    if (dying >= 0 && g_runtime_supervisor_ready && g_tasks[dying].id.service_id != 0) {
        vibeos_process_exit_reason_t reason = g_tasks[dying].id.exit_signal != 0
            ? VIBEOS_PROCESS_EXIT_SIGNAL : VIBEOS_PROCESS_EXIT_NORMAL;
        (void)vibeos_service_supervisor_report_exit_pid(&g_runtime_supervisor,
                                                        g_tasks[dying].id.pid,
                                                        (uint32_t)code, reason);
        g_tasks[dying].id.service_id = 0;
    }

    /* A process owns its sockets: releasing them here is what stops a task that
     * exits with connections open from leaking them for the life of the system.
     * Done before the task is retired, while its descriptor table is still
     * ours to walk. */
    if (dying >= 0) {
        int fd;
        if (g_net_up) {
            hw_spin_lock_named(&g_net_lock, __func__);
            (void)vibeos_inet_release_owner_sockets(&g_net, g_tasks[dying].id.tgid);
            hw_spin_unlock(&g_net_lock);
        }
        for (fd = 0; fd < (int)vibeos_fdtable_count(); fd++) {
            hw_fd_t *f = vibeos_fdtable_entry(&g_tasks[dying].files, (uint32_t)fd);
            if (!f->used) {
                continue;
            }
            if (f->net_sock >= 0) {
                f->net_sock = -1;
            }
            /* Exiting closes everything, and for a pipe that is not tidiness:
             * the reader at the other end is waiting for its writers to reach
             * zero, and a program that produced its output and exited without
             * closing is the normal case. Leaving the count high is how
             * ls | wc -l prints nothing and hangs. The redirections of 0-2 are
             * released the same way. */
            hw_pipe_release(f);
            f->used = 0;
        }
    }

    if (dying >= 0) {
        /* Only the exit code is recorded here. Becoming a ZOMBIE - which is
         * what lets the parent reap the slot and hand it to the next fork -
         * waits until this task's address space has actually been taken down,
         * further below.
         *
         * It used to happen here, and that window is the wedge: the parent
         * reaps between the announcement and the teardown, a fork on another
         * core takes the freed slot and builds an address space in it, and
         * then the teardown below frees the *new* tenant's page tables. The
         * new process keeps running on a CR3 whose top-level page has gone
         * back to the allocator, and the next core to install it stops on the
         * instruction that loads it, with no kernel mapped to report from. */
        hw_spin_lock_named(&g_sched_lock, __func__);
        g_tasks[dying].id.exit_code = code;
        hw_spin_unlock(&g_sched_lock);
        vibeos_x86_64_serial_lock();
        hw_log(VIBEOS_LOG_DEBUG, 40u, (uint64_t)g_tasks[dying].id.pid, code,
               "task exited (a0 = pid, a1 = code)");
        vibeos_x86_64_serial_puts("[SCHED] task pid=0x");
        vibeos_x86_64_serial_print_hex(g_tasks[dying].id.pid);
        vibeos_x86_64_serial_puts(" exited code=0x");
        vibeos_x86_64_serial_print_hex(code);
        vibeos_x86_64_serial_puts("\n");
        vibeos_x86_64_serial_unlock();
    }
    /* Tell a joiner this thread is gone.
     *
     * Before anything is scheduled, and that placement is the whole of it.
     * This used to sit after the switch to the next task, where the CR3 and
     * g_current_task both belong to somebody else - so the write went to
     * another address space and the range check refused it for the honest
     * reason that there was no current user task. The joiner was never woken
     * and pthread_join hung, with the kernel having politely declined to do
     * the one thing the joiner was waiting for.
     *
     * The word lives in the dying task's address space, so it can only be
     * written while that space is still the one loaded. */
    if (dying >= 0 && g_tasks[dying].id.clear_child_tid != 0u) {
        uint64_t addr = g_tasks[dying].id.clear_child_tid;
        uint32_t why = HW_RANGE_OK;

        uint32_t zero = 0;
        if (hw_user_range_why(addr, 4u, 1, &why) &&
            vibeos_uaccess_copy((void *)(uintptr_t)addr, &zero, 4u) == 0) {
            /* The dying thread's process, which is still attached here: this
             * block runs before hw_procstate_put further down. Keep it that
             * way. With the reference already given back, ps would be 0, no
             * waiter would match, and every pthread_join on this thread would
             * sleep forever - with nothing failing to compile. */
            hw_futex_wake(g_tasks[dying].ps, addr, 0x7FFFFFFF);
        } else {
            hw_log(VIBEOS_LOG_WARN, 28u, addr,
                   (uint64_t)why | ((uint64_t)g_tasks[dying].id.pid << 8),
                   "exit: join word not writable, nobody will be woken "
                   "(a1 = reason | tid<<8)");
        }
        g_tasks[dying].id.clear_child_tid = 0;
    }

    /* Off, and not turned back on by anything below: the iretq at the end of
     * vibeos_x86_64_task_enter restores the flags from next's own saved frame.
     * Everything between - the teardown, the locks it takes and releases -
     * saves and restores the interrupt state it found, which is now off. */
    __asm__ __volatile__("cli");
    /* From here to vibeos_x86_64_task_enter this core has made `next` current
     * while still running the dying task's code. A timer interrupt in that
     * stretch makes hw_schedule save this frame - the dying task's, on a kernel
     * stack already parked for release - as next's context, and hand next to
     * another core. The interrupt flag is not guaranteed off here: a task
     * killed out of a wait that did `sti; hlt` arrives with it set. Counted,
     * and the boot gate asserts the count is zero. */
    {
        uint64_t fl;
        __asm__ __volatile__("pushfq; popq %0" : "=r"(fl) : : "memory");
        if ((fl & 0x200ull) != 0u) {
            vibeos_task_stats()->exit_switch_irq_on++;
        }
    }
    hw_spin_lock_named(&g_sched_lock, __func__);
    next = hw_pick_next(cpu);
    if (next < 0) {
        next = cpu->idle_task;
    }
    if (next < 0) {
        hw_spin_unlock(&g_sched_lock);
        vibeos_x86_64_serial_puts("[SCHED] no runnable task; halting\n");
        for (;;) {
            __asm__ __volatile__("hlt");
        }
    }
    if (dying >= 0) {
        g_tasks[dying].on_cpu = 0;
    }
    cpu->current_task = next;
    (void)hw_task_set_state(next, HW_TASK_RUNNING, __func__);
    g_tasks[next].on_cpu = 1;
    vibeos_account_switch(hw_this_cpu()->index, next, vibeos_task_ready_at((uint32_t)next));
    hw_this_cpu()->slice_left = hw_slice_for(next);
    hw_spin_unlock(&g_sched_lock);
    /* No save: the outgoing task is dying and its registers go with it. The
     * restore is not optional though - without it the next task inherits
     * whatever the dying one left, which is the defect this whole mechanism
     * exists to stop, arriving by a different door. */
    hw_fpu_restore(g_tasks[next].fpu);
    hw_task_load_cpu_state(next);
    /* Now on the next task's CR3; the dying user address space is still
     * reachable through the shared kernel identity map, so free it. */
    if (dying >= 0 && g_tasks[dying].id.is_user) {
        int last;

        /* The order this subsystem learned the hard way, declared rather than
         * remembered: the address space goes before the slot is announced,
         * because a parent may reap the instant it sees a zombie. A step taken
         * out of order is counted and the boot gate asserts the count. */
        vibeos_teardown_reset((uint32_t)dying);

        hw_spin_lock_named(&g_sched_lock, __func__);
        last = !hw_aspace_still_shared(dying);
        if (!last) {
            /* A sibling is still running in here. Give up the reference
             * without freeing anything - and clear the pointer, so nothing
             * later mistakes this task for an owner. */
            g_tasks[dying].proc.as.pml4 = 0;
        }
        hw_spin_unlock(&g_sched_lock);

        if (!last) {
            /* Shared with a sibling: the tables stay, but this task has still
             * finished with them - the same point in the order. */
            (void)vibeos_teardown_step((uint32_t)dying, VIBEOS_TEARDOWN_ASPACE);
            HW_TASK_MARK(dying, aspace_killed_by, "thread_exit_kept_shared");
        } else {
        vibeos_task_stats()->exited++;
        HW_TASK_MARK(dying, aspace_killed_by, "task_exit");
        hw_aspace_destroy(&g_tasks[dying].proc.as);
        (void)vibeos_teardown_step((uint32_t)dying, VIBEOS_TEARDOWN_ASPACE);
        }
        /* The regions go with the last reference to the process, not with
         * this task. That used to be written as "a sibling thread that keeps
         * the tables keeps the list too", which was true only because every
         * sibling held its own private copy of the list head. */
        hw_procstate_put(g_tasks[dying].ps);
        g_tasks[dying].ps = 0;
        /* The kernel stack is parked on this core, not left for the parent.
         *
         * We are still executing on it right now, and the parent may reap this
         * slot the instant it sees the zombie - so a reaper that freed the
         * stack would be freeing the one under our feet. See
         * hw_cpu_t::dead_kstack_base. */
    }

    /* Now, and not before: the address space is gone, so a parent that reaps
     * this slot the instant it sees the zombie cannot have its child's page
     * tables pulled out from under it. cr3 goes too, so nothing can be
     * scheduled on a table that has been freed - the guard in
     * hw_task_load_cpu_state would catch that, but not being wrong beats
     * being told. */
    if (dying >= 0) {
        hw_spin_lock_named(&g_sched_lock, __func__);
        g_tasks[dying].cr3 = 0;

        /* Park the stack before the slot is published in any form. Taken from
         * the task under the same lock that publishes the state, so a reaper
         * that sees the zombie can never also see a stack to free. */
        cpu->dead_kstack_base = g_tasks[dying].kstack_base;
        cpu->dead_kstack_pages = g_tasks[dying].kstack_pages;
        g_tasks[dying].kstack_base = 0;
        g_tasks[dying].kstack_pages = 0;

        /* A thread has no reaper, so it must not become a zombie.
         *
         * waitpid matches on ppid, and a thread inherits its *creator's*
         * parent rather than becoming its child - so the thing that joins it
         * cannot wait for it, and nothing else is looking. Left as a zombie it
         * holds its slot for the rest of the boot.
         *
         * That is exactly what happened. A program creating and joining eight
         * threads leaked eight slots, and the failure surfaced nowhere near the
         * threads: the shell's next fork was refused, on a machine with plenty
         * of memory, because the task table had quietly filled with threads
         * that had already finished. The fork guard is what made it visible -
         * before that the slots were simply gone.
         *
         * Joining is already synchronised without a zombie: exit clears the
         * word the joiner sleeps on and wakes the futex, a few lines above.
         * The zombie was carrying an exit status nobody can ask for.
         *
         * Released here rather than at exit because this is after the context
         * switch: the stack it was running on is no longer in use. */
        if (g_tasks[dying].id.is_thread) {
            /* Through ZOMBIE, not straight to FREE. The transition table
             * refuses running->free outright, and it was right to: the first
             * version of this called hw_task_release directly, the table logged
             * ILLEGAL, and the slot stayed RUNNING for the rest of the boot - a
             * worse leak than the one being fixed, and invisible except for
             * that one line. Reap it immediately afterwards, since this is the
             * reaper nothing else will be. */
            (void)hw_task_set_state(dying, HW_TASK_ZOMBIE, __func__);
            hw_task_release(dying);
        } else {
        (void)hw_task_set_state(dying, HW_TASK_ZOMBIE, __func__);
        for (i = 0; i < VIBEOS_HW_MAX_TASKS; i++) {
            if (hw_slot_state(i) == HW_TASK_BLOCKED &&
                g_tasks[i].id.pid == g_tasks[dying].id.ppid) {
                (void)hw_task_set_state(i, HW_TASK_READY, __func__);
                HW_TASK_MARK(i, ready_by, "parent_woken_by_child_exit");
            }
        }
        }
        hw_spin_unlock(&g_sched_lock);
    }
    hw_ctx_check(next, "schedule");
    vibeos_x86_64_task_enter(&g_tasks[next].ctx); /* does not return */
}

/* Let go of one end of a pipe. The pipe itself lives until both ends are
 * gone, because a reader may still have data to drain after every writer has
 * closed. */
void hw_pipe_release(hw_fd_t *f) {
    if (!f || f->pipe < 0) {
        return;
    }
    vibeos_pipe_end_release(f);
    /* Somebody may be waiting for the data or the space that just became
     * possible - or for the end of file that just became true. */
    hw_keyboard_wake();
}

/* What happens to a signal nobody handles.
 *
 * Getting this table wrong is not a small error: an ignored SIGCHLD that kills
 * the process, or a SIGTERM that is quietly dropped, both look like the
 * program misbehaving rather than the kernel. */
int hw_signal_default_kills(uint32_t sig) {
    switch (sig) {
        case VIBEOS_SIGCHLD:
        case VIBEOS_SIGCONT:
        case VIBEOS_SIGWINCH:
            return 0;   /* ignored by default */
        default:
            return 1;
    }
}

/* Should a task waiting in the kernel stop waiting?
 *
 * No wait here asked this, and a signal is only delivered on the way back to
 * user space - so a task blocked in a futex, on a pipe, in waitpid or in a
 * console read was moved to READY by hw_signal_raise, ran, and went straight
 * back to sleep. kill -9 could not end a process waiting on a pipe, and
 * exit_group could not end a sibling waiting on anything but the exiting
 * thread's own join. The Ctrl-C case file had already recorded that delivery
 * to a blocked process had never been observed by any test.
 *
 * A signal interrupts a wait only if acting on it does something: SIGKILL and
 * SIGSTOP always, and any other unblocked one unless it would be discarded -
 * ignored, or left at a default that ignores. A wait interrupted by a signal
 * that is then thrown away is a spurious EINTR, and a caller that ignores its
 * wait4 result - the native shell does - would stop waiting for its child.
 * SIGCHLD is never raised today, which is exactly why that case must not be
 * left to chance. */
int hw_signal_interrupts(int task) {
    const hw_task_t *t;
    const hw_procstate_t *ps;
    uint64_t ready;
    uint32_t sig;

    if (task < 0 || task >= VIBEOS_HW_MAX_TASKS) {
        return 0;
    }
    t = &g_tasks[task];
    ready = (t->id.sig_pending & ~t->id.sig_blocked) |
            (t->id.sig_pending & ((1ull << VIBEOS_SIGKILL) | (1ull << VIBEOS_SIGSTOP)));
    if (ready == 0u) {
        return 0;
    }
    ps = t->ps;
    for (sig = 1; sig < VIBEOS_HW_NSIG; sig++) {
        uint64_t handler;

        if ((ready & (1ull << sig)) == 0u) {
            continue;
        }
        if (sig == VIBEOS_SIGKILL || sig == VIBEOS_SIGSTOP) {
            return 1;
        }
        handler = ps ? ps->sig_handler[sig] : SIG_DFL_ADDR;
        if (handler == SIG_IGN_ADDR) {
            continue;
        }
        if (handler == SIG_DFL_ADDR && !hw_signal_default_kills(sig)) {
            continue;
        }
        return 1;
    }
    return 0;
}

/* Raise a signal against a task. Does not deliver it: delivery happens on the
 * way back to user space, on that task's own stack, which is the only place a
 * signal frame can safely be built. Raising can happen from an interrupt, from
 * another CPU, or from the task itself, and none of those own that stack. */
int hw_signal_raise(int task_index, uint32_t sig) {
    if (task_index < 0 || task_index >= VIBEOS_HW_MAX_TASKS || sig == 0u || sig > VIBEOS_HW_SIG_MAX) {
        return -1;
    }
    if (!g_tasks[task_index].id.is_user || hw_slot_state(task_index) == HW_TASK_FREE ||
        g_tasks[task_index].ps == 0) {
        return -1;   /* no process left to hold a disposition: it is exiting */
    }
    if (sig == VIBEOS_SIGCONT && g_tasks[task_index].id.signal_stopped) {
        g_tasks[task_index].id.signal_stopped = 0;
        (void)hw_task_set_state(task_index, HW_TASK_READY, __func__);
        HW_TASK_MARK(task_index, ready_by, "sigcont");
    }
    /* SIGKILL and SIGSTOP cannot be caught or blocked. Honouring a handler for
     * them would make a process unkillable. */
    if (sig != VIBEOS_SIGKILL && sig != VIBEOS_SIGSTOP &&
        g_tasks[task_index].ps->sig_handler[sig] == SIG_IGN_ADDR) {
        return 0;   /* explicitly ignored: raised and discarded, as Linux does */
    }
    __sync_fetch_and_or(&g_tasks[task_index].id.sig_pending, 1ull << sig);
    /* A task asleep in read() has to wake up to notice. */
    if (hw_slot_state(task_index) == HW_TASK_BLOCKED) {
        g_tasks[task_index].id.wait_input = 0;
        (void)hw_task_set_state(task_index, HW_TASK_READY, __func__);
        HW_TASK_MARK(task_index, ready_by, "signal_wake");
    }
    return 0;
}

/* exit_group: the whole process ends, and the parent sees `code`.
 *
 * It used to share a case with exit and end only the calling thread, so a
 * worker that called exit() left main running and the parent reaped whatever
 * main returned later - THREADS_C5_EXIT_GROUP saw 7 instead of 42.
 *
 * The siblings are ended with SIGKILL, which also wakes one that is blocked.
 * The group's code is recorded first and delivery consults it, because waitpid
 * builds the status from the leader, and a leader ended by that SIGKILL would
 * otherwise report "killed by 9" - which is what an exit_group built from
 * SIGKILL alone produces, and not what Linux reports.
 *
 * The first caller's code wins, as on Linux. The loop runs under g_sched_lock:
 * it matches slots on ps, and both slots and hw_procstate_t are reused, so an
 * unlocked match can be a new tenant (H-007). An earlier version of this comment
 * said the lock could not be held because hw_signal_raise reaches
 * hw_task_set_state; that function takes no lock, and hw_schedule calls it
 * under this one. */
void hw_task_exit_group(uint64_t code) {
    int me = g_current_task;
    hw_procstate_t *ps;
    uint32_t zero = 0;
    int i;

    if (me < 0 || (ps = g_tasks[me].ps) == 0) {
        hw_task_exit(code);
        return;
    }
    if (__atomic_compare_exchange_n(&ps->exit_group_claimed, &zero, 1u, 0,
                                    __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
        ps->exit_group_code = code;
        __atomic_store_n(&ps->exit_group, 1u, __ATOMIC_RELEASE);
    } else {
        while (__atomic_load_n(&ps->exit_group, __ATOMIC_ACQUIRE) == 0u) {
            __asm__ __volatile__("pause" ::: "memory");
        }
        code = ps->exit_group_code;
    }
    hw_spin_lock_named(&g_sched_lock, __func__);
    for (i = 0; i < VIBEOS_HW_MAX_TASKS; i++) {
        if (i != me && g_tasks[i].ps == ps && hw_slot_state(i) != HW_TASK_FREE &&
            hw_slot_state(i) != HW_TASK_RESERVED &&
            hw_slot_state(i) != HW_TASK_ZOMBIE) {
            (void)hw_signal_raise(i, VIBEOS_SIGKILL);
        }
    }
    hw_spin_unlock(&g_sched_lock);
    g_tasks[me].id.exit_signal = 0;
    hw_task_exit(code);
}

/* Find a task by thread-group id: kill(pid) names a process, and any of its
 * threads will do as the place to record a pending signal. Threads are looked
 * up by hw_task_by_tid instead, which matches the thread id. */
/* An index, not a reference - H-007, verified. A slot is reused the moment
 * its task is reaped, so an index a syscall found a few instructions ago can
 * name a different task by the time it is used: kill(old_pid, SIGKILL) then
 * lands on whoever forked into the slot. pids are never reused, so what makes
 * the index safe is holding g_sched_lock from the lookup to the use - every
 * allocation and every publish to FREE is done under it. Callers that act on
 * the result take it; hw_signal_raise is safe under it, since
 * hw_task_set_state takes no lock of its own and hw_schedule already calls it
 * there.
 *
 * RESERVED is skipped for the second half of the same finding: allocation
 * leaves the previous tenant's pid, tgid, sid and pgid in place until fork or
 * clone rewrites them, so a slot being built answers to a dead process's id. */
int hw_task_by_pid(uint32_t pid) {
    int i;
    for (i = 0; i < VIBEOS_HW_MAX_TASKS; i++) {
        if (g_tasks[i].id.is_user && hw_slot_state(i) != HW_TASK_FREE &&
            hw_slot_state(i) != HW_TASK_RESERVED &&
            g_tasks[i].id.tgid == pid) {
            return i;
        }
    }
    return -1;
}

/* Find a task by thread id.
 *
 * This used to be `return hw_task_by_pid(tid);`, under a comment saying there
 * was one thread per process so tid and pid were equal. That stopped being true
 * when clone(CLONE_THREAD) started giving each thread its own id, and the lookup
 * went on matching thread-group ids: a thread that was not the leader matched
 * nothing and tkill answered ESRCH for a thread that existed, while the leader's
 * own id matched whichever member of the group sat in the lowest slot - so
 * tgkill's identity check could refuse even the leader. raise() in a C library
 * is tkill(gettid()), so it failed in every thread but the first.
 *
 * The comment described the kernel at the moment it was written, and nothing
 * made it wrong except the kernel changing around it. */
int hw_task_by_tid(uint32_t tid) {
    int i;
    for (i = 0; i < VIBEOS_HW_MAX_TASKS; i++) {
        if (g_tasks[i].id.is_user && hw_slot_state(i) != HW_TASK_FREE &&
            hw_slot_state(i) != HW_TASK_RESERVED &&
            g_tasks[i].id.pid == tid) {
            return i;
        }
    }
    return -1;
}

hw_futex_waiter_t g_futex_waiters[VIBEOS_HW_MAX_FUTEX_WAITERS];

hw_lock_t g_futex_lock;

/* Wake up to `count` waiters on `addr`. Returns how many were woken, which is
 * what the caller is told: a library uses it to decide whether it needs to
 * wake anybody else. */
long hw_futex_wake(const hw_procstate_t *ps, uint64_t addr,
                          uint32_t count) {
    long woke = 0;
    uint32_t i;

    if (addr == 0u) {
        return 0;
    }
    hw_spin_lock_named(&g_futex_lock, __func__);
    for (i = 0; i < VIBEOS_HW_MAX_FUTEX_WAITERS && (uint32_t)woke < count; i++) {
        if (!g_futex_waiters[i].used || g_futex_waiters[i].addr != addr ||
            g_futex_waiters[i].ps != ps) {
            continue;
        }
        /* The slot must still hold the very task that enqueued: a reaped-and-
         * reused slot is a different tenant, and waking it by a stale entry is
         * the ABA that scheduled an exited thread onto a stack being freed
         * (H-007, here in the futex table). alloc_seq is stable for the life of
         * a tenancy and changes on every reuse. */
        if (g_tasks[g_futex_waiters[i].task].alloc_seq != g_futex_waiters[i].seq) {
            continue;   /* stale entry; the enqueuer is long gone */
        }
        g_futex_waiters[i].addr = 0;   /* no second wake for this waiter */
        g_futex_waiters[i].woken = 1;
        hw_spin_lock_named(&g_sched_lock, __func__);
        if (hw_slot_state(g_futex_waiters[i].task) == HW_TASK_BLOCKED &&
            g_tasks[g_futex_waiters[i].task].alloc_seq == g_futex_waiters[i].seq) {
            (void)hw_task_set_state(g_futex_waiters[i].task, HW_TASK_READY, __func__);
            HW_TASK_MARK(g_futex_waiters[i].task, ready_by, "futex_wake");
        }
        hw_spin_unlock(&g_sched_lock);
        woke++;
    }
    hw_spin_unlock(&g_futex_lock);
    hw_log(VIBEOS_LOG_DEBUG, 20u, addr, (uint64_t)woke, "futex wake");
    return woke;
}

void hw_fault_kill_current_user(const vibeos_x86_64_isr_frame_t *frame,
                                       uint64_t fault_address) {
    uint64_t vector = frame->vector;
    uint32_t sig;

    if (g_current_task < 0 || !g_tasks[g_current_task].id.is_user) {
        return;   /* nothing to kill: the caller panics instead */
    }
    switch (vector) {
        case 0u:  sig = VIBEOS_SIGFPE;  break;   /* #DE divide error      */
        case 6u:  sig = VIBEOS_SIGILL;  break;   /* #UD invalid opcode    */
        case 13u: sig = VIBEOS_SIGSEGV; break;   /* #GP general protection*/
        case 14u: sig = VIBEOS_SIGSEGV; break;   /* #PF page fault        */
        default:  sig = VIBEOS_SIGSEGV; break;
    }
    /* One call, one line: this is read next to the trap dump above it, and a
     * diagnostic split across two writes came back interleaved from two cores
     * once already and read as a contradiction. */
    /* A ring-3 instruction fetch on a page that is not present: the page the
     * process was executing from has gone. Nothing else is dumped, so a healthy
     * boot pays nothing. */
    if (vector == 14u && (frame->error_code & 0x01u) == 0u &&
        (frame->error_code & 0x04u) != 0u && (frame->error_code & 0x10u) != 0u) {
        hw_dump_vanished(fault_address);
    }
    hw_log(VIBEOS_LOG_WARN, 29u, vector, (uint64_t)sig,
           "ring-3 fault: killing the task (a0 = vector, a1 = signal)");
    vibeos_x86_64_serial_lock();
    vibeos_x86_64_serial_puts("[HW][TRAP] ring3 fault: killing task, not the machine, cpu=0x");
    vibeos_x86_64_serial_print_hex((uint64_t)vibeos_x86_64_cpu_id());
    vibeos_x86_64_serial_puts(" vector=0x");
    vibeos_x86_64_serial_print_hex(vector);
    vibeos_x86_64_serial_puts(" sig=0x");
    vibeos_x86_64_serial_print_hex(sig);
    vibeos_x86_64_serial_puts("\n");
    vibeos_x86_64_serial_unlock();

    /* Take the whole picture before anything is torn down. Filled here, where a
     * trap frame means something, and kept by kernel/diag/crash.c, which takes
     * the copy under its own lock. */
    {
        vibeos_crash_t local;
        vibeos_crash_t *rec = &local;
        const hw_task_t *t = &g_tasks[g_current_task];
        uint32_t i;

        rec->pid = t->id.pid;
        rec->sig = sig;
        rec->vector = vector;
        rec->error_code = frame->error_code;
        rec->fault_addr = fault_address;
        rec->nregs = 0;
#define HW_CRASH_REG(r) do { rec->regs[rec->nregs].name = #r; \
        rec->regs[rec->nregs].value = frame->r; rec->nregs++; } while (0)
        HW_CRASH_REG(rip); HW_CRASH_REG(rsp); HW_CRASH_REG(rbp); HW_CRASH_REG(rflags);
        HW_CRASH_REG(rax); HW_CRASH_REG(rbx); HW_CRASH_REG(rcx); HW_CRASH_REG(rdx);
        HW_CRASH_REG(rsi); HW_CRASH_REG(rdi); HW_CRASH_REG(r8);  HW_CRASH_REG(r9);
#undef HW_CRASH_REG
        for (i = 0; i < sizeof(rec->exe) - 1u && t->proc.exe_path[i]; i++) {
            rec->exe[i] = t->proc.exe_path[i];
        }
        rec->exe[i] = 0;
        /* The stack is user memory and the fault may well have been about
         * exactly that, so every word is range-checked. Stopping at the first
         * unreadable one is the honest thing: a dump that invents the rest is
         * worse than a short dump. */
        rec->stack_words = 0;
        for (i = 0; i < VIBEOS_CRASH_STACK_WORDS; i++) {
            uint64_t addr = frame->rsp + (uint64_t)i * 8ull;
            if (!linux_user_ok(addr, 8u, 0)) {
                break;
            }
            /* Fault-safe on purpose: this runs inside the trap handler, so a
             * direct deref that faulted would nest a fault and panic. */
            if (vibeos_uaccess_copy(&rec->stack[i], (const void *)(uintptr_t)addr,
                                    8u) != 0) {
                break;
            }
            rec->stack_words++;
        }
        (void)vibeos_crash_record(rec);

        vibeos_x86_64_serial_lock();
        vibeos_x86_64_serial_puts("[CRASH] recorded pid=0x");
        vibeos_x86_64_serial_print_hex((uint64_t)rec->pid);
        vibeos_x86_64_serial_puts(" sig=0x");
        vibeos_x86_64_serial_print_hex((uint64_t)sig);
        vibeos_x86_64_serial_puts(" exe=");
        vibeos_x86_64_serial_puts(rec->exe[0] ? rec->exe : "(unknown)");
        vibeos_x86_64_serial_puts(" - `crash` on the console prints it in full\n");

        /* Is this task standing on a page that was given away underneath it?
         *
         * A crash whose registers and stack are all 0xDEAD0000DEAD0000 is not
         * a program bug: that is the pattern this kernel writes into a page as
         * it frees one. The task returned into poison because the memory it
         * was running on had been handed back to the allocator while it was
         * still there.
         *
         * That has now happened three times and been diagnosed from the far
         * end each time. The page still carries the name of whoever released
         * it, in the word after the freelist link, so read it out and say it
         * here - at the crash, where somebody is already looking. */
        if (rec->stack_words > 0 && rec->stack[0] == HW_PAGE_POISON) {
            uint64_t page = frame->rsp & ~0xFFFull;
            const char *freed_by = 0;

            if (linux_user_ok(page, 16u, 0)) {
                freed_by = (const char *)(uintptr_t)((const uint64_t *)(uintptr_t)page)[1];
            }
            vibeos_x86_64_serial_puts("[CRASH] this task's stack is the free-page "
                                      "poison: its memory was reclaimed while it "
                                      "was still running on it");
            if (freed_by &&
                (uint64_t)(uintptr_t)freed_by > 0x100000ull &&
                (uint64_t)(uintptr_t)freed_by < VIBEOS_HW_IDENTITY_LIMIT) {
                vibeos_x86_64_serial_puts(", last freed by ");
                vibeos_x86_64_serial_puts(freed_by);
            }
            vibeos_x86_64_serial_puts("\n");
        }
        vibeos_x86_64_serial_unlock();
    }

    g_tasks[g_current_task].id.exit_signal = sig;
    hw_task_exit(128ull + sig);   /* switches away; does not return */
}

static hw_lock_t g_crash_lock;

static void hw_crash_lock(void) {
    hw_spin_lock_named(&g_crash_lock, "vibeos_crash");
}

static void hw_crash_unlock(void) {
    hw_spin_unlock(&g_crash_lock);
}

void hw_crash_init(void) {
    vibeos_crash_set_lock(hw_crash_lock, hw_crash_unlock);
    vibeos_crash_reset();
}

/* ---- asking a task who it is ------------------------------------------------------
 *
 * The context switch names a task's identity where it decides (is it idle, is it a
 * user task) and nowhere else in arch_hw.c; the diagnostics and the address-space code
 * ask through these, so what a "pid" is stays this file's business. */
uint32_t hw_task_pid_of(const hw_task_t *t) {
    return t->id.pid;
}

uint32_t hw_task_pgid_of(const hw_task_t *t) {
    return t->id.pgid;
}

int hw_task_is_user_of(const hw_task_t *t) {
    return t->id.is_user;
}

void hw_task_set_service(int slot, uint32_t service_id) {
    g_tasks[slot].id.service_id = service_id;
}

/* Claim a free descriptor slot in the calling process. */
int hw_fd_alloc(hw_task_t *t) {
    return vibeos_fdtable_claim(&t->files);
}
