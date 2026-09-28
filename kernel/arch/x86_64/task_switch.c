/* The context switch: the task table, the scheduler's pick, and loading a task
 * onto a CPU.
 *
 * Kernel stacks and their parking on exit, slot allocation and the transition
 * table's arch half, the pick (policy first, run queue as the fallback), the
 * x87/SSE register file, hw_task_load_cpu_state with the CR3 guard, the context
 * check, and hw_schedule itself.
 *
 * Lifted out of arch_hw.c whole (2026-09-28), like task_life.c before it and
 * mm_bridge.c the same day. Nothing changed in the move. Four of the five
 * functions check-task-identity.py allows to name a task's identity live here,
 * so that check reads this file as well as arch_hw.c - moving them out of its
 * sight would have been a check that stopped looking without saying so. What
 * crosses to arch_hw.c is declared in arch_hw_internal.h under "the context
 * switch". */

#include <stdint.h>
#include "vibeos/arch_x86_64.h"
#include "vibeos/trap.h"
#include "vibeos/boot.h"
#include "vibeos/mm.h"
#include "vibeos/inet.h"
#include "vibeos/elf.h"
#include "vibeos/services.h"
#include "vibeos/exec_stats.h"
#include "arch_hw_internal.h"
#include "vibeos/account.h"
#include "vibeos/forkguard.h"
#include "vibeos/sched_policy.h"
#include "vibeos/pageinfo.h"
#include "vibeos/rmap.h"
#include "vibeos/reclaim.h"
#include "vibeos/mbz.h"
#include "vibeos/abi.h"
#include "vibeos/abi_linux.h"
#include "vibeos/ceildiv.h"
#include "vibeos/blkdev.h"
#include "vibeos/io_stats.h"
#include "vibeos/blockdev.h"
#include "vibeos/partition.h"
#include "vibeos/parttab.h"
#include "vibeos/ext2.h"
#include "vibeos/iso9660.h"
#include "vibeos/exfat.h"
#include "vibeos/ntfs.h"
#include "vibeos/logsink.h"
#include "vibeos/storage.h"
#include "vibeos/swapmap.h"
#include "vibeos/anon.h"
#include "vibeos/swaparea.h"
#include "vibeos/log.h"
#include "vibeos/klog.h"
#include "vibeos/crash.h"
#include "vibeos/device.h"
#include "vibeos/mm_model.h"
#include "vibeos/frame.h"
#include "vibeos/vmspace.h"
#include "vibeos/vma.h"
#include "vibeos/backing.h"
#include "vibeos/task_stats.h"
#include "vibeos/task.h"
#include "vibeos/runq.h"
#include "vibeos/lifetime.h"
#include "vibeos/vfs.h"

/* Point the CPU at a task's ring-0 stack: the TSS one is used when ring 3 is
 * interrupted, the syscall one when it issues `syscall`. */
void hw_set_kernel_stack(uint64_t top) {
    hw_cpu_t *cpu = hw_this_cpu();
    cpu->tss.rsp0 = top;
    cpu->syscall_kstack_top = top;
}


/* Two contiguous pages when the PMM can give them, one otherwise. Reports the
 * base and page count so the stack can be reclaimed when the task exits. */
uint64_t hw_alloc_kstack(uint64_t *out_base, uint32_t *out_pages) {
    uint8_t *p = 0;
    uint64_t size = 8192ull;
    uint32_t pages = 2u;

    /* Two contiguous frames from the frame layer, not from the bump allocator
     * underneath it. The bump allocator is closed once the layer is up, and a
     * kernel stack taken from it would be a frame the layer believes is free. */
    p = (uint8_t *)hw_alloc_pages_contig(2u);
    if (!p) {
        p = (uint8_t *)hw_alloc_page();
        size = 4096ull;
        pages = 1u;
    }
    if (!p) {
        return 0;
    }
    if (out_base) {
        *out_base = (uint64_t)(uintptr_t)p;
    }
    if (out_pages) {
        *out_pages = pages;
    }
    return (uint64_t)(uintptr_t)p + size;
}

static void hw_free_kstack_pages(uint64_t base, uint32_t pages) {
    uint32_t i;
    for (i = 0; i < pages; i++) {
        hw_free_page((void *)(uintptr_t)(base + (uint64_t)i * 4096ull));
    }
}

void hw_drain_dead_kstack(void) {
    hw_cpu_t *cpu = hw_this_cpu();
    uint64_t base;
    uint32_t pages;

    if (cpu == 0 || cpu->dead_kstack_base == 0 || cpu->dead_kstack_pages == 0) {
        return;
    }
    /* Taken out of the slot before the pages are freed. hw_free_page can
     * schedule nothing and takes only the memory lock, but a slot still
     * holding an address whose pages are on the freelist is a second chance to
     * free them, and this is the one place that would ever get. */
    base = cpu->dead_kstack_base;
    pages = cpu->dead_kstack_pages;
    cpu->dead_kstack_base = 0;
    cpu->dead_kstack_pages = 0;

    /* The one assertion worth making here: this core must not be standing on
     * the stack it is about to free. If it is, the parking is wrong somewhere
     * and the machine should say so rather than corrupt itself quietly. */
    {
        uint64_t rsp;
        __asm__ __volatile__("mov %%rsp, %0" : "=r"(rsp));
        if (rsp >= base && rsp < base + (uint64_t)pages * 4096ull) {
            hw_panic("draining the kernel stack this core is standing on");
        }
    }
    hw_free_kstack_pages(base, pages);
    vibeos_task_stats()->dead_kstacks_freed++;
}

