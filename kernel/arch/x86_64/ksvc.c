/* Kernel services (include/vibeos/ksvc.h) as this machine provides them.
 *
 * Everything a syscall personality asks of the kernel, answered from the task
 * table, the locks, the page tables and the trap frame that live here. Most of
 * it is one line over a function that already existed; the header is the point,
 * not this file. What is more than a line is what used to sit inside the Linux
 * handlers and is really about the machine: the per-process mm lock's interrupt
 * window, the leaf bits of a fresh page, reading an entry for pageinfo, a fork's
 * or a thread's saved registers, an exec's entry, a signal frame, the TLS MSR.
 *
 * docs/abi/phases.md, A2. */

#include "vibeos/sched_policy.h"
#include "arch_hw_internal.h"

_Static_assert(VIBEOS_HW_MAX_TASKS <= LINUX_FUTEX_TABLE,
               "the futex table holds one waiter per task");

static vibeos_x86_64_isr_frame_t *hw_regs(ks_regs_t *r) {
    return (vibeos_x86_64_isr_frame_t *)(void *)r;
}

static const vibeos_x86_64_isr_frame_t *hw_regs_c(const ks_regs_t *r) {
    return (const vibeos_x86_64_isr_frame_t *)(const void *)r;
}

/* ---- the entry --------------------------------------------------------------
 *
 * The Linux ABI on x86-64: nr in rax, arguments in rdi, rsi, rdx, r10, r8, r9.
 * Reached from both the native `syscall` trampoline and the int 0x80 gate. */
long vibeos_x86_64_linux_syscall(vibeos_x86_64_isr_frame_t *frame,
                                 uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3) {
    uint64_t a[6];

    a[0] = a1;
    a[1] = a2;
    a[2] = a3;
    a[3] = frame->r10;
    a[4] = frame->r8;
    a[5] = frame->r9;
    return linux_syscall((struct ks_regs *)(void *)frame, nr, a);
}

/* ---- tasks ------------------------------------------------------------------ */

int ks_current(void) { return hw_current_task(); }
uint32_t ks_slots(void) { return (uint32_t)VIBEOS_HW_MAX_TASKS; }
vibeos_task_t *ks_id(int slot) { return &g_tasks[slot].id; }
vibeos_procstate_t *ks_ps(int slot) { return g_tasks[slot].ps; }
void ks_set_ps(int slot, vibeos_procstate_t *ps) { g_tasks[slot].ps = ps; }
vibeos_image_t *ks_image(int slot) { return &g_tasks[slot].proc; }
uint32_t ks_seq(int slot) { return g_tasks[slot].alloc_seq; }
const vibeos_abi_t *ks_abi(int slot) { return g_tasks[slot].abi; }
uint64_t ks_cr3(int slot) { return g_tasks[slot].cr3; }
void ks_mark_ready(int slot, const char *where) { HW_TASK_MARK(slot, ready_by, where); }
void ks_mark_aspace(int slot, const char *where) { HW_TASK_MARK(slot, aspace_killed_by, where); }

vibeos_lock_t *ks_sched_lock(void) { return &g_sched_lock; }
int ks_set_state(int slot, vibeos_task_state_t to, const char *why) {
    return hw_task_set_state(slot, to, why);
}
int ks_task_by_pid(uint32_t pid) { return hw_task_by_pid(pid); }
int ks_task_by_tid(uint32_t tid) { return hw_task_by_tid(tid); }
int ks_task_alloc_for_user(const char *what) { return hw_task_alloc_for_user(what); }
void ks_task_release(int slot) { hw_task_release(slot); }
uint32_t ks_next_pid(void) { return (uint32_t)__sync_fetch_and_add(&g_next_pid, 1u); }
void ks_task_exit(uint64_t code) { hw_task_exit(code); }
void ks_task_exit_group(uint64_t code) { hw_task_exit_group(code); }

/* ---- locks and waiting ---------------------------------------------------------- */

