/* How the machine reports on itself: the kernel log's serial device, the
 * backtrace, hw_panic and the park of every core, the lock-deadlock report, the
 * per-CPU summary a panic prints, the walk that explains a vanished mapping, and
 * the crash dump.
 *
 * The portable half is kernel/diag (the log ring, the crash records, the
 * must-be-zero registry); this is the half that needs the machine - the serial
 * port, the frame pointers, the per-CPU table and the task table.
 *
 * Lifted out of arch_hw.c whole (2026-09-29), after the memory bridge, the
 * context switch, AP bring-up and the userland start. Nothing changed in the
 * move. Most of what CLAUDE.md learned about panics and wedges was learned in
 * these lines: a panic that stopped one core instead of the machine, a
 * backtrace named by nearest-preceding symbol, a deadlock that went silent. */

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

/* ---- the kernel log's device --------------------------------------------
 *
 * The log itself - the ring, its lock, the formatting, the sinks and the dumps -
 * is kernel/diag/klog.c (C6). What stays here is what is different on another
 * machine: the UART it writes to, and the spin lock it is serialised with.
 *
 * Events are always recorded into the ring, which costs a memcpy, and only
 * those at or above INFO are also written to the serial line: the line is slow
 * enough under emulation to change the timing of the bug being hunted. When the
 * machine dies the ring is dumped, including everything that was too quiet to
 * print at the time, which is usually the part worth reading. */

static hw_lock_t g_klog_lock;

static void hw_klog_lock(void) {
    hw_spin_lock_named(&g_klog_lock, "vibeos_klog");
}

static void hw_klog_unlock(void) {
    hw_spin_unlock(&g_klog_lock);
}

/* One complete line, in one call to serial_puts - which is one critical section
 * on the console lock. There is nothing to bracket, so nothing to forget to
 * bracket: that is the defect this sink exists not to be able to have. Its
 * predecessor, hw_log_emit, built each line from eight writes and ended with an
 * unlock it had never taken - see "An unmatched unlock" in CLAUDE.md.
 *
 * It never logs from inside itself, so it is registered with no reentrancy
 * guard (below) and its `reentered` count is zero by construction; the gate
 * asserts it anyway, so turning the guard back on for it would be seen. */
static int hw_serial_sink_write(void *ctx, const char *line, uint32_t len) {
    (void)ctx;
    (void)len;
    vibeos_x86_64_serial_puts(line);
    return 0;
}

/* No reentrancy guard: serial_puts never logs, so this sink cannot be re-entered,
 * and a sink with no guard is never skipped. The guard used to be on for every
 * sink and keyed by core, which could skip serial lines for a reason that did
 * not apply to it. Designated, because a positional initialiser silently shifts
 * the day the struct gains a field - it just did. */
static const vibeos_klog_sink_t g_serial_sink = {
    .name = "serial",
    .min_level = VIBEOS_LOG_INFO,
    .prefix = "[LOG]",
    .numbered = 1,
    .newline = 1,
    .guard_reentry = 0,
    .write = hw_serial_sink_write,
    .ctx = 0,
};

/* Who is logging, for the module's reentrancy guard: the running task, which
 * stays the same when the scheduler moves it to another core and which an
 * interrupt nested in it shares (a write nested in a write is recursion). Before
 * this core's per-CPU block exists - GS.base is still zero - or before it runs a
 * task, one number per core past the task slots.
 *
 * GS.base is read from the MSR rather than dereferenced, because an application
 * processor logs before its block is installed, and following a zero GS there
 * is how a previous diagnostic wrote through garbage into low memory. */
_Static_assert(VIBEOS_HW_MAX_TASKS + VIBEOS_HW_MAX_CPUS <= VIBEOS_KLOG_MAX_CONTEXTS,
               "the kernel log's reentrancy guard must tell every context apart");