hw_task_t g_tasks[VIBEOS_HW_MAX_TASKS];

/* Recorded by hw_task_release when it finds a core still current on the slot it
 * is about to free, and printed by hw_panic_cpu_summary - which runs with the
 * cores quiesced, so the line comes out intact instead of interleaved with the
 * other cores' simultaneous panic the way the raw print did. */
volatile uint64_t g_relrace_rip;
volatile int g_relrace_curcpu = -1;
volatile int g_relrace_relcpu = -1;
volatile int g_relrace_slot = -1;
static uint32_t g_alloc_seq;
uint32_t g_console_foreground_pgid;
vibeos_service_supervisor_t g_runtime_supervisor;
uint8_t g_runtime_supervisor_ready;

/* Claim a free slot atomically: two cores can fork at the same time, so the
 * slot is marked RESERVED (never schedulable, never reapable) until the caller
 * has finished filling it in. */
/* Answer the portable task view. Phase S-P0 of docs/sched/.
 *
 * This is the whole of what the architecture layer owes the rest of the kernel
 * about a task: a copy of one slot. Deciding what to print is portable and
 * lives in kernel/sched/view.c; reading the table is not and lives here. The
 * split is the first piece of the scheduler rewrite, and it is deliberately the
 * cheapest one - a subsystem that cannot be looked at is a subsystem that gets
 * debugged by adding print statements to an eight-thousand-line file, which is
 * how every task defect in this project has actually been found.
 *
 * A copy, not a pointer: the console prints this while other cores create and
 * destroy tasks, and a pointer would name a slot that can be recycled between
 * one field and the next. */
const char *hw_task_state_name(int state) {
    switch (state) {
        case HW_TASK_FREE:     return "free";
        case HW_TASK_READY:    return "ready";
        case HW_TASK_RUNNING:  return "running";
        case HW_TASK_ZOMBIE:   return "zombie";
        case HW_TASK_BLOCKED:  return "blocked";
        case HW_TASK_RESERVED: return "reserved";
        default:               return "?";
    }
}

uint32_t hw_task_slots(void) {
    return (uint32_t)VIBEOS_HW_MAX_TASKS;
}

/* The only function that changes a task's state. Phase S-P1 of docs/sched/.
 *
 * It asks the portable transition table first, so an illegal change is refused
 * and counted rather than performed - and the two most expensive defects this
 * kernel has had were illegal changes that nothing refused: a slot released
 * while its task was still on a CPU, and a dying task announced before its
 * address space was gone.
 *
 * Both the layer's copy and this file's field are written here, in one place,
 * so they cannot disagree. The field stays because forty reads use it and
 * moving storage is S-P3's work; what has moved already is the decision about
 * whether the change is allowed at all.
 *
 * `why` is kept and printed by `tasks`. Three fields in this struct exist
 * because somebody once needed exactly that and did not have it. */
int hw_task_set_state(int slot, vibeos_task_state_t to, const char *why) {
    if (slot < 0 || slot >= (int)VIBEOS_HW_MAX_TASKS) {
        return -1;
    }
    if (vibeos_task_transition((uint32_t)slot, to, why) != 0) {
        vibeos_x86_64_serial_lock();
        vibeos_x86_64_serial_puts("[TASKS] ILLEGAL slot=0x");
        vibeos_x86_64_serial_print_hex((uint64_t)slot);
        vibeos_x86_64_serial_puts(" from=");
        vibeos_x86_64_serial_puts(vibeos_task_state_name(
            (vibeos_task_state_t)hw_slot_state(slot)));
        vibeos_x86_64_serial_puts(" to=");
        vibeos_x86_64_serial_puts(vibeos_task_state_name(to));
        vibeos_x86_64_serial_puts(" by=");
        vibeos_x86_64_serial_puts(why ? why : "-");
        vibeos_x86_64_serial_puts("\n");
        vibeos_x86_64_serial_unlock();
        return -1;
    }
    return 0;
}
static void hw_fpu_save(unsigned char *area);