void ks_lock(vibeos_lock_t *l, const char *fn) { hw_spin_lock_named(l, fn); }
void ks_unlock(vibeos_lock_t *l) { hw_spin_unlock(l); }
void ks_lock_preemptible(vibeos_lock_t *l) { hw_spin_lock_preemptible(l); }
void ks_unlock_preemptible(vibeos_lock_t *l) { hw_spin_unlock_preemptible(l); }
void ks_irq_off(void) { __asm__ __volatile__("cli"); }
void ks_irq_on(void) { __asm__ __volatile__("sti"); }
void ks_idle(void) { __asm__ __volatile__("sti; hlt" ::: "memory"); }
void ks_block_point(void) {
    int cur = hw_this_cpu()->current_task;

    /* Waiting, for /proc's S; the dispatcher clears it when the call returns. */
    if (cur >= 0 && cur < VIBEOS_HW_MAX_TASKS) {
        g_tasks[cur].id.sleeping = 1;
    }
    hw_sched_point("block");
    __asm__ __volatile__("sti; hlt" ::: "memory");
}
void ks_wait_tick(void) { ks_block_point(); }
void ks_wake_waiters(void) { hw_keyboard_wake(); }
int ks_signal_interrupts(int slot) { return hw_signal_interrupts(slot); }
int ks_signal_raise(int slot, uint32_t sig) { return hw_signal_raise(slot, sig); }
int ks_task_nice(int slot) { return vibeos_sched_policy_nice((uint32_t)slot); }
int ks_task_set_nice(int slot, int nice) { return vibeos_sched_policy_set_nice((uint32_t)slot, nice); }
int ks_signal_send(int slot, uint32_t sig, const vibeos_siginfo_t *info) { return hw_signal_send(slot, sig, info); }
int ks_signal_take(int slot, uint32_t sig, vibeos_siginfo_t *out) { return hw_signal_take(slot, sig, out); }
int ks_signal_default_kills(uint32_t sig) { return hw_signal_default_kills(sig); }

/* ---- time ------------------------------------------------------------------------- */

uint64_t ks_ticks(void) { return g_timer_ticks; }
uint32_t ks_hz(void) { return VIBEOS_HW_TIMER_HZ; }

/* ---- user memory -------------------------------------------------------------------- */

int ks_user_ok(uint64_t base, uint64_t len, int write) { return hw_user_range_ok(base, len, write); }
int ks_user_addr_ok(uint64_t va) { return hw_user_addr_ok(va); }
int ks_user_range_why(uint64_t va, uint64_t len, int write, uint32_t *why) {
    return hw_user_range_why(va, len, write, why);
}
int ks_copy_user_string(uint64_t uptr, char *dst, int max) { return hw_copy_user_string(uptr, dst, max); }

#define HW_RANGE_LEVEL1      4u
#define HW_RANGE_LEVEL2      5u

/* The codes as words, because a number in a log has to be looked up and a
 * reason that has to be looked up is one people stop reading. Which level of
 * the walk failed is the whole diagnostic here: an absent PML4 entry and a leaf
 * that is present but not user-accessible are completely different bugs. */
const char *ks_user_range_why_name(uint32_t why) {
    switch (why) {
        case HW_RANGE_OK:       return "ok";
        case HW_RANGE_NO_TASK:  return "no_current_user_task";
        case HW_RANGE_WRAP:     return "address_wrapped";
        case HW_RANGE_LEVEL0:   return "pml4_absent_or_not_user";
        case HW_RANGE_LEVEL1:   return "pdpt_absent_or_not_user";
        case HW_RANGE_LEVEL2:   return "pd_absent_or_not_user";
        case HW_RANGE_LEAF:     return "leaf_absent_or_not_user";
        case HW_RANGE_READONLY: return "leaf_not_writable";
        default:                return "?";
    }
}

/* ---- the address space ---------------------------------------------------------------
 *
 * One address-space mutation at a time per process. A compare-exchange flag,
 * not a spinlock: the work under it can be many pages, and a spinlock would
 * hold them all with the timer off. Every thread of a process shares the ps,
 * so this serialises brk against fork's read of the same tables and list -
 * and mmap, munmap and mprotect as each is converted to take it.
 *
 * Bounded, because the failure mode of a lock is a hang, and a hang here is a
 * silent machine. A holder that never releases - a return path that forgot to
 * unlock - becomes a named panic instead. The bound is the shootdown's, chosen
 * for the same reason: far beyond any honest wait between two cores. */