static uint32_t hw_klog_context(void) {
    uint32_t lo, hi;
    uint32_t cpu = vibeos_x86_64_cpu_id() % VIBEOS_HW_MAX_CPUS;

    __asm__ __volatile__("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000101u));
    if (lo != 0u || hi != 0u) {
        int t = hw_this_cpu()->current_task;
        if (t >= 0 && t < VIBEOS_HW_MAX_TASKS) {
            return (uint32_t)t;
        }
    }
    return (uint32_t)VIBEOS_HW_MAX_TASKS + cpu;
}

void hw_klog_init(void) {
    vibeos_klog_set_lock(hw_klog_lock, hw_klog_unlock);
    vibeos_klog_set_context(hw_klog_context);
    vibeos_klog_reset();
    if (vibeos_klog_add_sink(&g_serial_sink) < 0) {
        hw_panic("kernel log: the serial sink did not register");
    }
}

void hw_log(vibeos_log_level_t level, uint32_t code, uint64_t a0,
                   uint64_t a1, const char *message) {
    vibeos_klog(level, code, a0, a1, message);
}

/* The ring, on demand, newest `want` entries, for the console's `log` command.
 * Bracketed as a whole so the listing arrives unbroken; the module takes its own
 * lock inside, one event at a time, which is the lock order it documents. */
void vibeos_x86_64_log_dump_recent(uint32_t want) {
    vibeos_x86_64_serial_lock();
    vibeos_klog_dump_recent(want, hw_serial_sink_write, 0);
    vibeos_x86_64_serial_unlock();
}

/* Walk the saved frame pointers and print the return addresses.
 *
 * A panic used to say what went wrong and nothing about how the machine got
 * there, which for a fault inside one of two hundred static helpers is most of
 * the question. The addresses are raw on purpose: almost everything in this
 * file is static, so naming them from the nearest preceding symbol is
 * frequently wrong - and a confidently wrong name costs more than a number.
 * scripts/dev/symbolize.py turns them into file and line with addr2line, which
 * has the debug info and does not guess.
 *
 * Every read is checked before it happens. This runs inside a fault handler,
 * so following a corrupt chain would fault again and the second fault would
 * replace the first one's evidence with a reset. */
#define HW_BT_MAX_FRAMES 16u
#define HW_BT_CODE_LO 0x4000000ull
#define HW_BT_CODE_HI 0x5000000ull

static int hw_bt_plausible_code(uint64_t addr) {
    return addr >= HW_BT_CODE_LO && addr < HW_BT_CODE_HI;
}

static int hw_bt_plausible_frame(uint64_t rbp, uint64_t prev) {
    if (rbp == 0 || (rbp & 7u) != 0) {
        return 0;   /* a frame pointer is always 8-aligned */
    }
    if (rbp < 0x1000ull || rbp >= 0x8000000000ull) {
        return 0;   /* not kernel memory */
    }
    /* Stacks grow down, so each caller's frame sits above the callee's. A
     * chain that goes backwards is a loop, and a loop here never ends. */
    return prev == 0 || rbp > prev;
}

void hw_backtrace(uint64_t rbp, uint64_t rip) {
    uint64_t prev = 0;
    uint32_t depth;

    /* The whole trace under one lock. It printed unlocked, so another core's
     * output landed between two frames of it - and a backtrace with a foreign
     * line in the middle is not a backtrace, it is a puzzle. */
    vibeos_x86_64_serial_lock();
    vibeos_x86_64_serial_puts("[BT] rip=0x");
    vibeos_x86_64_serial_print_hex(rip);
    vibeos_x86_64_serial_puts("\n");

    for (depth = 0; depth < HW_BT_MAX_FRAMES; depth++) {
        const uint64_t *frame;
        uint64_t ret;

        if (!hw_bt_plausible_frame(rbp, prev)) {
            break;
        }
        frame = (const uint64_t *)(uintptr_t)rbp;
        ret = frame[1];             /* [rbp+8] is the return address */
        if (!hw_bt_plausible_code(ret)) {
            break;
        }
        vibeos_x86_64_serial_puts("[BT]   #");
        vibeos_x86_64_serial_print_hex(depth);
        vibeos_x86_64_serial_puts(" 0x");
        vibeos_x86_64_serial_print_hex(ret);
        vibeos_x86_64_serial_puts("\n");
        prev = rbp;
        rbp = frame[0];             /* [rbp] is the caller's frame pointer */
    }
    vibeos_x86_64_serial_puts("[BT] end depth=0x");
    vibeos_x86_64_serial_print_hex(depth);
    vibeos_x86_64_serial_puts("\n");
    vibeos_x86_64_serial_unlock();
}

/* Set by hw_panic before it halts. Every core checks it on the way out of an
 * interrupt and parks itself.
 *
 * A panic used to stop one core. The message even said so - "halting on cpu 2" -
 * and it was read for weeks as though it said "halting". The other three cores
 * carried on, the log kept moving, and the machine went quiet some seconds
 * later at a place with no connection to the fault. That is the whole silent
 * wedge family: the evidence and the failure were separated by however long it
 * took the survivors to need the core that had died.
 *
 * Not an IPI, deliberately. Every core already takes a timer interrupt a
 * hundred times a second, so this needs no new vector, no acknowledgement
 * protocol and no timeout to get wrong. A core sitting in a syscall with
 * interrupts masked answers when it returns to ring 3; a core that never
 * returns was stuck with or without this. */
volatile int g_panicked;


/* Decision T7: no lock may be held across a scheduling point.
 *
 * The classic priority inversion - a low-priority task holding a lock a
 * high-priority task wants, while something in between runs - cannot happen
 * here in its usual form, because these spinlocks mask interrupts: a core
 * inside one never takes the timer, so it is never preempted while holding.
 *
 * What *can* happen is worse. A path that takes a lock and then blocks or
 * exits leaves the lock held by a task that is no longer running, and every
 * core that wants it spins with interrupts off, forever. That is not a task
 * being delayed; it is the machine stopping. The virtio-net wedge found this
 * evening was exactly that shape, one layer down.
 *
 * So the answer is not inheritance or a priority ceiling - both of which would
 * make the situation survivable - but making it impossible to enter, and
 * saying so when somebody tries.
 *
 * Counted rather than fatal, for the reason the transition table gives: a count
 * can be asserted across a whole boot, and a panic can only be met once, on
 * somebody else's investigation. */
void hw_sched_point(const char *where) {
    (void)where;
    /* Not implemented yet, and the first attempt is why.
     *
     * The check needs a per-core count of held locks, so the first version
     * incremented one in hw_spin_lock through hw_this_cpu(). That reads the
     * per-CPU pointer out of GS - and an application processor takes locks
     * before it installs its own area, so the increment wrote through a garbage
     * pointer into low memory. The boot died with a #GP at rip=0x39: the kernel
     * jumping to an address that is not code.
     *
     * A diagnostic that corrupts is worse than no diagnostic, which this
     * project has already learned three times over, so it came straight back
     * out rather than being guarded into working.
     *
     * What the next attempt needs is a core identity that is valid before the
     * per-CPU area exists. `vibeos_x86_64_cpu_id()` is exactly that - it reads
     * the initial APIC id from CPUID leaf 1, which is valid from the first
     * instruction - but CPUID is serialising and this would be on every lock,
     * so the honest options are a cheap per-core index established earlier in
     * bring-up, or counting only in the handful of layers that can block.
     *
     * The decision itself stands and is not in question: no lock may be held
     * across a scheduling point. Nothing in the kernel does it today. What is
     * missing is the check that would notice if something started. */
}

void hw_panic(const char *why) {
    /* The medium first, before anything else this function does.
     *
     * A panic is the one moment the on-disk log exists for, and it is also the
     * moment when the least is working: another core may hold any lock and the
     * scheduler is about to be parked. vibeos_klog_panic takes no lock - see
     * kernel/diag/klog.c - so this line can be written from here. It goes first
     * because every line after it is one more chance to stop before reaching
     * the disk. */
    vibeos_klog_panic(why);

    uint64_t rbp;
    uint64_t rip;

    __asm__ __volatile__("movq %%rbp, %0" : "=r"(rbp));
    /* The panic site itself, so the first line of the trace is this call and
     * not whatever the compiler left in a register. */
    __asm__ __volatile__("leaq (%%rip), %0" : "=r"(rip));

    vibeos_x86_64_serial_lock();
    vibeos_x86_64_serial_puts(" FATAL: ");
    vibeos_x86_64_serial_puts(why);
    vibeos_x86_64_serial_puts(", halting on cpu 0x");
    vibeos_x86_64_serial_print_hex((uint64_t)vibeos_x86_64_cpu_id());
    vibeos_x86_64_serial_puts("\n");

    hw_panic_cpu_summary();
    vibeos_x86_64_serial_unlock();

    hw_backtrace(rbp, rip);
    vibeos_klog_dump_unlocked(hw_serial_sink_write, 0);

    /* Announced after the dump, so the evidence is out before anything else
     * stops, and on one line because three other cores may still be writing. */
    g_panicked = 1;
    __asm__ __volatile__("sfence" ::: "memory");
    vibeos_x86_64_serial_lock();
    vibeos_x86_64_serial_puts("[PANIC] MACHINE_STOPPED every core parks on its "
                              "next interrupt\n");
    vibeos_x86_64_serial_unlock();

    for (;;) {
        __asm__ __volatile__("cli; hlt");
    }
}
/* A lock nobody released, reported instead of waited on forever.
 *
 * Everything is read from the lock before the panic, because hw_panic parks
 * every other core and the holder's identity is the whole message.
 *
 * The names are function names captured at acquisition rather than addresses.
 * Almost everything in this file is static, so an address resolves to whichever
 * symbol happens to precede it, and a confidently wrong name is worse than a
 * number: both of the wedges examined this week reported `hw_task_slots`, a
 * function that returns a constant.
 *
 * ## Written without taking the console lock, deliberately
 *
 * The first version of this used serial_puts, which locks. That deadlocked the
 * deadlock reporter: under svc-press the console lock is one of the jammed
 * ones, so the core that had detected the problem sat in
 * vibeos_x86_64_serial_lock and the message never appeared. The wedge report
 * caught it in the act - CPU#3 stopped inside this function.
 *
 * So it writes raw bytes. The output can interleave with another core's, and
 * that is the correct trade for the same reason the console lock's own bound
 * takes the lock rather than waiting: interleaving is detected by the boot
 * gate's first check, and silence is the failure where every piece of evidence
 * is missing rather than wrong. A lock reporter that can only speak when the
 * locks are healthy reports nothing on exactly the boots it exists for. */
static void hw_raw_puts(const char *s) {
    if (!s) {
        return;
    }
    while (*s) {
        if (*s == '\n') {
            vibeos_x86_64_serial_putc('\r');
        }
        vibeos_x86_64_serial_putc(*s++);
    }
}

static void hw_raw_hex(uint64_t v) {
    const char *d = "0123456789abcdef";
    int i;
    for (i = 60; i >= 0; i -= 4) {
        vibeos_x86_64_serial_putc(d[(v >> i) & 0xFull]);
    }
}

void hw_lock_deadlock(hw_lock_t *lock, const char *waiter,
                             const char *holder, int holder_cpu) {
    hw_raw_puts("\n[LOCK] DEADLOCK lock=0x");
    hw_raw_hex((uint64_t)(uintptr_t)lock);
    hw_raw_puts(" waiter_cpu=0x");
    hw_raw_hex((uint64_t)vibeos_x86_64_cpu_id());
    hw_raw_puts(" waiting_in=");
    hw_raw_puts(waiter ? waiter : "?");
    hw_raw_puts(" held_by_cpu=0x");
    hw_raw_hex((uint64_t)(holder_cpu < 0 ? 255 : holder_cpu));
    hw_raw_puts(" taken_in=");
    hw_raw_puts(holder ? holder : "(released, or never named)");
    hw_raw_puts("\n");
    hw_panic("spinlock held too long: deadlock");
}


void hw_panic_cpu_summary(void) {
    /* Who else was running what.
     *
     * A ring-3 fault gets a full crash record; a panic got a backtrace of the
     * one core that noticed and nothing about the other three. Every wedge
     * investigated in this project needed exactly this - which core was on
     * which task, and on whose address space - and the answer had to be dug
     * out of QEMU's monitor from the host afterwards, when it could have been
     * printed here for the cost of a loop.
     *
     * Read from the per-CPU blocks rather than stopping the cores: this is a
     * machine that is about to halt, and an IPI round trip is exactly the kind
     * of thing that hangs instead of reporting. A slightly stale line is worth
     * more than no line. */
    uint32_t i;
    if (g_relrace_curcpu >= 0) {
        vibeos_x86_64_serial_puts("[PANIC] release-race: caller_rip=0x");
        vibeos_x86_64_serial_print_hex(g_relrace_rip);
        vibeos_x86_64_serial_puts(" cur_cpu=0x");
        vibeos_x86_64_serial_print_hex((uint64_t)(int64_t)g_relrace_curcpu);
        vibeos_x86_64_serial_puts(" releasing_cpu=0x");
        vibeos_x86_64_serial_print_hex((uint64_t)(int64_t)g_relrace_relcpu);
        vibeos_x86_64_serial_puts(" slot=0x");
        vibeos_x86_64_serial_print_hex((uint64_t)(int64_t)g_relrace_slot);
        vibeos_x86_64_serial_puts("\n");
    }
    {
        for (i = 0; i < VIBEOS_HW_MAX_CPUS; i++) {
            int t;
            if (!g_cpus[i].online) {
                continue;
            }
            t = g_cpus[i].current_task;
            vibeos_x86_64_serial_puts("[PANIC] cpu=0x");
            vibeos_x86_64_serial_print_hex((uint64_t)i);
            vibeos_x86_64_serial_puts(" lapic=0x");
            vibeos_x86_64_serial_print_hex((uint64_t)g_cpus[i].lapic_id);
            vibeos_x86_64_serial_puts(" task=0x");
            vibeos_x86_64_serial_print_hex((uint64_t)(int64_t)t);
            if (t >= 0 && t < (int)VIBEOS_HW_MAX_TASKS) {
                vibeos_x86_64_serial_puts(" pid=0x");
                vibeos_x86_64_serial_print_hex((uint64_t)hw_task_pid_of(&g_tasks[t]));
                vibeos_x86_64_serial_puts(" state=0x");
                vibeos_x86_64_serial_print_hex((uint64_t)hw_slot_state(t));
                vibeos_x86_64_serial_puts(" cr3=0x");
                vibeos_x86_64_serial_print_hex(g_tasks[t].cr3);
                vibeos_x86_64_serial_puts(" exe=");
                vibeos_x86_64_serial_puts(g_tasks[t].proc.exe_path[0]
                                          ? g_tasks[t].proc.exe_path
                                          : "(none)");
            }
            vibeos_x86_64_serial_puts("\n");
        }
    }
}

/* Everything known about a page that vanished under a running process.
 *
 * Armed on one signature only: a ring-3 *instruction fetch* on a page that is
 * not present. That is narrow enough to cost nothing on a healthy boot - an
 * earlier, broader version of this dump perturbed the timing enough to change
 * the failure rate, which is its own kind of lying - and it is the exact shape
 * of the defect in boot_repeatability.md: cr2 equal to rip, inside the page the
 * program was already executing from.
 *
 * The walk answers which level is missing. The frame questions answer the one
 * the walk cannot: whether the page table the CPU is using has been handed back
 * to the allocator while a task still runs on it. A leaf that is gone is a lost
 * mapping; a PML4 that is *free* is an address space destroyed under its owner,
 * and those are different defects with different fixes. */
void hw_dump_vanished(uint64_t va) {
    static const uint32_t shifts[4] = {39u, 30u, 21u, 12u};
    uint64_t e[4] = {0, 0, 0, 0};
    uint64_t cr3 = hw_read_cr3() & 0x000FFFFFFFFFF000ull;
    uint64_t *tbl;
    uint32_t level;
    int depth = 0;

    tbl = (uint64_t *)(uintptr_t)cr3;
    for (level = 0; level < 4u; level++) {
        uint64_t next;
        e[level] = tbl[(va >> shifts[level]) & 0x1FFu];
        depth = (int)level + 1;
        if ((e[level] & PTE_PRESENT) == 0u || (e[level] & PTE_PS) != 0u) {
            break;
        }
        if (level == 3u) {
            break;
        }
        next = e[level] & 0x000FFFFFFFFFF000ull;
        if (next + 4096ull > VIBEOS_HW_IDENTITY_LIMIT) {
            break;
        }
        tbl = (uint64_t *)(uintptr_t)next;
    }

    vibeos_x86_64_serial_lock();
    vibeos_x86_64_serial_puts("[MM] VANISHED va=0x");
    vibeos_x86_64_serial_print_hex(va);
    vibeos_x86_64_serial_puts(" cr3=0x");
    vibeos_x86_64_serial_print_hex(cr3);
    vibeos_x86_64_serial_puts(" stopped_at=0x");
    vibeos_x86_64_serial_print_hex((uint64_t)depth);
    vibeos_x86_64_serial_puts(" pml4e=0x");
    vibeos_x86_64_serial_print_hex(e[0]);
    vibeos_x86_64_serial_puts(" pdpte=0x");
    vibeos_x86_64_serial_print_hex(e[1]);
    vibeos_x86_64_serial_puts(" pde=0x");
    vibeos_x86_64_serial_print_hex(e[2]);
    vibeos_x86_64_serial_puts(" pte=0x");
    vibeos_x86_64_serial_print_hex(e[3]);
    /* The decisive pair: if the frame holding this CR3 is free, or owned by
     * nobody, the address space was destroyed while its task kept running. */
    vibeos_x86_64_serial_puts(" cr3_state=0x");
    vibeos_x86_64_serial_print_hex((uint64_t)vibeos_frame_state(cr3));
    vibeos_x86_64_serial_puts(" cr3_owners=0x");
    vibeos_x86_64_serial_print_hex((uint64_t)vibeos_frame_owners(cr3));
    vibeos_x86_64_serial_puts(" slot=0x");
    vibeos_x86_64_serial_print_hex((uint64_t)g_current_task);
    vibeos_x86_64_serial_puts(" gen=0x");
    vibeos_x86_64_serial_print_hex((uint64_t)(g_current_task >= 0 ?
        vibeos_task_generation((uint32_t)g_current_task) : 0u));
    vibeos_x86_64_serial_puts(" cr3_by=");
    vibeos_x86_64_serial_puts((g_current_task >= 0 &&
                               g_tasks[g_current_task].cr3_set_by) ?
                              g_tasks[g_current_task].cr3_set_by : "-");
    vibeos_x86_64_serial_puts(" aspace_by=");
    vibeos_x86_64_serial_puts((g_current_task >= 0 &&
                               g_tasks[g_current_task].aspace_killed_by) ?
                              g_tasks[g_current_task].aspace_killed_by : "-");
    vibeos_x86_64_serial_puts(" last_destroy=");
    vibeos_x86_64_serial_puts(g_last_destroy_why ? g_last_destroy_why : "-");
    vibeos_x86_64_serial_puts(" of=0x");
    vibeos_x86_64_serial_print_hex(g_last_destroy_pml4);
    vibeos_x86_64_serial_puts(" by_cpu=0x");
    vibeos_x86_64_serial_print_hex((uint64_t)g_last_destroy_cpu);
    vibeos_x86_64_serial_puts("\n");
    vibeos_x86_64_serial_unlock();
}

/* Print the most recent crash in full. Reached from the kernel console's
 * `crash` command; safe to call at any time, including when nothing has
 * crashed - saying so plainly is more useful than printing an empty record. */
void vibeos_x86_64_crash_dump(void) {
    vibeos_x86_64_serial_lock();
    vibeos_crash_dump(hw_serial_sink_write, 0);
    vibeos_x86_64_serial_unlock();
}