int hw_task_alloc_guarded(int guarded, int privileged, uint32_t parent_pid,
                                 vibeos_fork_verdict_t *out_verdict) {
    int i;
    if (out_verdict) {
        *out_verdict = VIBEOS_FORK_OK;
    }
    hw_spin_lock_named(&g_sched_lock, __func__);
    if (guarded) {
        uint32_t in_use = 0, kids = 0;
        vibeos_fork_verdict_t v;

        for (i = 0; i < VIBEOS_HW_MAX_TASKS; i++) {
            if (hw_slot_state(i) == HW_TASK_FREE) {
                continue;
            }
            in_use++;
            /* What this thread group is accountable for, which is *not* just
             * its children.
             *
             * A thread inherits its creator's parent rather than becoming its
             * child - `child->ppid = parent->ppid` in clone - so counting by
             * ppid alone bounds fork and does not bound threads at all. With
             * the ceiling set to two, a boot that creates four threads produced
             * no clone refusal whatsoever: the reserve stopped a thread bomb
             * from taking the machine, and the per-task ceiling did nothing.
             *
             * So both are counted: tasks whose parent is this group, and other
             * tasks in this group. Counted rather than tracked in a field,
             * because a counter has to be right on every exit path including
             * the ones that fail half way, and this table is twenty-four
             * entries long. Zombies count, because a zombie still holds a
             * slot. */
            if (vibeos_task_accountable_to(&g_tasks[i].id, parent_pid)) {
                kids++;
            }
        }
        v = vibeos_forkguard_check(in_use, kids, privileged);
        if (v != VIBEOS_FORK_OK) {
            hw_spin_unlock(&g_sched_lock);
            if (out_verdict) {
                *out_verdict = v;
            }
            return -1;
        }
    }
    for (i = 0; i < VIBEOS_HW_MAX_TASKS; i++) {
        if (hw_slot_state(i) == HW_TASK_FREE) {
            /* A recycled slot must not keep the previous tenant's address
             * space. Leaving cr3 behind is not a tidiness problem: the page it
             * names has been freed and handed back to the allocator, so a slot
             * scheduled before its creator finishes filling it in installs a
             * page table that belongs to somebody else now. Zeroing it makes
             * that a named panic on the next context switch instead of a
             * machine that stops without a word.
             *
             * The three markers go with it, or a diagnostic reads as the
             * history of whoever had the slot last. */
            g_tasks[i].cr3 = 0;
            g_tasks[i].cr3_set_by = 0;
            g_tasks[i].ready_by = 0;
            /* Marked, not cleared: this is the other way pml4 reaches zero,
             * and leaving it blank made the guard's report ambiguous between
             * "the space was destroyed" and "the slot was handed out again". */
            g_tasks[i].aspace_killed_by = "task_alloc_clear";
            /* A defined FPU state, not the previous tenant's. Same family as
             * interp_base: a field written on one path and read on all of them
             * hands out whatever was there. */
            hw_fpu_init_area(g_tasks[i].fpu);
            g_tasks[i].alloc_seq = (uint32_t)__sync_add_and_fetch(&g_alloc_seq, 1u);
            g_tasks[i].proc.as.pml4 = 0;
            g_tasks[i].ps = 0;
            /* Every identity field, not the three that happened to be remembered:
             * a recycled slot otherwise starts with the previous tenant's pending
             * signals, exit status and name. */
            vibeos_task_identity_reset(&g_tasks[i].id);
            g_tasks[i].abi = vibeos_abi_linux();   /* bound once, here (C4) */
            (void)hw_task_set_state(i, HW_TASK_RESERVED, __func__);
            /* Admitted here because this is the only place a slot is claimed,
             * so no way of making a task can forget to. The class is corrected
             * by the idle-task path, which is the only caller that knows it is
             * making one; everything else is a normal task at nice 0 until
             * something says otherwise. */
            (void)vibeos_sched_policy_admit((uint32_t)i, VIBEOS_SCHED_NORMAL, 0, 0u);
            vibeos_task_stats()->created++;
            hw_spin_unlock(&g_sched_lock);
            return i;
        }
    }
    vibeos_task_stats()->slot_refused++;
    hw_spin_unlock(&g_sched_lock);
    if (out_verdict) {
        *out_verdict = VIBEOS_FORK_NO_SLOTS;
    }
    return -1;
}

/* Unguarded, for the kernel's own tasks and the idle tasks. */
int hw_task_alloc(void) {
    return hw_task_alloc_guarded(0, 1, 0u, 0);
}

/* Give a reserved slot back after a failed creation. */
void hw_task_release(int i) {
    /* No core may still call this slot its current task. The releasing core has
     * already made `next` current (hw_schedule/the exit path set current_task
     * before this), so it never matches; a *different* core that does is running
     * on a slot about to go FREE and have its kernel stack reclaimed - the
     * four-worker use-after-free, named at its source rather than later at the
     * kstack free. */
    if (i >= 0 && i < VIBEOS_HW_MAX_TASKS) {
        uint32_t c;
        for (c = 0; c < VIBEOS_HW_MAX_CPUS; c++) {
            if (g_cpus[c].current_task != i) {
                continue;
            }
            /* hw_task_release runs under g_sched_lock, so the console lock would
             * deadlock here; record the culprit in globals instead and let
             * hw_panic_cpu_summary print them once the cores are quiesced -
             * intact, and with the caller of hw_task_release named. */
            g_relrace_rip = (uint64_t)(uintptr_t)__builtin_return_address(0);
            g_relrace_curcpu = (int)c;
            g_relrace_relcpu = (int)hw_this_cpu()->index;
            g_relrace_slot = i;
            hw_panic("releasing a slot a cpu is still current on");
        }
    }
    /* Forgotten before the slot is published, not after: a slot the policy
     * still knows about is one it will schedule the moment somebody else
     * claims it, charging the new tenant's time to the old one's history. */
    if (i >= 0 && i < VIBEOS_HW_MAX_TASKS) {
        vibeos_sched_policy_forget((uint32_t)i);
    }
    if (i >= 0) {
        (void)hw_task_set_state(i, HW_TASK_FREE, __func__);
    }
}