void ks_mm_lock(vibeos_procstate_t *ps) {
    uint64_t rflags, spins = 0;

    /* The caller's interrupt state, restored on acquire. Callers are syscalls,
     * so this is masked - but the spin below must unmask, so it is captured. */
    __asm__ __volatile__("pushfq; popq %0" : "=r"(rflags));
    for (;;) {
        uint32_t zero = 0;
        /* The holder's slot plus one, not just "busy": a fault that finds the
         * lock held by the very task that faulted is a store to user memory
         * made under it, which no amount of retrying ends (hw_stack_grow). */
        uint32_t me = (uint32_t)(hw_current_task() + 1);
        if (__atomic_compare_exchange_n(&ps->mm_busy, &zero, me ? me : 1u, 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
            if (rflags & 0x200ull) {
                __asm__ __volatile__("sti" ::: "memory");
            }
            return;
        }
        /* Open a window for interrupts before trying again, because a core
         * spinning here must be able to acknowledge a TLB shootdown IPI. The
         * lock holder can be inside a shootdown that targets this very core: a
         * fork holds this lock across clone_cow, which revokes the parent's
         * write permission and shoots down every core running the address
         * space, and mmap/munmap/mprotect of a sibling thread contend the same
         * lock on such a core. A syscall runs with interrupts masked, so
         * without this window that core could not answer until it left the
         * spin, and it would not leave until the holder released - the
         * shootdown's timeout panic, a deadlock. The spin holds no lock, so a
         * timer taken here is a safe preemption, and g_current_task is per-CPU
         * so identity survives it. Masked again before the next attempt, so
         * acquisition and the mutation run with interrupts as the caller had
         * them. */
        __asm__ __volatile__("sti; pause; cli" ::: "memory");
        if (++spins > 200000000ull) {
            hw_panic("mm lock held too long: a mutation did not release it");
        }
    }
}

void ks_mm_unlock(vibeos_procstate_t *ps) {
    __atomic_store_n(&ps->mm_busy, 0u, __ATOMIC_RELEASE);
}

vibeos_vmspace_t ks_vm(int slot) { return hw_vm(&g_tasks[slot].proc.as); }

/* One fresh page. A guard (no access) is present and nothing else, so ring 3
 * faults on it; anything else is user-reachable, writable when asked, and
 * no-execute unless asked (M-036). The map takes its own reference on the
 * frame, so the allocation's is given back either way (D9). */
static uint64_t ks_leaf_of(vibeos_prot_t prot) {
    uint64_t leaf = PTE_PRESENT;

    /* The two marks that are not access rights say nothing about whether the
     * page is reachable: a PROT_NONE region with the NOWRITE ceiling is still
     * a guard. */
    if ((prot & ~(VIBEOS_PROT_SHARED | VIBEOS_PROT_NOWRITE)) != VIBEOS_PROT_NONE) {
        leaf |= PTE_USER;
        if (prot & VIBEOS_PROT_WRITE) {
            leaf |= PTE_WRITE;
        }
        if (!(prot & VIBEOS_PROT_EXEC)) {
            leaf |= PTE_NX;
        }
    }
    if (prot & VIBEOS_PROT_SHARED) {
        leaf |= VIBEOS_PTE_SHARED;   /* a fork hands it on as it is (L3) */
    }
    return leaf;
}

int ks_map_anon(int slot, uint64_t va, vibeos_prot_t prot) {
    void *page = hw_alloc_user_page();
    int r;

    if (!page) {
        return -1;
    }
    r = hw_map_page(&g_tasks[slot].proc.as, va, (uint64_t)(uintptr_t)page, ks_leaf_of(prot));
    hw_page_put((uint64_t)(uintptr_t)page);
    return r != 0 ? -1 : 0;
}

/* A page somebody else owns too. The kernel reaches memory by identity, so the
 * pointer a filesystem holds its page by is the frame's address. */
int ks_map_page(int slot, uint64_t va, void *page, vibeos_prot_t prot) {
    return hw_map_page(&g_tasks[slot].proc.as, va, (uint64_t)(uintptr_t)page, ks_leaf_of(prot)) != 0
               ? -1 : 0;
}

void ks_page_unhold(void *page) {
    hw_page_put((uint64_t)(uintptr_t)page);
}

/* A mapping at an address the program chose goes in the high user window -
 * the 512 GiB of PML4 slot 1 - and nowhere else. The low window, where Linux
 * programs are linked, is carved out of the kernel's identity map a page at a
 * time, over physical memory reserved for exactly the image that lives there;
 * it is not somewhere to put an arbitrary mapping. */
int ks_user_fixed_ok(uint64_t base, uint64_t len) {
    const uint64_t lo = VIBEOS_HW_USER_BASE, hi = VIBEOS_HW_USER_BASE + (512ull << 30);

    return base >= lo && len <= hi - lo && base <= hi - len;
}

int ks_map_user_pages(int slot, uint64_t va, uint64_t pages) {
    return hw_map_user_pages(&g_tasks[slot].proc.as, va, pages);
}
void ks_tlb_drain(void) { hw_tlbq_drain(); }
void ks_tlb_flush_page(uint64_t va) { hw_invlpg(va); }
void *ks_page_alloc(void) { return hw_alloc_page(); }
void ks_page_free(void *page, const char *why) { hw_free_page_why(page, why); }
uint64_t ks_heap_base(void) { return VIBEOS_HW_USER_HEAP_BASE; }
uint64_t ks_mmap_base(void) { return VIBEOS_HW_USER_MMAP_BASE; }
/* How large the stack may become, which is what RLIMIT_STACK reports. */
uint64_t ks_stack_bytes(void) { return (uint64_t)(VIBEOS_HW_USER_STACK_MAX_PAGES - 1u) * 4096ull; }

/* Describe the page behind one address of a task's own address space: the bits
 * of its entry, and - read from one pinned instant - the frame's identity, its
 * owners and its first word. */
void ks_pageinfo(int slot, uint64_t va, vibeos_pageinfo_t *out) {
    uint64_t *pte;
    uint64_t entry;

    out->frame = 0;
    out->flags = 0;
    out->owners = 0;
    out->first_word = 0;

    pte = hw_pte_lookup(&g_tasks[slot].proc.as, va);
    if (!pte) {
        return;
    }
    entry = __atomic_load_n(pte, __ATOMIC_ACQUIRE);
    if (entry & PTE_PRESENT) {
        uint64_t phys = entry & 0x000FFFFFFFFFF000ull;
        uint32_t id = vibeos_frame_id(phys);

        out->flags |= VIBEOS_PAGE_PRESENT;
        if (entry & PTE_WRITE)         { out->flags |= VIBEOS_PAGE_WRITE; }
        if (entry & PTE_USER)          { out->flags |= VIBEOS_PAGE_USER; }
        if (entry & PTE_COW)           { out->flags |= VIBEOS_PAGE_COW; }
        if (entry & VIBEOS_PTE_OWNED)  { out->flags |= VIBEOS_PAGE_OWNED; }
        if (entry & PTE_NX)            { out->flags |= VIBEOS_PAGE_NX; }
        if (entry & VIBEOS_PTE_SHARED) { out->flags |= VIBEOS_PAGE_SHARED; }
        /* Everything below describes one frame, so it is all read from the
         * same pinned instant (M-038). A sibling thread can munmap the page,
         * the frame can be released and handed to another process, and a
         * bare read through the address taken from an entry this function
         * does not own returns that process's first eight bytes - and an
         * identity and an owner count taken at different moments describe
         * different frames.
         *
         * Pin the frame first - try_get refuses one nobody owns - and then
         * confirm the entry still names it: had the frame been freed and
         * reused before the pin, the entry would have changed. Only then are
         * the identity, the count and the word this caller's own page's.
         * If the page is gone, the flags from the entry stay and the rest
         * reports nothing, which is the truth.
         *
         * A frame the allocator does not describe - the reserved low
         * identity window - reports no identity rather than a wrong one, and
         * has no owner to race with, so it is read as before. The word is
         * read through the kernel's identity map deliberately, not through
         * the caller's translation: the value of this field is that it does
         * not use the mapping under suspicion. */
        if (id == 0xFFFFFFFFu) {
            out->first_word = *(const volatile uint64_t *)(uintptr_t)phys;
        } else if (vibeos_frame_try_get(phys)) {
            if (__atomic_load_n(pte, __ATOMIC_ACQUIRE) == entry) {
                out->frame = (uint64_t)id + 1ull;   /* 0 stays "nothing" */
                /* Our own pin is one of the owners; the caller asked about
                 * the mappings that were there. */
                out->owners = vibeos_frame_owners(phys) - 1u;
                out->first_word = *(const volatile uint64_t *)(uintptr_t)phys;
            }
            (void)vibeos_frame_put(phys);
        }
    }
}

/* ---- processes ------------------------------------------------------------------------ */

vibeos_procstate_t *ks_procstate_new(void) { return hw_procstate_new(); }
void ks_procstate_put(vibeos_procstate_t *ps) { hw_procstate_put(ps); }

int ks_fork_aspace(int child, int parent) {
    if (hw_aspace_create(&g_tasks[child].proc.as) != 0 ||
        hw_aspace_copy_user(&g_tasks[child].proc.as, &g_tasks[parent].proc.as) != 0) {
        return -1;
    }
    return 0;
}

void ks_drop_aspace(int slot) {
    hw_aspace_destroy_why(&g_tasks[slot].proc.as, "fork_failed");
}

int ks_alloc_kstack(int slot) {
    hw_task_t *t = &g_tasks[slot];

    t->kstack_top = hw_alloc_kstack(&t->kstack_base, &t->kstack_pages);
    return t->kstack_top ? 0 : -1;
}

void ks_fork_regs(int child, int parent, const ks_regs_t *frame) {
    hw_task_t *c = &g_tasks[child];
    const hw_task_t *p = &g_tasks[parent];
    uint32_t fi;

    /* The parent's image, whole, on the child's own address space. Only the
     * entry point was copied here, so every other field of the child's image -
     * the path it was started from above all - was whatever the slot's last
     * tenant had left: a forked child that asked who it was (readlink of
     * /proc/self/exe) was told the name of a program that had exited. Nothing
     * showed it until docs/abi/ L1 step 8 made execve answer for the same name,
     * and BusyBox's shell - which runs each applet by executing "the program I
     * am" from a forked child - started the thread tests instead of sort.
     * CLAUDE.md, "a field written on one path and read on all of them". */
    {
        vibeos_hw_aspace_t own = c->proc.as;
        c->proc = p->proc;
        c->proc.as = own;
    }
    c->cr3 = hw_proc_cr3(&c->proc);
    c->cr3_set_by = "fork";
    c->ctx = *hw_regs_c(frame);   /* resume exactly where the parent is */
    /* Including the vector registers. "Exactly where the parent is" was only
     * ever true of the integer ones, and a child that resumes mid-expression
     * with somebody else's xmm is the same defect as not saving them at all. */
    for (fi = 0; fi < 512u; fi++) {
        c->fpu[fi] = p->fpu[fi];
    }
    c->ctx.rax = 0;               /* ... but fork() returns 0 in the child */
    c->fs_base = p->fs_base;      /* the copied image expects its TLS */
}

void ks_thread_regs(int child, int parent, const ks_regs_t *frame,
                    uint64_t stack, uint64_t tls) {
    hw_task_t *c = &g_tasks[child];
    const hw_task_t *p = &g_tasks[parent];

    /* The same address space, by sharing the description rather than copying
     * the tables. hw_aspace_create is deliberately not called: two sets of
     * page tables would be two processes wearing one name. */
    c->proc = p->proc;
    c->cr3 = p->cr3;
    c->cr3_set_by = "clone_thread";
    c->ctx = *hw_regs_c(frame);
    c->ctx.rax = 0;            /* the child's return from clone() */
    c->ctx.rsp = stack;        /* on the stack the library gave it */
    c->ctx.rbp = 0;            /* no caller frame: this is a stack top */
    /* A clean FP environment, not the creator's. A new thread begins at a
     * function entry, so there is no partly-built vector value to inherit -
     * and inheriting one would mean two threads sharing a register file they
     * each believe is theirs. */
    hw_fpu_init_area(c->fpu);
    c->fs_base = tls;
}

uint8_t *ks_exec_buffer(uint32_t *cap) {
    *cap = g_exec_elf_cap;
    return g_exec_elf;
}
long ks_read_file_cached(const char *path, void *buf, uint32_t cap, uint32_t *out_id) {
    return hw_read_file_cached(path, buf, cap, out_id);
}
int ks_exec_refuse(vibeos_exec_fail_t why, const char *path, const char *detail) {
    return hw_exec_refuse(why, path, detail);
}
int ks_image_create(vibeos_image_t *img, vibeos_procstate_t *ps,
                    const unsigned char *elf, uint64_t len, uint64_t staged,
                    const char *const *argv, const char *const *envp,
                    const char *path, uint32_t file_id) {
    return hw_proc_create(img, ps, elf, len, staged, argv, envp, path, file_id);
}
void ks_image_drop(vibeos_image_t *img, const char *why) { hw_aspace_destroy_why(&img->as, why); }

void ks_exec_switch(int slot, const vibeos_image_t *img) {
    hw_task_t *t = &g_tasks[slot];
    vibeos_hw_aspace_t old_as = t->proc.as;   /* reclaim after switching CR3 */
    int shared;

    t->proc = *img;
    t->cr3 = hw_proc_cr3(&t->proc);
    t->cr3_set_by = "execve";
    hw_write_cr3(t->cr3);

    hw_spin_lock_named(&g_sched_lock, __func__);
    shared = hw_aspace_shared_by_other(old_as.pml4, slot);
    hw_spin_unlock(&g_sched_lock);
    if (shared) {
        HW_TASK_MARK(slot, aspace_killed_by, "execve_kept_shared");
    } else {
        hw_aspace_destroy_why(&old_as, "execve");   /* old CR3 no longer active */
    }
}

void ks_exec_regs(int slot, ks_regs_t *regs, uint64_t entry, uint64_t sp) {
    vibeos_x86_64_isr_frame_t *frame = hw_regs(regs);
    uint32_t k;

    for (k = 0; k < (uint32_t)sizeof(*frame); k++) {
        ((uint8_t *)(void *)frame)[k] = 0;
    }
    /* The old image is gone and its thread-local storage with it. Clear the
     * base here and in the register, because exec returns straight to user
     * space without passing through the scheduler's restore. */
    g_tasks[slot].fs_base = 0;
    hw_wrmsr(MSR_FS_BASE, 0);
    frame->rip = entry;
    frame->cs = VIBEOS_HW_USER_CODE_SEL;
    frame->rflags = 0x202;
    frame->rsp = sp;
    frame->ss = VIBEOS_HW_USER_DATA_SEL;
}

/* ---- registers ------------------------------------------------------------------------ */

uint64_t ks_regs_sp(const ks_regs_t *frame) { return hw_regs_c(frame)->rsp; }
uint64_t ks_regs_ret(const ks_regs_t *frame) { return hw_regs_c(frame)->rax; }

/* `syscall` and `int $0x80` are both two bytes, so stepping back two lands on
 * whichever the program used. */
void ks_regs_restart(ks_regs_t *regs, uint64_t nr) {
    vibeos_x86_64_isr_frame_t *frame = hw_regs(regs);

    frame->rax = nr;
    frame->rip -= 2u;
}
/* The registers by name, for a personality's signal frame (docs/abi/ L2). */
void ks_regs_get(const ks_regs_t *regs, vibeos_uregs_t *out) {
    const vibeos_x86_64_isr_frame_t *f = hw_regs_c(regs);

    out->r8 = f->r8;   out->r9 = f->r9;   out->r10 = f->r10; out->r11 = f->r11;
    out->r12 = f->r12; out->r13 = f->r13; out->r14 = f->r14; out->r15 = f->r15;
    out->rdi = f->rdi; out->rsi = f->rsi; out->rbp = f->rbp; out->rbx = f->rbx;
    out->rdx = f->rdx; out->rax = f->rax; out->rcx = f->rcx; out->rsp = f->rsp;
    out->rip = f->rip; out->rflags = f->rflags;
    out->cs = (uint16_t)f->cs;
    out->ss = (uint16_t)f->ss;
}

int ks_regs_set(ks_regs_t *regs, const vibeos_uregs_t *in) {
    vibeos_x86_64_isr_frame_t *f = hw_regs(regs);

    /* rip comes from memory the program can write; a non-canonical rip reaches
     * iretq and #GPs in ring 0 (H-018). The selectors and flags are forced
     * below, so this is the remaining ring-0 fault vector. rsp is left
     * unchecked - a bad rsp faults in ring 3 on the next push, killing the
     * task safely. Refused before anything is written. */
    if (!hw_user_addr_ok(in->rip)) {
        return -1;
    }
    f->r8 = in->r8;   f->r9 = in->r9;   f->r10 = in->r10; f->r11 = in->r11;
    f->r12 = in->r12; f->r13 = in->r13; f->r14 = in->r14; f->r15 = in->r15;
    f->rdi = in->rdi; f->rsi = in->rsi; f->rbp = in->rbp; f->rbx = in->rbx;
    f->rdx = in->rdx; f->rax = in->rax; f->rcx = in->rcx; f->rsp = in->rsp;
    f->rip = in->rip;
    /* Only user state, and the selectors are the user ones whatever the frame
     * said: nothing read out of user memory may decide privilege. The flags
     * keep the arithmetic ones, DF and OF; IF is always on, IOPL zero, no trap. */
    f->cs = VIBEOS_HW_USER_CODE_SEL;
    f->ss = VIBEOS_HW_USER_DATA_SEL;
    f->rflags = (in->rflags & 0x0000000000000CD5ull) | 0x202ull;
    return 0;
}

/* The kernel is built without SSE (CMakeLists.txt), so while it runs on a
 * task's behalf the vector registers are still the task's: the context switch
 * restores them on the way in. What is live is what the program had. */
uint64_t ks_fpu_size(void) { return 512u; }

int ks_fpu_save(uint64_t uaddr) {
    unsigned char area[512] __attribute__((aligned(16)));
    uint32_t k;

    /* FXSAVE leaves bytes 464-511 (software's) and the reserved ones it does
     * not define as it found them, and here they are this kernel stack's old
     * contents - pointers among them - copied into every signal frame a
     * program can read (external review, 2026-10-07). Cleared first. */
    for (k = 0; k < sizeof(area); k++) {
        ((volatile unsigned char *)area)[k] = 0;
    }
    __asm__ __volatile__("fxsave (%0)" :: "r"(area) : "memory");
    return vibeos_uaccess_copy((void *)(uintptr_t)uaddr, area, sizeof(area)) == 0 ? 0 : -1;
}

/* The word is the caller's to have judged, through linux_user_ok, as for the
 * copy: the range check has one call site (check-chokepoints.py). What this
 * adds is surviving the word going away between that and the exchange. */
int ks_user_cmpxchg32(uint64_t uaddr, uint32_t *expected, uint32_t desired) {
    if ((uaddr & 3u) != 0u) {
        return -1;
    }
    return vibeos_uaccess_cmpxchg32((uint32_t *)(uintptr_t)uaddr, expected, desired);
}

int ks_fpu_restore(uint64_t uaddr) {
    unsigned char area[512] __attribute__((aligned(16)));
    uint32_t mxcsr;

    if (vibeos_uaccess_copy(area, (const void *)(uintptr_t)uaddr, sizeof(area)) != 0) {
        return -1;
    }
    /* MXCSR's upper half is reserved and fxrstor raises #GP - in ring 0 - on a
     * set bit there. The program wrote this; it is cleared, as Linux does. */
    mxcsr = (uint32_t)area[24] | ((uint32_t)area[25] << 8) |
            ((uint32_t)area[26] << 16) | ((uint32_t)area[27] << 24);
    mxcsr &= 0x0000FFFFu;
    area[24] = (unsigned char)mxcsr;
    area[25] = (unsigned char)(mxcsr >> 8);
    area[26] = 0;
    area[27] = 0;
    __asm__ __volatile__("fxrstor (%0)" :: "r"(area) : "memory");
    return 0;
}

void ks_regs_enter_handler(ks_regs_t *regs, uint64_t handler, uint64_t sp,
                           uint64_t a0, uint64_t a1, uint64_t a2) {
    vibeos_x86_64_isr_frame_t *frame = hw_regs(regs);

    frame->rip = handler;
    frame->rsp = sp;
    frame->rdi = a0;
    frame->rsi = a1;
    frame->rdx = a2;
    frame->rax = 0;
    /* The System V ABI enters a function with the direction flag clear, and a
     * handler that inherited a set one would copy backwards. Trap off too:
     * single-stepping is not the handler's. */
    frame->rflags &= ~((1ull << 10) | (1ull << 8));
}

uint64_t ks_tls_get(int slot) { return g_tasks[slot].fs_base; }

/* %fs base, set by arch_prctl(ARCH_SET_FS) for the calling task: the field the
 * context switch restores, and the MSR now. */
void ks_tls_set(int slot, uint64_t base) {
    g_tasks[slot].fs_base = base;
    hw_wrmsr(MSR_FS_BASE, base);
}

/* ---- devices ------------------------------------------------------------------------- */

vibeos_inet_t *ks_net(void) { return g_net_up ? &g_net : 0; }
vibeos_lock_t *ks_net_lock(void) { return &g_net_lock; }
int ks_console_getc(void) { return hw_console_getc(); }
void ks_console_echo(char c) { hw_console_echo(c); }
uint32_t ks_foreground_pgid(void) { return g_console_foreground_pgid; }
void ks_set_foreground_pgid(uint32_t pgid) { g_console_foreground_pgid = pgid; }

void ks_con_lock(void) { vibeos_x86_64_serial_lock(); }
void ks_con_unlock(void) { vibeos_x86_64_serial_unlock(); }
void ks_con_puts(const char *s) { vibeos_x86_64_serial_puts(s); }
void ks_con_putc(char c) { vibeos_x86_64_serial_putc(c); }
void ks_con_hex(uint64_t v) { vibeos_x86_64_serial_print_hex(v); }
void ks_log(vibeos_log_level_t level, uint32_t code, uint64_t a0, uint64_t a1,
            const char *msg) {
    hw_log(level, code, a0, a1, msg);
}
void ks_panic(const char *why) { hw_panic(why); }
uint32_t ks_cpu_id(void) { return vibeos_x86_64_cpu_id(); }
uint64_t ks_cr3_now(void) { return hw_read_cr3(); }

void ks_con_cow_faults(uint64_t page) {
    uint32_t k2;

    for (k2 = 0; k2 < HW_COW_RING; k2++) {
        if (g_cow_ring[k2].va == 0ull ||
            (g_cow_ring[k2].va & ~0xFFFull) != page) {
            continue;
        }
        vibeos_x86_64_serial_puts(" | fault err=0x");
        vibeos_x86_64_serial_print_hex(g_cow_ring[k2].err);
        vibeos_x86_64_serial_puts(" rip=0x");
        vibeos_x86_64_serial_print_hex(g_cow_ring[k2].rip);
        vibeos_x86_64_serial_puts(" pid=0x");
        vibeos_x86_64_serial_print_hex(g_cow_ring[k2].pid);
        vibeos_x86_64_serial_puts(" ok=0x");
        vibeos_x86_64_serial_print_hex(g_cow_ring[k2].handled);
        vibeos_x86_64_serial_puts(" cpu=0x");
        vibeos_x86_64_serial_print_hex(g_cow_ring[k2].cpu);
    }
}