/* Next task to run on `cpu`, round robin. Only READY tasks are candidates: a
 * RUNNING one is owned by some core (possibly another), so on SMP it must never
 * be picked twice. Idle tasks are skipped unless nothing else is available.
 * Returns -1 when the caller should simply keep running what it has. */
/* Is this slot something `cpu` may run?
 *
 * The one thing the run queue cannot answer for itself, and deliberately so:
 * the state machine is the single source of truth about what READY means, and
 * a queue that kept its own opinion would be a second one. An idle task is
 * excluded because it is the fallback, not a candidate; a task already on a
 * CPU is excluded because two cores running one task is a defect this kernel
 * has had. */
int hw_task_runnable(void *ctx, uint32_t slot, uint32_t cpu) {
    (void)ctx;
    (void)cpu;
    if (slot >= (uint32_t)VIBEOS_HW_MAX_TASKS) {
        return 0;
    }
    return vibeos_task_state(slot) == VIBEOS_TASK_READY &&
           !g_tasks[slot].id.is_idle && !g_tasks[slot].on_cpu;
}

vibeos_runq_t g_runq;

/* Now a wrapper. The scan, the cursor and the idle fallback are in
 * kernel/sched/runq.c, where "two CPUs never pick the same slot" is a unit test
 * rather than a boot somebody hopes goes wrong.
 *
 * One behaviour changed in the move, and it is a fix: the old scan started from
 * the *current* task, which is fair while something is running and degenerates
 * when the current task is idle - the search then begins at the same place
 * every time and the high-numbered slots wait behind the low ones. The queue
 * keeps a cursor per CPU instead. */
/* How long this task may hold a core, in ticks. An idle task gets the
 * shortest slice there is: it must give the core back the moment anything else
 * wants it, and its slice only bounds how long a core stays idle when work has
 * appeared and no timer has fired yet. */
uint32_t hw_slice_for(int slot) {
    vibeos_sched_class_t cls;

    if (slot < 0 || slot >= VIBEOS_HW_MAX_TASKS) {
        return 1u;
    }
    /* Asked, not derived. Deriving it from is_idle was the same mistake in
     * two places: it cannot see a KERNEL task, so the top class got a NORMAL
     * slice. */
    cls = vibeos_sched_policy_class((uint32_t)slot);
    return vibeos_sched_policy_quantum(cls);
}

/* Is anything runnable that outranks what this core is running?
 *
 * Only a class comparison, deliberately. Preempting for a *better-weighted*
 * task in the same class would make the slice meaningless - the whole point of
 * a slice is that a task keeps the core for it - and weights are already
 * honoured when the next task is chosen. A class is different: it is a promise
 * that higher work runs first, and a promise that waits for a slice boundary is
 * a promise with an asterisk. */
static int hw_higher_class_runnable(hw_cpu_t *cpu, int current) {
    uint32_t i;
    uint64_t runnable = 0;

    if (current < 0 || current >= VIBEOS_HW_MAX_TASKS) {
        return 1;
    }
    for (i = 0; i < (uint32_t)VIBEOS_HW_MAX_TASKS; i++) {
        if ((int)i != current && hw_task_runnable(0, i, (uint32_t)cpu->index)) {
            runnable |= 1ull << i;
        }
    }
    /* The rule itself lives with the policy, so this cannot half-implement it:
     * this function tested only "current is IDLE", and a KERNEL task waited
     * behind a NORMAL one for the rest of its slice (M-035). */
    return vibeos_sched_policy_should_preempt((uint32_t)cpu->index, current, runnable);
}

int hw_pick_next(hw_cpu_t *cpu) {
    uint64_t runnable = 0;
    uint32_t i;
    int chosen;

    /* The run queue decides *who may run*; the policy decides *which of them*.
     *
     * Keeping those two apart is the reason the policy could be written as a
     * pure function and tested exhaustively: it never asks about task state,
     * only about the set it is handed. hw_task_runnable stays the single
     * definition of runnable, so a task the scheduler would not have run
     * cannot become runnable by being favoured.
     */
    /* The bitmask is 64 bits wide, so the table must fit in it. This was a
     * runtime `i < 64u` that has never once been false, which is worse than no
     * check at all: it reads as a guard, it costs a comparison per slot, and
     * it would have silently dropped slots 64 and up rather than failing. A
     * static assertion cannot be true-by-accident. */
    _Static_assert(VIBEOS_HW_MAX_TASKS <= 64,
                   "the runnable bitmask is 64 bits wide");
    for (i = 0; i < (uint32_t)VIBEOS_HW_MAX_TASKS; i++) {
        if (hw_task_runnable(0, i, (uint32_t)cpu->index)) {
            runnable |= (1ull << i);
        }
    }

    chosen = vibeos_sched_policy_pick((uint32_t)cpu->index, runnable);
    if (chosen >= 0) {
        return chosen;
    }

    /* Nothing the policy knows about. Fall back rather than idle: a task the
     * policy was never told about is a bug in admission, and refusing to run it
     * would turn that bug into a hang instead of a slower machine. The run
     * queue also owns the "keep running the current task" answer, which the
     * policy has no way to express. */
    return vibeos_runq_pick(&g_runq, (uint32_t)cpu->index, cpu->current_task);
}


/* Load the CPU state that belongs to a task rather than to the CPU: its page
 * tables, its kernel stack, and its thread-local storage base.
 *
 * This exists as one function because there is more than one way to resume a
 * task - the timer switch, the exit path, and the idle entry - and each of
 * them has to reload all of it. Missing the TLS base in one of them is not
 * visible at the point of the mistake: the task simply reads through whatever
 * base the previous occupant of the CPU left behind, which is a fault if that
 * was zero and someone else's thread state if it was not. */
/* ---- the x87/SSE register file across a context switch --------------------
 *
 * The kernel enabled SSE and never saved it. See
 * docs/implementation_progress/mm_no_fpu_context.md: a task's vector registers
 * survived a syscall or an interrupt by luck, and because clang stores 16-byte
 * stack buffers with movdqa, a user buffer that lost a store lost exactly
 * sixteen bytes - one register - which is the width that had no explanation
 * while the search was in the memory manager.
 *
 * Eager, not lazy. CR0.TS with a trap on first use is the classic optimisation
 * and it is deliberately not taken here: it needs a fault handler, a per-core
 * notion of who owns the registers, and cross-core invalidation when a task
 * migrates - three mechanisms, on the path this kernel has the most defects in.
 * fxsave and fxrstor are about a hundred cycles each on a switch that already
 * costs far more, and C0's histogram is there to say if that is wrong.
 */
static void hw_fpu_save(unsigned char *area) {
    __asm__ __volatile__("fxsave (%0)" :: "r"(area) : "memory");
}

void hw_fpu_restore(const unsigned char *area) {
    __asm__ __volatile__("fxrstor (%0)" :: "r"(area) : "memory");
}

/* The state a task starts with, built by hand rather than by fxsave.
 *
 * Zeroing the area is not enough and the reason is easy to miss: MXCSR lives at
 * offset 24, and zero there unmasks every SSE exception, so the first floating
 * point operation in a fresh program faults. 0x1F80 is the reset value - all
 * exceptions masked - and 0x037F at offset 0 is the x87 control word's.
 *
 * By hand rather than fxsave-ing a freshly initialised unit, because doing that
 * would mean disturbing the FPU of whatever task happens to be running when a
 * new one is created. */
void hw_fpu_init_area(unsigned char *area) {
    uint32_t i;

    for (i = 0; i < 512u; i++) {
        area[i] = 0u;
    }
    area[0] = 0x7Fu;  area[1] = 0x03u;   /* FCW  = 0x037F */
    area[24] = 0x80u; area[25] = 0x1Fu;  /* MXCSR = 0x1F80 */
    area[28] = 0xFFu; area[29] = 0xFFu;  /* MXCSR_MASK = 0xFFFF */
}

void hw_task_load_cpu_state(int idx) {
    /* Counted, not timed. This function *is* the switch - it ends by installing
     * cr3 and returning into the next task - so there is no second point at
     * which to stop a clock: the cycles after the cr3 write belong to whoever
     * runs next. A count is still the number that matters here, because what
     * the refactor could change is how often the machine switches, and that is
     * deterministic. */
    __sync_fetch_and_add(&g_perf_switch.count, 1ull);

    /* Refuse to install an address space that no longer maps the kernel.
     *
     * This is the check that turned the intermittent wedge from a mystery into
     * an event with a name. Asking the emulator where a stopped guest's cores
     * were put one of them, every time, on the CR3 write below - holding a
     * PML4 whose entry 0, the kernel's own mapping, was not present. After
     * that write the next instruction fetch has nowhere to come from, and the
     * machine cannot even report a fault, because reporting one means running
     * kernel code. Hence silence rather than a panic.
     *
     * Entry 0 goes missing because a freed page is where the allocator keeps
     * its freelist link: hw_free_page writes the next pointer into offset zero
     * of the page it is reclaiming, and offset zero of a PML4 is exactly the
     * kernel's entry. So an address space that has been destroyed does not
     * merely become stale, it becomes an address space with no kernel in it -
     * and the pointer stored there is page-aligned, so the present bit reads
     * as clear.
     *
     * Panicking here is not a fix; it converts an unexplainable silence into a
     * backtrace, a log dump and the identity of the task involved, which is
     * what the fix will be built from. */
    {
        const uint64_t *pml4 =
            (const uint64_t *)(uintptr_t)(g_tasks[idx].cr3 & ~0xFFFull);

        if (pml4 == 0 || (pml4[0] & PTE_PRESENT) == 0) {
            vibeos_x86_64_serial_puts("[SCHED] address space has no kernel:"
                                      " task=0x");
            vibeos_x86_64_serial_print_hex((uint64_t)idx);
            vibeos_x86_64_serial_puts(" pid=0x");
            vibeos_x86_64_serial_print_hex(g_tasks[idx].id.pid);
            vibeos_x86_64_serial_puts(" cr3=0x");
            vibeos_x86_64_serial_print_hex(g_tasks[idx].cr3);
            vibeos_x86_64_serial_puts(" pml4[0]=0x");
            vibeos_x86_64_serial_print_hex(pml4 ? pml4[0] : 0);
            /* What *kind* of task this is decides where to look. A cr3 of zero
             * and a cr3 pointing at a recycled page are different bugs wearing
             * the same symptom: the first is a task made runnable before its
             * address space was installed, the second one whose space was
             * taken away afterwards. Reading the code did not separate them,
             * so the guard says which it is holding. */
            vibeos_x86_64_serial_puts(" state=0x");
            vibeos_x86_64_serial_print_hex((uint64_t)hw_slot_state(idx));
            vibeos_x86_64_serial_puts(" user=0x");
            vibeos_x86_64_serial_print_hex((uint64_t)g_tasks[idx].id.is_user);
            vibeos_x86_64_serial_puts(" idle=0x");
            vibeos_x86_64_serial_print_hex((uint64_t)g_tasks[idx].id.is_idle);
            vibeos_x86_64_serial_puts(" on_cpu=0x");
            vibeos_x86_64_serial_print_hex((uint64_t)g_tasks[idx].on_cpu);
            vibeos_x86_64_serial_puts(" ppid=0x");
            vibeos_x86_64_serial_print_hex((uint64_t)g_tasks[idx].id.ppid);
            vibeos_x86_64_serial_puts(" pml4=0x");
            vibeos_x86_64_serial_print_hex(
                (uint64_t)(uintptr_t)g_tasks[idx].proc.as.pml4);
            vibeos_x86_64_serial_puts(" cr3_set_by=");
            vibeos_x86_64_serial_puts(g_tasks[idx].cr3_set_by
                                      ? g_tasks[idx].cr3_set_by : "never");
            vibeos_x86_64_serial_puts(" ready_by=");
            vibeos_x86_64_serial_puts(g_tasks[idx].ready_by
                                      ? g_tasks[idx].ready_by : "never");
            vibeos_x86_64_serial_puts(" aspace_killed_by=");
            vibeos_x86_64_serial_puts(g_tasks[idx].aspace_killed_by
                                      ? g_tasks[idx].aspace_killed_by : "never");
            vibeos_x86_64_serial_puts(" last_destroy=");
            vibeos_x86_64_serial_puts(g_last_destroy_why ? g_last_destroy_why
                                                         : "none");
            vibeos_x86_64_serial_puts(" of=0x");
            vibeos_x86_64_serial_print_hex(g_last_destroy_pml4);
            vibeos_x86_64_serial_puts(" on_cpu=0x");
            vibeos_x86_64_serial_print_hex((uint64_t)g_last_destroy_cpu);
            vibeos_x86_64_serial_puts("\n");
            hw_log(VIBEOS_LOG_FATAL, 5u, g_tasks[idx].cr3,
                   (uint64_t)g_tasks[idx].id.pid,
                   "scheduler asked to install a freed address space");
            hw_panic("address space freed while still schedulable");
        }
    }
    if (g_tasks[idx].id.is_thread && vibeos_task_first_run((uint32_t)idx)) {
        hw_log(VIBEOS_LOG_DEBUG, 24u, (uint64_t)g_tasks[idx].id.pid,
               g_tasks[idx].ctx.rip, "thread scheduled for the first time");
    }
    hw_write_cr3(g_tasks[idx].cr3);
    hw_set_kernel_stack(g_tasks[idx].kstack_top);
    if (g_tasks[idx].id.is_user) {
        hw_wrmsr(MSR_FS_BASE, g_tasks[idx].fs_base);
    }
}

/* Timer-driven preemption: save the interrupted task, pick the next runnable
 * one, and resume it by rewriting the live IRQ frame and switching CR3. Runs
 * for the life of the system - there is no "demo over" exit.
 *
 * On SMP every core's local-APIC timer calls this independently; the run queue
 * is shared, so the pick-and-claim step is done under the scheduler lock. */
/* Refuse to enter a context that cannot be a context. Phase S-P1 of docs/sched/.
 *
 * The failure this exists for: a #GP inside vibeos_x86_64_task_enter, and then
 * a second #GP at rip=0xe4e4e4e4e4e4e4e4 - a saved register context holding a
 * repeated fill byte. iretq consumed it, so the machine died on the way into
 * ring 3 with the panic handler running on the same broken state, and every
 * boot it happened in reported nothing but silence.
 *
 * iretq is the wrong place to find out. It has no way to say which task, which
 * slot, or who filled it in; it just faults, and by then the state that would
 * have named the culprit is the state that caused the fault. Six comparisons
 * before the jump turn that into a sentence.
 *
 * What is checked is only what the hardware requires and this kernel controls:
 * the two selector pairs it ever uses, a canonical rip and rsp, and the two
 * rflags bits that are not negotiable (bit 1 is always set; IF must be on or
 * the task can never be preempted again). Anything else a task may legitimately
 * hold. */
void hw_ctx_check(int slot, const char *where) {
    const vibeos_x86_64_isr_frame_t *c;
    const char *bad = 0;

    if (slot < 0 || slot >= VIBEOS_HW_MAX_TASKS) {
        hw_panic("task_enter on a slot that does not exist");
    }
    c = &g_tasks[slot].ctx;

    if (c->cs == VIBEOS_HW_USER_CODE_SEL) {
        if (c->ss != VIBEOS_HW_USER_DATA_SEL) { bad = "user cs with a non-user ss"; }
    } else if (c->cs == VIBEOS_HW_KERNEL_CS) {
        if (c->ss != 0x10u) { bad = "kernel cs with a non-kernel ss"; }
    } else {
        bad = "cs is not a selector this kernel issues";
    }
    /* Canonical: bits 63..47 all equal. A fill byte fails this, and so does a
     * pointer read out of a page that has been recycled. */
    if (!bad && ((c->rip >> 47) != 0ull && (c->rip >> 47) != 0x1FFFFull)) {
        bad = "rip is not canonical";
    }
    if (!bad && ((c->rsp >> 47) != 0ull && (c->rsp >> 47) != 0x1FFFFull)) {
        bad = "rsp is not canonical";
    }
    if (!bad && (c->rflags & 0x2ull) == 0ull) { bad = "rflags bit 1 clear"; }
    if (!bad && (c->rflags & 0x200ull) == 0ull) { bad = "rflags has interrupts off"; }
    if (!bad) {
        return;
    }

    /* One call, because a report assembled from a dozen serial_puts is a dozen
     * critical sections and three other cores are writing their own. */
    vibeos_x86_64_serial_lock();
    vibeos_x86_64_serial_puts("[CTX] refusing to enter task: ");
    vibeos_x86_64_serial_puts(bad);
    vibeos_x86_64_serial_puts(" from=");
    vibeos_x86_64_serial_puts(where ? where : "-");
    vibeos_x86_64_serial_puts(" slot=0x");
    vibeos_x86_64_serial_print_hex((uint64_t)slot);
    vibeos_x86_64_serial_puts(" gen=0x");
    vibeos_x86_64_serial_print_hex((uint64_t)g_tasks[slot].alloc_seq);
    vibeos_x86_64_serial_puts(" state=");
    vibeos_x86_64_serial_puts(hw_task_state_name(hw_slot_state(slot)));
    vibeos_x86_64_serial_puts(" state_by=");
    vibeos_x86_64_serial_puts(vibeos_task_last_why((uint32_t)slot));
    vibeos_x86_64_serial_puts(" ready_by=");
    vibeos_x86_64_serial_puts(g_tasks[slot].ready_by ? g_tasks[slot].ready_by : "-");
    vibeos_x86_64_serial_puts(" on_cpu=0x");
    vibeos_x86_64_serial_print_hex((uint64_t)g_tasks[slot].on_cpu);
    vibeos_x86_64_serial_puts(" pid=0x");
    vibeos_x86_64_serial_print_hex((uint64_t)g_tasks[slot].id.pid);
    vibeos_x86_64_serial_puts(" cr3=0x");
    vibeos_x86_64_serial_print_hex(g_tasks[slot].cr3);
    vibeos_x86_64_serial_puts(" cr3_by=");
    vibeos_x86_64_serial_puts(g_tasks[slot].cr3_set_by ? g_tasks[slot].cr3_set_by : "-");
    vibeos_x86_64_serial_puts(" exe=");
    vibeos_x86_64_serial_puts(g_tasks[slot].proc.exe_path[0] ?
                              g_tasks[slot].proc.exe_path : "-");
    vibeos_x86_64_serial_puts(" rip=0x");
    vibeos_x86_64_serial_print_hex(c->rip);
    vibeos_x86_64_serial_puts(" cs=0x");
    vibeos_x86_64_serial_print_hex(c->cs);
    vibeos_x86_64_serial_puts(" rflags=0x");
    vibeos_x86_64_serial_print_hex(c->rflags);
    vibeos_x86_64_serial_puts(" rsp=0x");
    vibeos_x86_64_serial_print_hex(c->rsp);
    vibeos_x86_64_serial_puts(" ss=0x");
    vibeos_x86_64_serial_print_hex(c->ss);
    vibeos_x86_64_serial_puts(" rax=0x");
    vibeos_x86_64_serial_print_hex(c->rax);
    vibeos_x86_64_serial_puts(" rbp=0x");
    vibeos_x86_64_serial_print_hex(c->rbp);
    vibeos_x86_64_serial_puts("\n");
    vibeos_x86_64_serial_unlock();

    /* A panic, not a skip. The context is already unusable, and a task quietly
     * dropped from the run queue is the silence this was written to end. */
    hw_panic("a task context was not enterable");
}

void hw_schedule(vibeos_x86_64_isr_frame_t *frame) {
    hw_cpu_t *cpu = hw_this_cpu();
    int cur, next;

    if (!g_sched_running) {
        return;
    }

    /* A stack left behind by a task that exited on this core. Safe here for
     * the reason the whole mechanism exists: this core is running on the
     * current task's stack, which is never the parked one. */
    hw_drain_dead_kstack();

    /* The quantum, finally consulted.
     *
     * Preemption used to happen on every timer tick, so the time slice was
     * "whatever the timer period is" - a number nobody chose, and the thing
     * step 1 of S-P5 exists to remove. The policy states a slice per class;
     * this is where it is spent.
     *
     * Two ways out of a slice before it is used up, and both are required
     * rather than convenient:
     *
     *   the task stopped being runnable - it blocked, exited, or was killed,
     *   and holding a core for the rest of a slice nobody is using is just a
     *   stall;
     *
     *   something in a higher class became runnable - a class is only absolute
     *   if it preempts, otherwise a kernel task waits behind a normal one for
     *   the rest of its slice and the guarantee is only true between slices.
     *
     * Checked before taking the scheduler lock, because the common case is
     * "keep running" and taking a lock to decide not to switch is the cost
     * this whole path is trying to avoid. */
    if (cpu->slice_left > 0u) {
        int c = cpu->current_task;

        if (c >= 0 && c < VIBEOS_HW_MAX_TASKS && !g_tasks[c].id.is_idle &&
            vibeos_task_state((uint32_t)c) == VIBEOS_TASK_RUNNING &&
            !hw_higher_class_runnable(cpu, c)) {
            cpu->slice_left--;
            return;
        }
        cpu->slice_left = 0;   /* the slice is over, whatever is left of it */
    }

    hw_spin_lock_named(&g_sched_lock, __func__);
    cur = cpu->current_task;
    next = hw_pick_next(cpu);
    if (next < 0 || next == cur) {
        hw_spin_unlock(&g_sched_lock);
        return; /* nothing else runnable: keep running the current task */
    }
    if (cur >= 0) {
        g_tasks[cur].ctx = *frame;
        /* Beside the integer context, and for the same reason it is here: after
         * on_cpu is cleared another core may take this task, and it must find
         * every register it left behind. */
        hw_fpu_save(g_tasks[cur].fpu);
        if (hw_slot_state(cur) == HW_TASK_RUNNING) {
            (void)hw_task_set_state(cur, HW_TASK_READY, __func__);
            HW_TASK_MARK(cur, ready_by, "preempted");
        }
        /* Only now is it safe for another core to take it: its context is
         * saved and this core is about to stop touching it. */
        g_tasks[cur].on_cpu = 0;
    }
    cpu->current_task = next;
    (void)hw_task_set_state(next, HW_TASK_RUNNING, __func__);
    g_tasks[next].on_cpu = 1;
    vibeos_account_switch(hw_this_cpu()->index, next, vibeos_task_ready_at((uint32_t)next));
    hw_this_cpu()->slice_left = hw_slice_for(next);
    *frame = g_tasks[next].ctx;
    hw_fpu_restore(g_tasks[next].fpu);
    hw_spin_unlock(&g_sched_lock);

    /* The ordinary switch, and the one that matters most: this frame is about
     * to be consumed by the iretq at the end of the ISR, several returns away
     * from anything that could say which task it belonged to.
     *
     * Checked outside the scheduler lock deliberately - the report takes the
     * console lock, and taking that one inside this one is a lock order this
     * kernel does not otherwise use. `next` is marked on_cpu here, so no other
     * core can be writing the context being read. */
    hw_ctx_check(next, "preempt");

    hw_task_load_cpu_state(next);
}

/* Does anyone else still hold this address space?
 *
 * Threads share page tables, so the space may only be destroyed by whoever
 * leaves last. The answer is derived from the task table rather than from a
 * reference count kept alongside it: the table is what the scheduler already
 * believes, and a second count of the same thing is a second thing that can
 * disagree with the first. Yesterday's silent wedge was exactly a teardown
 * acting on a belief the scheduler no longer shared.
 *
 * Must be called with g_sched_lock held: without it the answer can be stale by
 * the time it is acted on, which is the whole failure mode.
 */
/* Does any live task other than `except` run on these tables?
 *
 * Asked by address space rather than by task, because exec needs the same
 * question about tables it has *already* stopped using - and asking it through
 * the task would look at the new address space, which is not the one about to
 * be freed. Both callers share this so the two can never answer differently.
 *
 * The caller holds g_sched_lock. */
int hw_aspace_shared_by_other(const uint64_t *pml4, int except) {
    int i;

    if (pml4 == 0) {
        return 0;
    }
    for (i = 0; i < VIBEOS_HW_MAX_TASKS; i++) {
        if (i == except || hw_slot_state(i) == HW_TASK_FREE) {
            continue;
        }
        /* A zombie is not running, but it has not been reaped either, and its
         * cr3 still names these tables - the guard in hw_task_load_cpu_state
         * would fire if they were freed under it. */
        if (g_tasks[i].proc.as.pml4 == pml4) {
            return 1;
        }
    }
    return 0;
}

int hw_aspace_still_shared(int me) {
    const uint64_t *pml4 = g_tasks[me].proc.as.pml4;
    int i;

    if (pml4 == 0) {
        return 0;
    }
    (void)i;
    return hw_aspace_shared_by_other(pml4, me);
}
