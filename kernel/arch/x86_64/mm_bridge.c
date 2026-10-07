/* The memory bridge: where the portable memory manager (kernel/mm) meets this
 * machine.
 *
 * The kernel's own page tables and the identity map, the frame pool and its
 * bring-up from the firmware map, the TLB quarantine that holds an unmapped
 * frame until every core has provably dropped it, allocation with admission,
 * the locks each portable layer is given, the vmspace backend, the swap
 * bridges, and the page cache wired to the filesystem.
 *
 * Lifted out of arch_hw.c whole (2026-09-28). Nearly every memory defect of the
 * reclaim work (M-056..M-073) was fixed in these lines while they sat inside a
 * 6,600-line file whose functions are almost all static - which is what makes
 * nearest-preceding-symbol names from a hung core wrong. Nothing here changed
 * in the move; what crosses to arch_hw.c is declared in arch_hw_internal.h,
 * under "the memory bridge". */

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

/* kernel.ld: the whole loaded image, text through bss, page-aligned. */
extern const char __kernel_image_start[];
extern const char __kernel_image_end[];

/* Used above their definitions. */
static void hw_tlb_shootdown(uint64_t cr3);
static int hw_frame_still_mapped(uint64_t phys, uint32_t *out_pid, uint32_t *out_mappers, uint64_t *out_va);
static void hw_tlbq_put(uint64_t root, uint64_t phys);
static int hw_swap_bring_in(vibeos_vmspace_t *sv, uint64_t va);
static int hw_cache_read(void *ctx, uint32_t file_id, uint64_t offset,
                         uint64_t phys);
static void hw_cache_audit(uint64_t *out_checked, uint64_t *out_bad);

/* ---- Paging ------------------------------------------------------------- */

#define PTE_PWT     0x008ull            /* write-through */
#define PTE_PCD     0x010ull            /* cache disable; with PWT gives UC- */

/* x86-64 leaves bits 9, 10 and 11 of a page-table entry to software, and this
 * kernel now uses two of them for different questions: PTE_COW says this page
 * is shared and must be duplicated before it is written, and VIBEOS_PTE_OWNED
 * says this address space holds a reference to the frame.
 *
 * They were briefly the same bit. Every mapped page then read as
 * copy-on-write, so a write fault on a genuinely read-only page would have
 * been resolved by granting the write rather than killing the process - and
 * sixteen clean boots showed nothing, because the two only disagree on a path
 * that needs a fault on a page a program is not allowed to write. Whoever
 * takes the third bit gets told at compile time instead. */
_Static_assert((PTE_COW & VIBEOS_PTE_OWNED) == 0ull,
               "the copy-on-write bit and the ownership bit must be different bits");
#define VIBEOS_HW_IDENTITY_GIB 4u       /* identity-map the first 4 GiB */


/* The `syscall` trampoline (isr.S) stashes the user rsp and loads a kernel
 * stack itself; both live in the per-CPU block reached through GS.base
 * (hw_cpu_t fields at offsets 8 and 0).
 */

/* Kernel-owned page tables (static BSS, no PMM dependency during bring-up).
 * The identity map is supervisor-only; user memory lives in its own PML4 slot
 * with per-process tables, so ring 3 cannot reach kernel pages. */
uint64_t g_pml4[512] __attribute__((aligned(4096)));
static uint64_t g_pdpt[512] __attribute__((aligned(4096)));
static uint64_t g_pd[VIBEOS_HW_IDENTITY_GIB][512] __attribute__((aligned(4096)));

/* ---- Page pool + per-process address spaces ------------------------------ */

/* Page frames come from the real physical memory manager, initialized from the
 * firmware memory map. The small static pool is only a fallback for boot paths
 * that hand us no boot_info (e.g. a direct -kernel load). */
#define VIBEOS_HW_POOL_PAGES 32u
static uint8_t g_page_pool[VIBEOS_HW_POOL_PAGES][4096] __attribute__((aligned(4096)));
static uint32_t g_pool_next;

/* Staging buffer for an image being exec'd.
 *
 * A real program is not small - BusyBox is about two megabytes - and putting
 * that in .bss would add it to every kernel image whether or not anything ever
 * execs. It is taken from the page allocator instead, once, at boot, and a
 * small static buffer remains as the fallback for the early paths that run
 * before the allocator exists. */
/* Six megabytes smaller than it was, and worth recording why it took so long.
 *
 * Nothing needs the old size: the parse takes a header window and the page
 * fill reads through the cache, both since I3. But shrinking these two
 * windows turned the boot red with mm_poison_hits=3019 on every run - which
 * read as this project's long-running one-boot-in-sixteen use-after-free,
 * finally reproducible, and held the shrink up for a phase.
 *
 * It was not a use-after-free at all. vibeos_frame_init never initialised the
 * descriptors' flags byte, and frame_push_free preserves the was-freed mark by
 * design - so on a table full of the bump allocator's leftovers, every frame
 * whose stale byte had bit 0x10 set came up claiming a release that never
 * happened, and the poison check judged its virgin contents. These sizes decide
 * where that table lands in physical memory, which is the entire mechanism
 * behind "shrinking either window alone is clean, both together fail".
 *
 * The detector was wrong and the memory manager was right, which is the
 * opposite of what three earlier readings assumed. It was closed by making the
 * report say which frame, which word, what value and who released it: the
 * answer came back "all zero, released by nobody", and a frame nobody released
 * is not a use-after-free.
 *
 * The original note follows.
 *
 * A header window, not a copy of the program.
 *
 * This was four megabytes because hw_read_file_cached would not proceed
 * without room for the whole file - a refusal about the buffer applied to a
 * file the cache did not need a buffer for. With the length separated from the
 * staged byte count, what has to be here is the ELF header and the program
 * headers, and nothing else: the page fill and the interpreter path both read
 * through the cache.
 *
 * 64 KiB against a real extent of a few hundred bytes. Sized generously
 * because the cost is now nothing and the failure mode of being too small is a
 * refusal to run a program - and refused *by name* rather than by truncation,
 * which is the distinction the whole phase is about. */
#define VIBEOS_HW_EXEC_STAGE_BYTES (64u * 1024u)
static uint8_t g_exec_elf_static[65536] __attribute__((aligned(16)));
uint8_t *g_exec_elf = g_exec_elf_static;
uint32_t g_exec_elf_cap = (uint32_t)sizeof(g_exec_elf_static);

/* And a second one, for a dynamic program's interpreter.
 *
 * It cannot share the buffer above: loading a dynamic program means having
 * both images in memory at once, because the interpreter is mapped into the
 * same address space as the program that named it.
 *
 * It used to be two megabytes because musl's loader is its C library and is
 * about seven hundred kilobytes - back when this held the image. It holds the
 * interpreter's headers now, for the same reason as the window above. */
/* Same reasoning as the exec window above. */
#define VIBEOS_HW_INTERP_STAGE_BYTES (64u * 1024u)
uint8_t *g_interp_elf;
uint32_t g_interp_elf_cap;

/* How many address spaces map each physical frame.
 *
 * fork() no longer copies pages; it maps the parent's frames into the child
 * read-only and marks both copies as copy-on-write. A frame may therefore be
 * live in several address spaces at once, and freeing it when the first of
 * them exits would hand a running process's memory to the allocator.
 *
 * One byte per frame over the allocator's region, taken from that region at
 * boot. A count of zero means "one owner" so that the ordinary case needs no
 * bookkeeping at all: only sharing writes here. The count saturates rather
 * than wrapping - at 255 owners the frame is simply never reclaimed, which
 * leaks a page instead of freeing memory somebody is still using. */
/* Reference counting lives in kernel/mm/frame.c now.
 *
 * What used to be here was one byte per frame in a side table, incremented with
 * a compare-exchange from nine call sites. It was correct arithmetic over an
 * incomplete picture: a frame the table did not describe answered "nobody owns
 * me", which reads as "free it". That default is what let one defect survive
 * four fixes, and it is reversed in the new layer - see docs/mm/.
 *
 * These three names stay as wrappers for now, because renaming nine call sites
 * and changing what they mean in the same commit is how the last four attempts
 * went. P1 step 4 replaces the names; this commit only moves where the
 * arithmetic happens.
 *
 * The layer is not internally locked - a host test has one thread - so the
 * memory lock is taken here, on the arch side, exactly where it was taken
 * before for the free list. */

/* There is deliberately no hw_page_get any more.
 *
 * Taking a reference on a user frame happens in exactly one place -
 * vibeos_vmspace_map, at the moment the mapping is installed - and the compiler
 * says so: the wrapper that used to exist here became unused the instant the
 * mapping functions moved into L1. That is the invariant this phase exists to
 * establish, and an unused-function warning is a cheap way to keep it. */

/* Returns non-zero when the last owner let go. Unlike the old one, that also
 * means the frame has *already* gone back to the free list, poisoned: releasing
 * and reclaiming are one operation now, so there is no window in which a frame
 * is owned by nobody and still not free. Every caller that used to follow this
 * with hw_free_page has had that call removed. */
/* No locking here any more. The frame layer takes the memory lock itself, via
 * the hooks installed at bring-up.
 *
 * It was taken at the call sites for one release, and that lasted exactly
 * until something new touched a frame: the copy-on-write fault moved into the
 * address-space layer, allocated without it, and two cores resolving a fault
 * at the same moment corrupted the free list. The boot wedged silently.
 * "Remember to hold the memory lock" is not a property a compiler checks, and
 * a layer several cores can drive has to defend itself. */
int hw_page_put(uint64_t phys) {
    return vibeos_frame_put(phys);
}

static uint32_t hw_page_owners(uint64_t phys) {
    return vibeos_frame_owners(phys);
}

/* Region descriptors for every process.
 *
 * Static and shared: a per-process pool would waste most of it, and a global
 * one turns "too many regions" into a refusal from mmap - which is what a
 * program can handle - rather than into an allocation failure somewhere less
 * convenient. Sixteen per task is generous for what runs here; the boot gate
 * asserts the count returns to zero. */
#define VIBEOS_HW_VMA_ENTRIES 2048u
static vibeos_vma_t g_vma_pool[VIBEOS_HW_VMA_ENTRIES];

/* The page cache, and the small table that gives a path an identity.
 *
 * Every exec reads a whole program off the disk, so a shell that runs twenty
 * commands reads twenty programs it has already seen - and the read happens
 * under g_exec_lock with interrupts masked, which is why a 2 MiB FAT read was
 * once indistinguishable from a hang. Caching by (file, offset) makes the
 * second read free.
 *
 * 3072 pages is twelve megabytes held out of the four hundred this machine
 * has. A boot reads about eleven megabytes of programs, and a table smaller
 * than that thrashes: the first size tried was 768 and every program evicted
 * the last one, so the hit count stayed in single figures. */
#define VIBEOS_HW_CACHE_ENTRIES 3072u

/* The swap map's bitmap, sized for the largest area this kernel would accept.
 * Static because the swap layers never allocate - they are called when memory
 * is short, and a layer that allocates on that path fails exactly when it is
 * needed. 8192 slots is 32 MiB of swap, which is a starting point rather than
 * a limit: it is one kilobyte of bitmap. */
uint8_t g_swap_bitmap[(VIBEOS_HW_SWAP_SLOTS + 7u) / 8u];
static vibeos_cache_entry_t g_cache_table[VIBEOS_HW_CACHE_ENTRIES];
hw_cached_file_t g_cached_files[VIBEOS_HW_CACHE_FILES];
uint32_t g_cached_file_count;

vibeos_pmm_t g_hw_pmm;
/* The bump allocator is now a bootstrap stage, not a running allocator: it
 * serves the early page tables and the staging buffers, and is closed the
 * moment the frame layer takes over the region. Nothing may allocate from it
 * afterwards - see hw_pmm_bringup for why that is a correctness rule and not a
 * tidiness one. */
static int g_hw_pmm_ready;

/* vibeos_hw_aspace_t now lives in arch_hw_internal.h. */

uint64_t hw_read_cr3(void) {
    uint64_t v;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(v));
    return v;
}

void hw_write_cr3(uint64_t pml4_phys) {
    uint32_t me = vibeos_x86_64_cpu_id();

    /* Announce the table before loading it, with a full fence between: the
     * quarantine clears an entry and then reads this, and this core publishes
     * and then walks the tables, so one of the two always sees the other -
     * either the quarantine sees this core coming, or this core's walk sees the
     * entry already gone. */
    if (me < VIBEOS_HW_MAX_CPUS) {
        __atomic_store_n(&g_cpus[me].loading_cr3, pml4_phys & ~0xFFFull,
                         __ATOMIC_SEQ_CST);
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
    }
    __asm__ __volatile__("mov %0, %%cr3" : : "r"(pml4_phys) : "memory");
    /* After the write, not before: the count means "this core has flushed",
     * and a reader that sees the new value must be able to conclude the flush
     * already happened. `loaded` likewise - until here the old table's entries
     * may still be cached. */
    if (me < VIBEOS_HW_MAX_CPUS) {
        __atomic_store_n(&g_cpus[me].loaded_cr3, pml4_phys & ~0xFFFull,
                         __ATOMIC_RELEASE);
        __atomic_add_fetch(&g_cpus[me].cr3_generation, 1ull,
                           __ATOMIC_RELEASE);
    }
}

/* ---- deferred release of unmapped frames --------------------------------
 *
 * The defect this closes, stated plainly: munmap clears the entry, invalidates
 * *this* core's translation, and puts the frame back on the free list. A thread
 * of the same process on another core can still hold the old translation, so it
 * can write into a frame the allocator has already handed to somebody else.
 *
 * A synchronous shootdown is the obvious fix and was measured to be the wrong
 * one. It was tried here and made the boot worse: two runs in twenty-four
 * failed with `tlb_acks below shootdowns`, a core that never answered.
 * `syscall` clears IF (SFMASK is 0x200), so a core inside a syscall cannot take
 * the IPI until it returns to ring 3, and munmap is called far more often than
 * fork - the stress run alone calls it a hundred and twenty times. Trading a
 * rare correctness gap for a frequent stall is the wrong trade.
 *
 * So this waits instead of asking. It needs no IPI, no rendezvous and no stall:
 *
 *   - the unmapping core has already done invlpg, so it is quiescent at once;
 *   - any *other* core that loads CR3 flushes its whole TLB, because this
 *     kernel enables neither PCID nor global pages - both checked, and both
 *     recorded on cr3_generation because the argument dies if either changes;
 *   - every context switch loads CR3 unconditionally in hw_task_load_cpu_state,
 *     and the timer preempts, so every core's generation advances on its own.
 *
 * A frame is therefore released once every other core's generation has moved
 * past the value it had when the frame was unmapped. That is the same shape as
 * dead_kstack_base above - a resource parked until the core that could still be
 * using it demonstrably is not - which is why this is a pattern here rather
 * than an invention.
 *
 * Only a core that has the address space loaded can hold a translation from it,
 * so a frame unmapped from a space no other core has loaded is released at once
 * and never parked (M-061). That is nearly every unmap - a single-threaded
 * process - and it matters: svc-press unmapping 64 MiB in a loop filled all
 * 512 slots faster than the other cores flushed, and every frame past that was
 * leaked, 2,486 of them. The quarantine is for the case it was written for, a
 * sibling thread on another core, and waits only on the cores that had the
 * space.
 *
 * On overflow the frame is leaked, not released (H-015) - see hw_tlbq_put. */
#define HW_TLBQ_SLOTS 512u

/* A core that had nothing of this space loaded when the frame was parked: it
 * has nothing to flush, and the drain does not wait for it. */
#define HW_TLBQ_NOT_HOLDING (~0ull)

typedef struct {
    uint64_t phys;
    uint64_t gen[VIBEOS_HW_MAX_CPUS];
    uint32_t owner;               /* the core that unmapped; already quiescent */
    uint32_t used;
} hw_tlbq_entry_t;

static hw_lock_t g_tlbq_lock;
static hw_tlbq_entry_t g_tlbq[HW_TLBQ_SLOTS];
/* How many frames are waiting, as an atomic, so the timer can ask without
 * taking the lock in an interrupt handler.
 *
 * It exists because of a hole the first version of this had, and the hole was
 * in exactly the case the defect lives in. hw_schedule returns early when the
 * next task is the current one, so a core running a single thread never
 * reloads CR3 and never becomes quiescent - and a sibling thread spinning on
 * another core is *both* the thing holding the stale translation and the thing
 * that would never advance. Frames would pile up until the quarantine
 * overflowed and fell back to the racy release, precisely under the workload
 * this was written for.
 *
 * So when anything is waiting, every core flushes on its next tick. The cost is
 * paid only while frames are actually parked, and a TLB flush per tick is what
 * a context switch already does on a kernel with no PCID. */
static volatile uint64_t g_tlbq_live;
static uint64_t g_tlbq_deferred;      /* frames that took the safe path */
static uint64_t g_tlbq_released;      /* frames the drain has since freed */
static uint64_t g_tlbq_overflow;      /* frames freed at once, gap still open */
static uint64_t g_tlbq_live_peak;
static uint64_t g_tlbq_immediate;     /* frames no other core could reach */

/* Has every core except `owner` flushed since the snapshot was taken? */
static int hw_tlbq_quiescent(const hw_tlbq_entry_t *e) {
    uint32_t c;
    for (c = 0; c < VIBEOS_HW_MAX_CPUS; c++) {
        if (c == e->owner || !g_cpus[c].online ||
            e->gen[c] == HW_TLBQ_NOT_HOLDING) {
            continue;
        }
        if (__atomic_load_n(&g_cpus[c].cr3_generation, __ATOMIC_ACQUIRE)
            == e->gen[c]) {
            return 0;
        }
    }
    return 1;
}

/* Release everything that has become safe. Called from the unmap path and from
 * the timer, so a quiet machine still drains. */
void hw_tlbq_drain(void) {
    uint32_t i;
    uint64_t ready[32];
    uint32_t n = 0;

    hw_spin_lock(&g_tlbq_lock);
    for (i = 0; i < HW_TLBQ_SLOTS && n < 32u; i++) {
        if (g_tlbq[i].used && hw_tlbq_quiescent(&g_tlbq[i])) {
            ready[n++] = g_tlbq[i].phys;
            g_tlbq[i].used = 0;
        }
    }
    g_tlbq_released += n;
    hw_spin_unlock(&g_tlbq_lock);
    if (n) {
        __atomic_sub_fetch(&g_tlbq_live, (uint64_t)n, __ATOMIC_RELEASE);
    }

    /* Outside the lock. vibeos_frame_put takes the frame layer's lock, and a
     * diagnostic that called a public accessor from inside its own lock is how
     * a previous investigation deadlocked the machine it was explaining. */
    for (i = 0; i < n; i++) {
        (void)vibeos_frame_put(ready[i]);
    }
}

/* How many references to `phys` the quarantine holds - the vmspace audit's
 * `quarantined` hook. Under the quarantine's own lock; nothing it calls takes
 * another. */
static uint32_t hw_tlbq_count(uint64_t phys) {
    uint32_t i, n = 0;

    hw_spin_lock(&g_tlbq_lock);
    for (i = 0; i < HW_TLBQ_SLOTS; i++) {
        if (g_tlbq[i].used && g_tlbq[i].phys == phys) {
            n++;
        }
    }
    hw_spin_unlock(&g_tlbq_lock);
    return n;
}

/* Could any core other than this one still hold a translation from `root`?
 * Called after the entry has been cleared - the clear is a locked
 * compare-exchange, a full barrier - so a core that has not announced the space
 * by now will walk tables that no longer have the entry. */
static int hw_tlbq_other_holders(uint64_t root, uint64_t gen[VIBEOS_HW_MAX_CPUS]) {
    uint32_t c, me = vibeos_x86_64_cpu_id();
    int holders = 0;

    root &= ~0xFFFull;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    for (c = 0; c < VIBEOS_HW_MAX_CPUS; c++) {
        /* The generation first: a core that is seen holding the space must
         * flush *after* this sample, and one seen not holding it needs none. */
        uint64_t g = __atomic_load_n(&g_cpus[c].cr3_generation, __ATOMIC_ACQUIRE);

        gen[c] = HW_TLBQ_NOT_HOLDING;
        if (c == me || !g_cpus[c].online) {
            continue;
        }
        if (__atomic_load_n(&g_cpus[c].loading_cr3, __ATOMIC_ACQUIRE) == root ||
            __atomic_load_n(&g_cpus[c].loaded_cr3, __ATOMIC_ACQUIRE) == root) {
            gen[c] = g;
            holders++;
        }
    }
    return holders;
}

/* Release `phys`, unmapped from `root`, as soon as that is safe. Returns 0 if
 * it was released at once, 1 if parked, -1 if the quarantine was full and it
 * was leaked. Counts nothing: hw_tlbq_put counts what munmap did, and the boot
 * self-test calls this directly so its two frames are not mistaken for
 * munmap's. */
static int hw_tlbq_place(uint64_t root, uint64_t phys) {
    uint32_t i, c, live = 0;
    int placed = 0;
    uint64_t gen[VIBEOS_HW_MAX_CPUS];

    if (hw_tlbq_other_holders(root, gen) == 0) {
        (void)vibeos_frame_put(phys);
        return 0;
    }

    hw_spin_lock(&g_tlbq_lock);
    for (i = 0; i < HW_TLBQ_SLOTS; i++) {
        if (g_tlbq[i].used) {
            live++;
            continue;
        }
        if (placed) {
            continue;
        }
        for (c = 0; c < VIBEOS_HW_MAX_CPUS; c++) {
            g_tlbq[i].gen[c] = gen[c];
        }
        g_tlbq[i].phys = phys;
        g_tlbq[i].owner = vibeos_x86_64_cpu_id();
        g_tlbq[i].used = 1;
        placed = 1;
        live++;
    }
    if (placed) {
        __atomic_add_fetch(&g_tlbq_live, 1ull, __ATOMIC_RELEASE);
    }
    if ((uint64_t)live > g_tlbq_live_peak) {
        g_tlbq_live_peak = (uint64_t)live;
    }
    hw_spin_unlock(&g_tlbq_lock);

    /* On overflow the frame is leaked, not released (H-015). The old code called
     * vibeos_frame_put here - the racy release the quarantine exists to replace:
     * another core can still hold a stale TLB entry for this page, so recycling
     * it now is a use-after-free. A frame never reclaimed is never handed out,
     * so leaking is safe in every caller context regardless of the cr3 loaded.
     * The count above (g_tlbq_overflow) is the measure of it, and the boot gate
     * asserts it is zero - so normal operation loses nothing, and only an
     * adversarial munmap of more than HW_TLBQ_SLOTS not-yet-quiescent pages
     * reaches this, losing memory rather than corrupting it. Since M-061 that
     * takes a burst of munmap by a process with a sibling thread running on
     * another core - a single-threaded process never parks anything. */
    return placed ? 1 : -1;
}

static void hw_tlbq_put(uint64_t root, uint64_t phys) {
    int r = hw_tlbq_place(root, phys);

    if (r == 0) {
        __atomic_add_fetch(&g_tlbq_immediate, 1ull, __ATOMIC_RELAXED);
    } else if (r > 0) {
        __atomic_add_fetch(&g_tlbq_deferred, 1ull, __ATOMIC_RELAXED);
    } else {
        __atomic_add_fetch(&g_tlbq_overflow, 1ull, __ATOMIC_RELAXED);
    }
}

/* Both halves of the quarantine's decision, constructed rather than hoped for.
 *
 * Whether munmap parks anything in a given boot depends on whether a sibling
 * thread happened to be running on another core at that instant: two boots in
 * ten parked nothing, correctly, and the gate that asserted a non-zero count
 * went red on a machine doing the right thing (M-061). So the boot builds the
 * two situations itself, on the real per-core fields:
 *
 *   - a frame released against a space another core has loaded must be
 *     parked, and must drain once that core has flushed;
 *   - a frame released against a space nobody has loaded must go back at once.
 *
 * Run after userland, when every other core has loaded a table through
 * hw_write_cr3 and is idling with its timer on. */
void hw_tlbq_selftest(void) {
    uint32_t c, me = vibeos_x86_64_cpu_id();
    uint64_t held_root = 0, f, spins = 0;
    int held = -2, unheld = -2, drained = 0, attempt;

    for (attempt = 0; attempt < 8 && held != 1; attempt++) {
        held_root = 0;
        for (c = 0; c < VIBEOS_HW_MAX_CPUS; c++) {
            if (c != me && g_cpus[c].online &&
                __atomic_load_n(&g_cpus[c].loaded_cr3, __ATOMIC_ACQUIRE) != 0u) {
                held_root = __atomic_load_n(&g_cpus[c].loaded_cr3, __ATOMIC_ACQUIRE);
                break;
            }
        }
        if (held_root == 0u) {
            break;          /* nobody else has loaded anything: cannot run */
        }
        f = vibeos_frame_alloc(VIBEOS_FRAME_ALLOCATED);
        if (f == 0u) {
            break;
        }
        held = hw_tlbq_place(held_root, f);
        if (held == 1) {
            /* The drain releases it once the holder's generation moves, which
             * its own timer tick does while anything is parked. */
            while (hw_tlbq_count(f) != 0u && ++spins < 5000000ull) {
                hw_tlbq_drain();
                __asm__ __volatile__("pause" ::: "memory");
            }
            drained = (hw_tlbq_count(f) == 0u);
        }
    }

    /* A root no core can have loaded: a frame that has never been a table. */
    {
        uint64_t never_a_root = vibeos_frame_alloc(VIBEOS_FRAME_ALLOCATED);

        f = vibeos_frame_alloc(VIBEOS_FRAME_ALLOCATED);
        if (never_a_root != 0u && f != 0u) {
            unheld = hw_tlbq_place(never_a_root, f);
        }
        if (never_a_root != 0u) {
            (void)vibeos_frame_put(never_a_root);
        }
    }

    vibeos_x86_64_serial_lock();
    vibeos_x86_64_serial_puts("[MM] TLBQ_SELFTEST held=");
    vibeos_x86_64_serial_puts(held == 1 ? "parked" : held == 0 ? "released" :
                              held == -1 ? "leaked" : "unavailable");
    vibeos_x86_64_serial_puts(" drained=");
    vibeos_x86_64_serial_puts(drained ? "1" : "0");
    vibeos_x86_64_serial_puts(" unheld=");
    vibeos_x86_64_serial_puts(unheld == 0 ? "released" : unheld == 1 ? "parked" :
                              unheld == -1 ? "leaked" : "unavailable");
    vibeos_x86_64_serial_puts("\n");
    vibeos_x86_64_serial_unlock();
}

/* Called from every core's timer tick. If anything is parked, flush - see the
 * note on g_tlbq_live for why a core that keeps running one thread would
 * otherwise never become quiescent, in exactly the case that matters. */
void hw_tlbq_help_quiesce(void) {
    if (__atomic_load_n(&g_tlbq_live, __ATOMIC_ACQUIRE) != 0ull) {
        hw_write_cr3(hw_read_cr3());
    }
}

uint64_t vibeos_x86_64_tlbq_deferred(void) { return g_tlbq_deferred; }
uint64_t vibeos_x86_64_tlbq_released(void) { return g_tlbq_released; }
uint64_t vibeos_x86_64_tlbq_overflow(void) { return g_tlbq_overflow; }
uint64_t vibeos_x86_64_tlbq_live_peak(void) { return g_tlbq_live_peak; }
uint64_t vibeos_x86_64_tlbq_immediate(void) {
    return __atomic_load_n(&g_tlbq_immediate, __ATOMIC_RELAXED);
}

/* How many words of a reclaimed page to check before handing it out again.
 * Sampled rather than exhaustive: the loop that zeroes the page is already the
 * cost of an allocation, and a writer that corrupts a free page almost never
 * touches only one word of it. */
#define HW_POISON_PROBES 16u

/* Counts frees, so only every sixty-fourth pays for the walk above. */
static uint32_t g_free_seq;

/* The address space being torn down right now, if any.
 *
 * hw_aspace_destroy runs before the dying task is marked ZOMBIE - deliberately,
 * so a parent reaping the slot cannot pull the tables out from under it - so a
 * walk of "live" tasks finds the dying task still holding every frame being
 * freed. The first version of the check below reported that as a defect on
 * every boot, eighty times over. It was reporting the teardown doing its job.
 *
 * A detector that fires constantly is worse than none: it is a hundred lines
 * of noise between whoever is looking and the one line that matters. */
static const uint64_t *g_aspace_being_destroyed;

/* Every free records who did it, in the page itself.
 *
 * The premature-free family has now been diagnosed twice from the far end and
 * fixed once, and it came back: a page still mapped by a live process turns up
 * on the freelist, and the poison says *that* it happened but not *who*. The
 * freelist link occupies the first word of a reclaimed page; the second is
 * free, so the caller's name goes there and costs nothing.
 *
 * When a poisoned page is handed out again with the poison disturbed, or when
 * anything else finds this pattern where data should be, the tag names the
 * last function to release it. */
/* Is a live process still mapping the frame that is about to be reclaimed?
 *
 * The same question hw_free_page_why has always asked, moved to where the
 * frees actually happen. Since the address-space layer arrived, almost nothing
 * reaches a frame's last release through hw_free_page: vibeos_vmspace_unmap
 * and vibeos_vmspace_destroy release directly, and the detector sat in a path
 * nothing walks - so its silence was being read as evidence when it was only
 * absence.
 *
 * Sampled one release in sixteen. The walk is expensive; the bug appears about
 * four boots in ten, so one in sixteen still meets it within a run. */
static void hw_frame_release_watch(uint64_t phys, uint32_t handouts) {
    uint32_t pid = 0;
    /* The count that triggered the report, kept.
     *
     * The first version re-read the owner count when printing it, so the
     * message showed mappers=1 owners=1 - numbers that cannot have triggered
     * anything - because the frame had been legitimately taken in between.
     * A report that does not show the values it fired on is unreadable, and
     * this is the third time in this subsystem that reading state twice and
     * reporting the second read has produced a confusing message. */
    uint32_t owners_at_check = 0;
    uint32_t mappers = 0;
    uint64_t mapped_va = 0;

    /* Sampled, and the rate is a named constant rather than a literal.
     *
     * It has been raised for an investigation and put back twice now, each time
     * by editing the mask in place, which leaves no way to tell from the source
     * whether a zero came from a full sweep or a sixteenth of one. A defect
     * that happens once a boot has a small chance of being seen by a check that
     * looks at one free in sixteen, so its zero is not evidence - and that
     * sentence is in scripts/dev/cases/mm-argv-poison.txt precisely because
     * somebody read it as evidence.
     *
     * 0 sweeps every free; 0x0F is one in sixteen and is the default, because
     * the walk is not free. */
#ifndef HW_FREE_CHECK_MASK
#define HW_FREE_CHECK_MASK 0x0Fu
#endif
    if ((++g_free_seq & HW_FREE_CHECK_MASK) != 0u) {
        return;
    }
    /* Teardown is NOT skipped, and that is a change of mind worth explaining.
     *
     * This used to return immediately whenever an address space was being
     * destroyed, because "teardown legitimately holds every frame it is
     * freeing" - the dying task still maps them, and reporting that cost
     * eighty false alarms a boot the first time this check existed.
     *
     * That was true of the walk as it was then. It is not true now:
     * hw_frame_still_mapped skips g_aspace_being_destroyed itself, so the
     * mappings it counts are only ever *other* processes'. The blanket return
     * here is a leftover from the earlier version, and what it suppresses is
     * precisely the case that matters - a teardown freeing a frame that
     * somebody else still maps.
     *
     * Which is the defect this detector exists for, and it has been unable to
     * see it. The two situations are "the dying space maps it", which is fine
     * and is already excluded one layer down, and "a live space maps it",
     * which is the bug; one flag could not tell them apart and answered for
     * both. */
    if (!hw_frame_still_mapped(phys, &pid, &mappers, &mapped_va)) {
        return;
    }
    /* Compare the two numbers that must agree, instead of asking whether the
     * frame is still free.
     *
     * Every owned page-table entry is one reference, so mappers and owners are
     * the same quantity counted two ways. Asking "is it still free" was wrong
     * in both directions: it reported a frame another core had legitimately
     * reallocated and mapped (owners=1 mappers=1, a healthy frame described by
     * an unhealthy detector), and it silently dropped the real cases where the
     * reallocation happened to win the race - which is how a genuine premature
     * free went unreported while the stress service was still finding it.
     *
     * More mappings than references is the dangerous direction and the only one
     * worth a report: it means somebody holds a page nothing is counting. */
    owners_at_check = vibeos_frame_owners(phys);
    if (mappers <= owners_at_check) {
        return;
    }
    /* Was the frame handed out while the walk ran? Then the mapping it found
     * may be a new tenant's, since unmapped and freed again - owners back at
     * zero, mapping seen a moment earlier. That is what svc-stress did to four
     * frames of an exiting svc-press, and every report said owners_now=1
     * (M-062). A frame nobody has taken since the release, still mapped by a
     * live process, is the defect this exists for - and is still reported. */
    if (vibeos_frame_handouts(phys) != handouts) {
        vibeos_mm_stats()->free_watch_torn++;
        return;
    }
    vibeos_mm_stats()->free_while_mapped++;
    hw_log(VIBEOS_LOG_ERROR, 46u, phys, (uint64_t)pid,
           "reclaiming a frame that a live process still maps "
           "(a0 = frame, a1 = pid)");
    vibeos_x86_64_serial_lock();
    vibeos_x86_64_serial_puts("[MM] FREE_WHILE_MAPPED frame=0x");
    vibeos_x86_64_serial_print_hex(phys);
    vibeos_x86_64_serial_puts(" still mapped by pid=0x");
    vibeos_x86_64_serial_print_hex((uint64_t)pid);
    vibeos_x86_64_serial_puts(" during ");
    vibeos_x86_64_serial_puts(vibeos_vmspace_current_op());
    vibeos_x86_64_serial_puts(" mappers=0x");
    vibeos_x86_64_serial_print_hex((uint64_t)mappers);
    vibeos_x86_64_serial_puts(" owners=0x");
    vibeos_x86_64_serial_print_hex((uint64_t)owners_at_check);
    vibeos_x86_64_serial_puts(" owners_now=0x");
    vibeos_x86_64_serial_print_hex((uint64_t)vibeos_frame_owners(phys));
    /* Where the surviving mapper holds it. A lost reference on a stack page, on
     * a program's text and on a page table are three different defects, and
     * this line is read long after the machine that produced it is gone. */
    vibeos_x86_64_serial_puts(" at_va=0x");
    vibeos_x86_64_serial_print_hex(mapped_va);
    vibeos_x86_64_serial_puts("\n");
    vibeos_x86_64_serial_unlock();
}

/* A frame handed out with its poison broken: something wrote to it after it
 * was freed.
 *
 * The first eight only. Three thousand of these would flood the log the gate
 * reads and change the timing of the thing being looked at - and the first one
 * is the one that matters, because after that the machine is already wrong.
 *
 * The tag is the pointer the release stored in word 1, which the probes never
 * touch. Printed as an address rather than dereferenced: it points at a string
 * literal in the kernel image, and addr2line turns it into a name safely from
 * outside, whereas following it here would be this diagnostic taking the same
 * risk as the defect it is reporting. */
/* Recorded here, printed from outside the frame layer's lock.
 *
 * The watch runs inside that lock, and it used to print from there - taking the
 * console lock with the frame lock held. Under memory pressure (svc-press, while
 * the anonymous-reclaim workload was being built) the machine then went silent
 * with one core in this function and the others queued on the console, and the
 * one line that would have named the use-after-free was never printed: the
 * diagnostic deadlocked the machine it was explaining, which CLAUDE.md records
 * once already for a watch that called a locking accessor. The watch now only
 * fills a record; the allocating core prints it once it has left the layer. */
typedef struct {
    uint64_t phys, word, found, tag, owners;
} hw_poison_rec_t;

static hw_poison_rec_t g_poison_rec[8];
static uint32_t g_poison_reported;      /* records filled, under the frame lock */
static uint32_t g_poison_printed;       /* records printed, claimed atomically  */

static void hw_frame_poison_watch(uint64_t phys, uint32_t word, uint64_t found,
                                  uint64_t tag) {
    uint32_t n = __atomic_load_n(&g_poison_reported, __ATOMIC_RELAXED);
    if (n >= 8u) {
        return;
    }
    g_poison_rec[n].phys = phys;
    g_poison_rec[n].word = (uint64_t)word;
    g_poison_rec[n].found = found;
    g_poison_rec[n].tag = tag;
    g_poison_rec[n].owners = (uint64_t)vibeos_frame_owners_locked(phys);
    __atomic_store_n(&g_poison_reported, n + 1u, __ATOMIC_RELEASE);
}

/* Print what the watch recorded. Only from a place that holds no layer lock:
 * the console lock is taken here, and the point of the record is that nobody
 * takes it while holding the frame lock. */
static void hw_poison_flush(void) {
    for (;;) {
        uint32_t done = __atomic_load_n(&g_poison_printed, __ATOMIC_ACQUIRE);
        uint32_t have = __atomic_load_n(&g_poison_reported, __ATOMIC_ACQUIRE);
        const hw_poison_rec_t *r;

        if (done >= have) {
            return;
        }
        if (!__atomic_compare_exchange_n(&g_poison_printed, &done, done + 1u, 0,
                                         __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
            continue;   /* another core took this one */
        }
        r = &g_poison_rec[done];
        vibeos_x86_64_serial_lock();
        vibeos_x86_64_serial_puts("[MM] POISON_BROKEN frame=0x");
        vibeos_x86_64_serial_print_hex(r->phys);
        vibeos_x86_64_serial_puts(" word=0x");
        vibeos_x86_64_serial_print_hex(r->word);
        vibeos_x86_64_serial_puts(" found=0x");
        vibeos_x86_64_serial_print_hex(r->found);
        vibeos_x86_64_serial_puts(" freed_by=0x");
        vibeos_x86_64_serial_print_hex(r->tag);
        vibeos_x86_64_serial_puts(" owners=0x");
        vibeos_x86_64_serial_print_hex(r->owners);
        vibeos_x86_64_serial_puts("\n");
        vibeos_x86_64_serial_unlock();
    }
}

void hw_free_page_why(void *p, const char *why) {
    uint64_t phys = (uint64_t)(uintptr_t)p;

    if (!p) {
        return;
    }
    /* Sampled, because the walk is expensive and the bug is not rare enough to
     * need every free checked. One in sixty-four frees costs little and still
     * catches a fault that appears once in twenty boots within a run or two -
     * and when it hits, it names the function doing the freeing and the process
     * that still has the page.
     *
     * Before the release, and outside the memory lock: hw_page_owners takes
     * that lock, and this walk reads page tables, which is not work to do with
     * interrupts masked. */
    if (vibeos_frame_total() != 0ull && (++g_free_seq & 0x7u) == 0u) {
        uint32_t pid = 0;
        if (hw_frame_still_mapped(phys, &pid, 0, 0)) {
            hw_log(VIBEOS_LOG_ERROR, 46u, phys, (uint64_t)pid,
                   "freeing a frame that a live process still maps "
                   "(a0 = frame, a1 = pid)");
            vibeos_x86_64_serial_lock();
            vibeos_x86_64_serial_puts("[MM] FREE_WHILE_MAPPED frame=0x");
            vibeos_x86_64_serial_print_hex(phys);
            vibeos_x86_64_serial_puts(" still mapped by pid=0x");
            vibeos_x86_64_serial_print_hex((uint64_t)pid);
            vibeos_x86_64_serial_puts(" freed by ");
            vibeos_x86_64_serial_puts(why ? why : "(unknown)");
            vibeos_x86_64_serial_puts(" owners_before_put=0x");
            vibeos_x86_64_serial_print_hex((uint64_t)hw_page_owners(phys));
            vibeos_x86_64_serial_puts("\n");
            vibeos_x86_64_serial_unlock();
        }
    }

    /* One owner, released. Direct callers of hw_free_page - page tables, kernel
     * stacks, the PML4 - allocate a frame and never map it, so the reference
     * the allocation gave them is the only one and this frees it. That is the
     * D9 contract; a caller that allocated *in order to map* drops its own
     * reference at the mapping instead and never comes through here. */
    /* The tag goes in through the layer, which writes it under the lock that
     * also guards allocation.
     *
     * Writing it here, after the release, was a use-after-free that took a day
     * to find: between the put and the store another core can allocate the
     * frame, and word 1 of a page is slot 1 of a PML4 - the user window. A live
     * process then faulted on an instruction fetch inside its own code, with
     * its whole user window replaced by a pointer to a string literal. The
     * comment that used to be here reasoned carefully about the poison check
     * and not at all about the frame being reallocated. */
    (void)vibeos_frame_put_why(phys, why);
}


/* Is the frame layer up? Until it is - the very first page tables, before the
 * firmware memory map has even been read - allocation comes from a small static
 * pool whose pages are never released. */
int g_frame_layer_ready;

/* An ordinary page for a user process, subject to the watermarks. */
static void *hw_alloc_page_admitted(int privileged) {
    uint64_t phys = 0;

    if (g_frame_layer_ready) {
        /* The marks are consulted here, and the first version of this file did
         * not consult them anywhere - they were configured and then read by
         * nobody, which is this project's most repeated defect wearing new
         * clothes. The pressure service found it in one boot: the machine ran
         * memory to nothing and died with a corrupted backtrace, because
         * "refuse before the last frame" was a number rather than a rule.
         *
         * Below the low mark, try to give something back first. The clean tier
         * costs nothing to drop - the file still has the page - so this is
         * worth doing before refusing anybody. */
        if (vibeos_reclaim_pressure() != VIBEOS_MEM_OK) {
            (void)vibeos_reclaim_run(32u);
        }
        if (!vibeos_reclaim_admit(privileged)) {
            return 0;
        }
        phys = vibeos_frame_alloc(VIBEOS_FRAME_ALLOCATED);
        /* Out of the frame layer's lock now: anything its poison watch found
         * during this allocation can be printed. */
        hw_poison_flush();
        return (void *)(uintptr_t)phys;
    }

    /* The bootstrap pool. Static, tiny, and deliberately not reference counted:
     * nothing allocated before the frame layer exists is ever released. */
    {
        uint8_t *p;
        uint32_t i;
        hw_spin_lock_named(&g_mm_lock, __func__);
        if (g_pool_next >= VIBEOS_HW_POOL_PAGES) {
            hw_spin_unlock(&g_mm_lock);
            return 0;
        }
        p = g_page_pool[g_pool_next++];
        hw_spin_unlock(&g_mm_lock);
        for (i = 0; i < 4096u; i++) {
            p[i] = 0;
        }
        return p;
    }
}

/* Several contiguous frames. Exists because the bump allocator underneath used
 * to serve this and must not serve anything once the frame layer is up: two
 * allocators over one region means one of them is wrong about what is free. */

/* The unqualified door, for everything the kernel needs in order to keep
 * working - page tables, kernel stacks, the bookkeeping a teardown allocates.
 * Refusing these is how a machine deadlocks at the minimum instead of
 * recovering from it, which is the whole reason the reserve exists. */
void *hw_alloc_page(void) {
    return hw_alloc_page_admitted(1);
}

/* And the door for a page a user process asked for, which is what the reserve
 * is being held back from. */
void *hw_alloc_user_page(void) {
    return hw_alloc_page_admitted(0);
}

void *hw_alloc_pages_contig(uint32_t count) {
    uint64_t phys;

    if (!g_frame_layer_ready) {
        return 0;
    }
    phys = vibeos_frame_alloc_contig(count, VIBEOS_FRAME_ALLOCATED);
    return (void *)(uintptr_t)phys;
}

/* The memory lock, as the frame layer wants it: two functions taking nothing.
 * Interrupts are masked for the duration, which is what hw_spin_lock does and
 * why nothing under this lock may be slow. */
static void hw_frame_lock(void) {
    hw_spin_lock_named(&g_mm_lock, __func__);
}

/* The page cache gets a lock of its own rather than the frame layer's.
 *
 * It has to: a lookup that misses allocates a frame and, on failure, releases
 * one, and the frame layer takes g_mm_lock to do either. Sharing the lock would
 * deadlock the first time a file page was not resident - which presents as a
 * machine that stops with no output, and this project has spent enough time on
 * that particular symptom.
 *
 * The ordering is therefore cache lock, then frame lock, and never the reverse:
 * nothing in the frame layer knows the cache exists. */
static hw_lock_t g_cache_lock;

/* The block cache's lock, below the filesystem.
 *
 * Its own, not the page cache's and not the frame layer's. Those sit at
 * different levels, and a lock shared across levels is what would have
 * deadlocked the page cache on its first miss - it allocates a frame, and the
 * frame layer takes its own lock to do it. The ordering here is filesystem,
 * then block cache, then the driver, and never upwards. */
static hw_lock_t g_blockcache_lock;

static void hw_blockcache_lock(void) {
    hw_spin_lock_named(&g_blockcache_lock, __func__);
}

static void hw_blockcache_unlock(void) {
    hw_spin_unlock(&g_blockcache_lock);
}

static void hw_cache_lock(void) {
    hw_spin_lock_named(&g_cache_lock, __func__);
}

static void hw_cache_unlock(void) {
    hw_spin_unlock(&g_cache_lock);
}

/* The reverse map's own lock. Its own, because it is called from inside the
 * address-space layer while the frame lock is held, and from teardown while
 * neither is - a borrowed lock would deadlock in the first case and protect
 * nothing in the second. */
static hw_lock_t g_rmap_lock;

static void hw_rmap_lock(void) {
    hw_spin_lock_named(&g_rmap_lock, __func__);
}

static void hw_rmap_unlock(void) {
    hw_spin_unlock(&g_rmap_lock);
}

/* A teardown waiting for a reclaimer to finish with its page tables (M-056).
 * The reclaimer holds a claim across one page's disk write, so an honest wait
 * is a write's length; the bound is the spinlocks' own, and crossing it means a
 * claim was never released - which, left alone, is a core spinning in exit
 * forever with nothing said. */
static void hw_rmap_relax(uint64_t spins) {
    if (spins > VIBEOS_HW_LOCK_SPIN_LIMIT) {
        hw_panic("teardown waited too long for a reclaim claim on its address "
                 "space: a claim was never released");
    }
    /* The reclaimer holding the claim may be in a shootdown waiting for this
     * very core - which, in a syscall, has interrupts off and cannot take the
     * IPI. That wait is how the first reclaim load stopped the machine. */
    hw_tlb_service_flush();
    __asm__ __volatile__("pause" ::: "memory");
}

static void hw_frame_unlock(void) {
    hw_spin_unlock(&g_mm_lock);
}

/* What L1 allocates its page tables from. A table is a frame like any other -
 * one owner, the address space that built it - and it is freed by structure at
 * teardown rather than by the ownership bit, which marks user frames only. */
static uint64_t hw_vmspace_alloc_table(void) {
    /* Pinned before it is used, and this is a safety property rather than a
     * tuning one. A page table that gets evicted does not make the machine
     * slow: the next walk reads whatever the frame now holds and installs it
     * as a translation, which corrupts an address space asynchronously and a
     * long way from here. The same argument covers DMA buffers, which is why
     * both are on the same list. */
    /* Allocated with its state, rather than as a generic frame relabelled
     * later. meminfo can then say how much of memory is page tables, which is
     * a figure that grows with the number of processes and had nowhere to be
     * seen before. */
    uint64_t phys = vibeos_frame_alloc(VIBEOS_FRAME_PAGE_TABLE);

    if (phys) {
        vibeos_reclaim_pin(phys);
    }
    return phys;
}

static void hw_vmspace_free_table(uint64_t phys) {
    /* Unpinned before release, or the bit follows the frame into the free list
     * and the next tenant inherits a pin nobody asked for - a frame that can
     * never be reclaimed again, which is a leak no counter would report as
     * one because the frame is perfectly accounted for. */
    vibeos_reclaim_unpin(phys);
    hw_free_page((void *)(uintptr_t)phys);
}

/* The page directories the kernel shares with every address space. L1 compares
 * against these to know what it must copy before carving a user page out of the
 * identity region - by identity, not by address range, so a future change to
 * where the kernel maps itself cannot quietly make the test wrong. */
static const uint64_t *hw_vmspace_shared_pd(uint32_t gib) {
    if (gib >= VIBEOS_HW_IDENTITY_GIB) {
        return 0;
    }
    return &g_pd[gib][0];
}

/* ---- the swap bridges ----------------------------------------------------
 *
 * vmspace does the page-table work and swapmap owns the slots; these two lines
 * are all that connects them, and they are here rather than in either layer
 * because neither may depend on the other. */
/* What each slot was given, as a hash, so what it gives back can be checked.
 *
 * Swap is the one place this kernel writes memory somewhere and trusts that
 * the same bytes come back, and for its whole life nothing asked. The first
 * load to force reclaim did, by accident: on QEMU's vvfat - the gate's boot
 * disk, a host directory presented as FAT - every slot past the first two read
 * back as slot 0 or slot 1, and the processes whose pages they were died with
 * each other's data. That took a day of reading the wrong layers. A hash per
 * slot names it on the first page-in. Must be zero; the gate asserts it. */
static uint64_t g_swap_hash[VIBEOS_HW_SWAP_SLOTS];

static uint64_t hw_swap_page_hash(const void *page) {
    const uint64_t *w = (const uint64_t *)page;
    uint64_t h = 0xcbf29ce484222325ull;
    uint32_t i;

    for (i = 0; i < 512u; i++) {
        h ^= w[i];
        h *= 0x100000001b3ull;
    }
    return h | 1ull;   /* never 0, so an unwritten slot never matches */
}

/* Who wrote each slot last and how many times it has been written (M-070): see
 * hw_swap_release, which says so when a slot is given back twice. */
static uint32_t g_swap_written_pid[VIBEOS_HW_SWAP_SLOTS];
static uint32_t g_swap_writes[VIBEOS_HW_SWAP_SLOTS];
static uint32_t hw_swap_pid_now(void);

static int hw_swap_write_page(uint32_t slot, void *page) {
    if (slot < VIBEOS_HW_SWAP_SLOTS) {
        g_swap_written_pid[slot] = hw_swap_pid_now();
        g_swap_writes[slot]++;
    }
    /* Hashed before the write: the page is read-only to its owner by now (the
     * swap-out marker), so this is what the device is given. */
    uint64_t h = hw_swap_page_hash(page);
    int rc = vibeos_swap_write(slot, page);

    if (rc == 0 && slot < VIBEOS_HW_SWAP_SLOTS) {
        __atomic_store_n(&g_swap_hash[slot], h, __ATOMIC_RELEASE);
    }
    return rc;
}

/* A swapped-out entry has gone away; its slot is free (M-063). */
/* Which operation last gave each slot back, so a second release of the same
 * slot can say who did the first. M-070 was one boot in 24 with only a count
 * - swap_double_free=1 - and a count says nothing about who. */
static const char *g_swap_freed_by[VIBEOS_HW_SWAP_SLOTS];
/* And which process was running when it did, and how many times the slot has
 * been written and given back: a release names an operation, and three
 * captures of M-070 have shown that an operation is not enough - the question
 * each of them left was *whose* page-in gave back a slot another address space
 * still named. */
static uint32_t g_swap_freed_pid[VIBEOS_HW_SWAP_SLOTS];
static uint32_t g_swap_frees[VIBEOS_HW_SWAP_SLOTS];

static uint32_t hw_swap_pid_now(void) {
    return g_current_task >= 0 ? (uint32_t)hw_task_pid_of(&g_tasks[g_current_task]) : 0u;
}

static void hw_swap_release(uint32_t slot) {
    const char *op = vibeos_vmspace_current_op();

    if (slot < VIBEOS_HW_SWAP_SLOTS && !vibeos_swap_is_allocated(slot)) {
        vibeos_x86_64_serial_lock();
        vibeos_x86_64_serial_puts("[MM] SWAP_DOUBLE_RELEASE slot=0x");
        vibeos_x86_64_serial_print_hex(slot);
        vibeos_x86_64_serial_puts(" now_by=");
        vibeos_x86_64_serial_puts(op ? op : "?");
        vibeos_x86_64_serial_puts(" before_by=");
        vibeos_x86_64_serial_puts(g_swap_freed_by[slot] ? g_swap_freed_by[slot]
                                                        : "not-a-release");
        vibeos_x86_64_serial_puts(" pid=0x");
        vibeos_x86_64_serial_print_hex(hw_swap_pid_now());
        vibeos_x86_64_serial_puts(" before_pid=0x");
        vibeos_x86_64_serial_print_hex(g_swap_freed_pid[slot]);
        vibeos_x86_64_serial_puts(" written_by_pid=0x");
        vibeos_x86_64_serial_print_hex(g_swap_written_pid[slot]);
        vibeos_x86_64_serial_puts(" writes=0x");
        vibeos_x86_64_serial_print_hex(g_swap_writes[slot]);
        vibeos_x86_64_serial_puts(" frees=0x");
        vibeos_x86_64_serial_print_hex(g_swap_frees[slot]);
        vibeos_x86_64_serial_puts("\n");
        vibeos_x86_64_serial_unlock();
    }
    if (slot < VIBEOS_HW_SWAP_SLOTS) {
        g_swap_freed_by[slot] = op;
        g_swap_freed_pid[slot] = hw_swap_pid_now();
        g_swap_frees[slot]++;
    }
    (void)vibeos_swap_free(slot);
}

/* Bring the page at `va` back from swap. Shared by the page fault and by fork,
 * so there is one place that knows the order: a frame, the page-in, and only
 * then the slot back to the map - which a failure must not do, because the
 * entry still names it.
 *
 * Privileged, deliberately. This allocation is what brings a page back;
 * refusing it at the low watermark would leave a process unable to touch
 * memory it already owns, and reclaim would be preventing the very thing it
 * reclaimed for. */
/* Why a page-in did not happen, in one line with everything that decides it: a
 * process killed on a swapped entry (M-070) has so far left only the entry
 * behind, and the entry does not say whether there was no frame to be had,
 * the slot could not be read, or the slot was no longer this entry's. */
static void hw_swap_in_failed(uint64_t va, int64_t slot, const char *why) {
    vibeos_x86_64_serial_lock();
    vibeos_x86_64_serial_puts("[MM] SWAP_IN_FAILED why=");
    vibeos_x86_64_serial_puts(why);
    vibeos_x86_64_serial_puts(" va=0x");
    vibeos_x86_64_serial_print_hex(va);
    vibeos_x86_64_serial_puts(" slot=0x");
    vibeos_x86_64_serial_print_hex((uint64_t)slot);
    vibeos_x86_64_serial_puts(" allocated=0x");
    vibeos_x86_64_serial_print_hex(slot >= 0 ? (uint64_t)vibeos_swap_is_allocated((uint32_t)slot) : 0ull);
    vibeos_x86_64_serial_puts(" pid=0x");
    vibeos_x86_64_serial_print_hex(hw_swap_pid_now());
    if (slot >= 0 && slot < (int64_t)VIBEOS_HW_SWAP_SLOTS) {
        vibeos_x86_64_serial_puts(" freed_by=");
        vibeos_x86_64_serial_puts(g_swap_freed_by[slot] ? g_swap_freed_by[slot] : "nobody");
        vibeos_x86_64_serial_puts(" freed_pid=0x");
        vibeos_x86_64_serial_print_hex(g_swap_freed_pid[slot]);
        vibeos_x86_64_serial_puts(" written_by_pid=0x");
        vibeos_x86_64_serial_print_hex(g_swap_written_pid[slot]);
        vibeos_x86_64_serial_puts(" writes=0x");
        vibeos_x86_64_serial_print_hex(g_swap_writes[slot]);
        vibeos_x86_64_serial_puts(" frees=0x");
        vibeos_x86_64_serial_print_hex(g_swap_frees[slot]);
    }
    vibeos_x86_64_serial_puts(" free_frames=0x");
    vibeos_x86_64_serial_print_hex(vibeos_mm_stats()->frames_free);
    vibeos_x86_64_serial_puts("\n");
    vibeos_x86_64_serial_unlock();
}

static int hw_swap_bring_in(vibeos_vmspace_t *sv, uint64_t va) {
    int64_t slot = vibeos_vmspace_swap_slot(sv, va);
    void *page;

    if (slot < 0) {
        return -1;
    }
    page = hw_alloc_page();
    if (!page) {
        hw_swap_in_failed(va, slot, "no_frame");
        return -1;
    }
    if (vibeos_vmspace_swap_in(sv, va, (uint64_t)(uintptr_t)page) != 0) {
        /* Nothing was taken: the frame is still this function's. Said only
         * when the entry still names a slot afterwards: an entry another core
         * brought in first is not a failure, it is somebody else's success. */
        hw_free_page_why(page, "swap_in_failed");
        if (vibeos_vmspace_swap_slot(sv, va) >= 0) {
            hw_swap_in_failed(va, slot, "page_in_refused");
        }
        return -1;
    }
    /* The slot went back inside the page-in, through hw_swap_release: only it
     * knows which slot it consumed. */
    return 0;
}

static int hw_swap_read_page(uint32_t slot, void *page) {
    int rc = vibeos_swap_read(slot, page);

    if (rc == 0 && slot < VIBEOS_HW_SWAP_SLOTS) {
        uint64_t want = __atomic_load_n(&g_swap_hash[slot], __ATOMIC_ACQUIRE);

        vibeos_mm_stats()->swap_read_checked++;
        if (hw_swap_page_hash(page) != want) {
            vibeos_mm_stats()->swap_read_mismatch++;
            vibeos_x86_64_serial_lock();
            vibeos_x86_64_serial_puts("[MM] SWAP_READ_MISMATCH slot=0x");
            vibeos_x86_64_serial_print_hex(slot);
            vibeos_x86_64_serial_puts(" first_word=0x");
            vibeos_x86_64_serial_print_hex(((const uint64_t *)page)[0]);
            vibeos_x86_64_serial_puts(" - the slot does not hold what was written to it\n");
            vibeos_x86_64_serial_unlock();
        }
    }
    return rc;
}

/* The kernel reaches every frame through the identity map, so "addressable" and
 * "below the identity limit" are the same question. The frame layer asks this
 * before poisoning or zeroing a frame; null means it counts the frame without
 * touching it. In practice the tail above the limit is reserved at bring-up, so
 * this never returns null here - it is the layer's contract, not a condition
 * this kernel leans on. */
void *hw_frame_identity_map(uint64_t phys) {
    if (phys + 4096ull > VIBEOS_HW_IDENTITY_LIMIT) {
        return 0;
    }
    return (void *)(uintptr_t)phys;
}

/* Bring the physical memory manager online from the firmware memory map. */
void hw_pmm_bringup(const vibeos_boot_info_t *boot_info) {
    if (!boot_info) {
        vibeos_x86_64_serial_puts("[HW] no boot_info: using the static page pool\n");
        return;
    }
    if (vibeos_pmm_init_from_boot_info(&g_hw_pmm, boot_info, 4096) != 0) {
        vibeos_x86_64_serial_puts("[HW] PMM init failed: falling back to the static page pool\n");
        return;
    }
    /* The kernel's own image must never be handed out. It is loader memory, so a
     * correct map already excludes it - but for as long as the loader passed the
     * map it took before loading the kernel, the largest free region contained
     * the image, and a load that allocated about 40 MiB reached the page holding
     * the IDT and the timer's interrupt stack and zeroed them (M-060). Every
     * symptom was a core faulting at a garbage rip inside the timer interrupt.
     * Reserve it regardless, and say whether the map needed correcting: the boot
     * gate asserts that it did not. */
    {
        uintptr_t img = (uintptr_t)__kernel_image_start;
        uintptr_t img_end = (uintptr_t)__kernel_image_end;
        int in_pool = !(img_end <= g_hw_pmm.base ||
                        img >= g_hw_pmm.base + g_hw_pmm.size_bytes);

        vibeos_x86_64_serial_lock();
        vibeos_x86_64_serial_puts("[HW] kernel image 0x");
        vibeos_x86_64_serial_print_hex((uint64_t)img);
        vibeos_x86_64_serial_puts("-0x");
        vibeos_x86_64_serial_print_hex((uint64_t)img_end);
        vibeos_x86_64_serial_puts(" in_free_map=");
        vibeos_x86_64_serial_puts(in_pool ? "1" : "0");
        vibeos_x86_64_serial_puts("\n");
        vibeos_x86_64_serial_unlock();
        if (vibeos_pmm_reserve(&g_hw_pmm, img, (size_t)(img_end - img)) != 0) {
            vibeos_x86_64_serial_puts("[HW] PMM cannot reserve the kernel image; "
                                     "falling back to the static page pool\n");
            return;
        }
    }
    /* Nothing of the kernel's may live where a process can shadow it. See
     * VIBEOS_HW_LOW_USER_BASE and vibeos_pmm_reserve for why this is a
     * correctness requirement and not a tidiness one. */
    if (vibeos_pmm_reserve(&g_hw_pmm, (uintptr_t)VIBEOS_HW_LOW_USER_BASE,
                           (size_t)(VIBEOS_HW_LOW_USER_LIMIT - VIBEOS_HW_LOW_USER_BASE)) != 0) {
        g_hw_pmm_ready = 0;
        vibeos_x86_64_serial_puts("[HW] PMM cannot reserve the low user window; "
                                 "falling back to the static page pool\n");
        return;
    }
    g_hw_pmm_ready = 1;

    /* Now that pages exist, take a contiguous staging area large enough for a
     * real program. Failing is not fatal: the small static buffer still works,
     * and small programs still run - they simply cannot be large ones. */
    {
        void *stage = vibeos_pmm_alloc_pages(&g_hw_pmm,
                                             VIBEOS_HW_EXEC_STAGE_BYTES / 4096u);
        if (stage && ((uint64_t)(uintptr_t)stage + VIBEOS_HW_EXEC_STAGE_BYTES)
                <= VIBEOS_HW_IDENTITY_LIMIT) {
            g_exec_elf = (uint8_t *)stage;
            g_exec_elf_cap = VIBEOS_HW_EXEC_STAGE_BYTES;
            vibeos_x86_64_serial_puts("[HW] exec staging buffer bytes=0x");
            vibeos_x86_64_serial_print_hex(
                (uint64_t)VIBEOS_HW_EXEC_STAGE_BYTES);
            vibeos_x86_64_serial_puts("\n");
        } else {
            vibeos_x86_64_serial_puts("[HW] exec staging buffer stays at 64 KiB; "
                                     "large programs will not load\n");
        }
    }
    {
        void *stage = vibeos_pmm_alloc_pages(&g_hw_pmm,
                                             VIBEOS_HW_INTERP_STAGE_BYTES / 4096u);

        if (stage && ((uint64_t)(uintptr_t)stage + VIBEOS_HW_INTERP_STAGE_BYTES)
                <= VIBEOS_HW_IDENTITY_LIMIT) {
            g_interp_elf = (uint8_t *)stage;
            g_interp_elf_cap = VIBEOS_HW_INTERP_STAGE_BYTES;
            vibeos_x86_64_serial_puts("[HW] interpreter staging buffer bytes=0x");
            vibeos_x86_64_serial_print_hex(
                (uint64_t)VIBEOS_HW_INTERP_STAGE_BYTES);
            vibeos_x86_64_serial_puts("\n");
        } else {
            /* Left null on purpose. A dynamic program is then refused with the
             * same message as before this existed, which is a truthful "cannot
             * load" rather than a load that goes somewhere undefined. */
            vibeos_x86_64_serial_puts("[HW] no interpreter staging buffer; "
                                     "dynamic programs will not load\n");
        }
    }

    /* Last, and the order matters.
     *
     * Everything above this point comes from the bump allocator: the early page
     * tables, the two staging buffers. From here on nothing may, because two
     * allocators over one region means one of them is wrong about what is free
     * - which is the shape of the defect this rewrite exists to end. So the
     * frame layer takes over the whole region and the prefix already handed out
     * is reserved rather than described as free.
     *
     * A bump allocator makes that prefix exactly [base, base + offset_bytes),
     * which is why this is a reservation and not a scan of anything. */
    {
        uint64_t frames = (uint64_t)g_hw_pmm.size_bytes / 4096ull;
        uint64_t table_bytes = frames * (uint64_t)sizeof(vibeos_frame_t);
        uint64_t table_pages = (table_bytes + 4095ull) / 4096ull;
        void *table = frames ? vibeos_pmm_alloc_pages(&g_hw_pmm,
                                                      (size_t)table_pages) : 0;
        /* The reverse map's storage, carved here and not later.
         *
         * It has to come out of the allocator *before* vibeos_frame_init,
         * beside the frame table, because what protects these pages from being
         * handed out again is the reserve of everything the physical allocator
         * had already given away by the time the frame layer started - the
         * prefix, below. Anything carved afterwards is memory the frame layer
         * believes is free.
         *
         * That was the first version, and it did exactly what it sounds like:
         * user pages were allocated on top of the node pool, the lists were
         * overwritten with whatever the new tenant wrote, and one of them
         * closed into a cycle. The boot stopped with CPU#0 inside
         * vibeos_rmap_add and every other core outside the kernel - a spin with
         * nobody to wait for, which is what walking a circular list looks like
         * from the outside.
         *
         * Sizing: one list head per frame is mandatory, because the table is
         * indexed directly and that is what removes collisions from a structure
         * whose whole job is to be trustworthy about identity. Two nodes per
         * frame is a choice - it covers a parent and a child sharing
         * everything, which is what a fork produces - and running out is
         * reported rather than fatal, so an unusual workload degrades reclaim
         * instead of failing a mapping. nodes_peak says afterwards what it
         * should have been. */
        uint64_t rmap_bytes = frames * (sizeof(uint32_t) + 2ull * 24ull);
        uint64_t rmap_pages = (rmap_bytes + 4095ull) / 4096ull;
        void *rmap_pool = frames ? vibeos_pmm_alloc_pages(&g_hw_pmm,
                                                          (size_t)rmap_pages) : 0;
        uint64_t prefix;
        int ok = 0;

        if (table && ((uint64_t)(uintptr_t)table + table_pages * 4096ull)
                <= VIBEOS_HW_IDENTITY_LIMIT) {
            /* The table describes the whole region including itself, because a
             * frame the table cannot describe is a frame nothing can account
             * for - and the previous version of this got the coverage wrong in
             * a way that stayed invisible until the machine used enough memory
             * to reach the frames it had missed. */
            /* Before init, so the layer is never touched unlocked - including
             * by the initialisation itself, which runs on one core here but
             * should not depend on that. */
            vibeos_frame_set_lock(hw_frame_lock, hw_frame_unlock);
            vibeos_frame_set_release_watch(hw_frame_release_watch);
            vibeos_frame_set_poison_watch(hw_frame_poison_watch);
            vibeos_vma_set_lock(hw_frame_lock, hw_frame_unlock);
            vibeos_vma_pool_init(g_vma_pool, VIBEOS_HW_VMA_ENTRIES);
            vibeos_cache_set_lock(hw_cache_lock, hw_cache_unlock);
            /* The block cache below the filesystem, which until I2 had no
             * lock and no caller on a booting machine. Its own, not the page
             * cache's: they sit at different levels and a shared lock between
             * levels is how the page cache would have deadlocked on its first
             * miss. */
            vibeos_blockcache_set_lock(hw_blockcache_lock, hw_blockcache_unlock);
            /* Its own lock, like every other layer here, and for the reason
             * CLAUDE.md gives: sharing the frame layer's would deadlock, since
             * this is called from inside the address-space layer while the
             * frame lock is held. */
            vibeos_rmap_set_lock(hw_rmap_lock, hw_rmap_unlock);
            vibeos_rmap_set_relax(hw_rmap_relax);
            if (rmap_pool &&
                ((uint64_t)(uintptr_t)rmap_pool + rmap_pages * 4096ull)
                    <= VIBEOS_HW_IDENTITY_LIMIT) {
                vibeos_rmap_set_base((uint64_t)g_hw_pmm.base);
                (void)vibeos_rmap_init(rmap_pool, rmap_pages * 4096ull,
                                       (uint32_t)frames);
            }
            vibeos_exec_set_auditor(hw_cache_audit);
            vibeos_cache_init(g_cache_table, VIBEOS_HW_CACHE_ENTRIES,
                              hw_cache_read, 0);
            prefix = (uint64_t)g_hw_pmm.offset_bytes;
            if (vibeos_frame_init((uint64_t)g_hw_pmm.base,
                                  (uint64_t)g_hw_pmm.size_bytes,
                                  (vibeos_frame_t *)table, (uint32_t)frames,
                                  hw_frame_identity_map) == 0 &&
                vibeos_frame_reserve((uint64_t)g_hw_pmm.base, prefix) == 0) {
                ok = 1;
                /* The kernel reaches frames only through the identity map, so a
                 * frame above that limit is one no allocation could use. The
                 * old allocator discovered this by handing one out and throwing
                 * it away; reserving says it once. */
                if ((uint64_t)g_hw_pmm.base + (uint64_t)g_hw_pmm.size_bytes
                        > VIBEOS_HW_IDENTITY_LIMIT) {
                    (void)vibeos_frame_reserve(VIBEOS_HW_IDENTITY_LIMIT,
                                               (uint64_t)g_hw_pmm.base +
                                               (uint64_t)g_hw_pmm.size_bytes -
                                               VIBEOS_HW_IDENTITY_LIMIT);
                }
            }
        }

        if (ok) {
            /* Watermarks, as a fraction of what this machine actually has
             * rather than as a constant. A number that suits a large machine
             * starves a small one, and this kernel is run on both.
             *
             * A 64th and a 256th - about 1.5% and 0.4% - which on a boot with
             * ~104k frames is a low mark near 1600 and a minimum near 400.
             * The minimum has to be large enough for the work that ends the
             * pressure: a teardown allocates page tables of its own. */
            {
                uint64_t total = vibeos_frame_total();
                uint64_t low = total / 64ull;
                uint64_t min = total / 256ull;

                if (min < 64ull) {
                    min = 64ull;      /* a floor for very small machines */
                }
                if (low <= min) {
                    low = min * 4ull;
                }
                (void)vibeos_reclaim_set_marks(low, min);
                vibeos_reclaim_set_clean_source(vibeos_cache_reclaim);
            }

            /* Swap is configured later, and it has to be.
             *
             * It used to be answered here, unconditionally NONE, under a
             * comment saying somebody would fill it in. It could never have
             * been filled in *here*: this runs during memory bring-up and the
             * disk driver does not bind until much further down
             * hw_early_init, so there is no device to name yet. The only
             * honest answer at this point in the boot is "not yet". See
             * hw_swap_bringup, called once the volume is mounted. */

            g_frame_layer_ready = 1;

            /* The address-space layer, immediately after the frame layer it
             * allocates from. Nothing has created an address space yet: user
             * tasks come later in the boot, and the kernel's own page tables
             * are the static ones built in hw_paging_bringup. */
            {
                vibeos_vmspace_backend_t vb;
                uint32_t bi;
                for (bi = 0; bi < sizeof(vb); bi++) {
                    ((uint8_t *)(void *)&vb)[bi] = 0;
                }
                vb.map_phys = hw_frame_identity_map;
                vb.alloc_table = hw_vmspace_alloc_table;
                vb.free_table = hw_vmspace_free_table;
                /* Exactly what hw_aspace_create used to write into slot 0:
                 * the kernel's identity map, shared, and with no PTE_USER so
                 * ring 3 cannot reach any of it. */
                vb.kernel_pml4e = (uint64_t)(uintptr_t)&g_pdpt[0] |
                                  PTE_PRESENT | PTE_WRITE;
                vb.identity_limit = VIBEOS_HW_IDENTITY_LIMIT;
                vb.shared_pdpt = &g_pdpt[0];
                vb.shared_pd = hw_vmspace_shared_pd;
                vb.invlpg = hw_invlpg;
                /* Only fork uses this today. munmap deliberately does not -
                 * see the long comment there: a synchronous barrier at
                 * munmap's call rate stalls, because syscall clears IF and a
                 * core inside a system call cannot answer the IPI. The layer
                 * asks; what the architecture does about it stays here. */
                vb.shootdown = hw_tlb_shootdown;
                /* And what munmap does instead of that barrier: park the
                 * frame until every other core has loaded CR3, which flushes
                 * its whole TLB because this kernel enables neither PCID nor
                 * global pages. No IPI, no rendezvous, no stall - the timer
                 * makes every core quiescent on its own. */
                vb.release_deferred = hw_tlbq_put;
                vb.quarantined = hw_tlbq_count;
                /* What a swapped-out entry needs from outside the layer: a slot
                 * given back when the entry goes away, and a page brought back
                 * when fork has to share it (M-063). */
                vb.swap_release = hw_swap_release;
                vb.swap_bring_in = hw_swap_bring_in;
                /* The two hooks that make page-out and page-in real.
                 *
                 * They were left null and the whole of P5 sat above them:
                 * built, host-tested against a memory-backed device, and never
                 * once run on this machine. vibeos_vmspace_swap_out checks for
                 * a null swap_write and gives up quietly, so the absence was
                 * not an error anywhere - it was a subsystem that could not be
                 * reached. */
                vb.swap_write = hw_swap_write_page;
                vb.swap_read = hw_swap_read_page;
                if (vibeos_vmspace_init(&vb) != 0) {
                    ok = 0;
                }
                /* So the layer's "which operation is this core inside" field
                 * is per core rather than a global every core overwrites. It
                 * was a global, and a CI failure was read through it this week
                 * - the value may have been another core's. Registered here
                 * beside the backend, because a layer that reports per-core
                 * facts and is never told which core it is on reports slot 0
                 * for everybody, which is worse than reporting nothing. */
                vibeos_vmspace_set_cpu_id(vibeos_x86_64_cpu_id);
            }
            /* The bump allocator is closed, not merely unused.
             *
             * Every vibeos_pmm_alloc_* call site checks this flag, so a call
             * added after this point returns null and fails visibly instead of
             * handing out frames the frame layer believes are free. One such
             * call - the GUI back buffer - already existed and cost an
             * afternoon: it produced a wild pointer inside a dynamic loader,
             * with none of the memory detectors firing, because nothing had
             * been freed early. Two allocators over one region do not corrupt
             * by freeing; they corrupt by agreeing. */
            g_hw_pmm_ready = 0;
            /* One call, because two log lines are not one fact - and these five
             * numbers are read together or not at all: they say whether every
             * frame the machine has is described, and how many of them were
             * already spoken for before the layer existed. */
            vibeos_x86_64_serial_lock();
            vibeos_x86_64_serial_puts("[MM] frame layer online: frames=0x");
            vibeos_x86_64_serial_print_hex(vibeos_frame_total());
            vibeos_x86_64_serial_puts(" free=0x");
            vibeos_x86_64_serial_print_hex(vibeos_frame_free_count());
            vibeos_x86_64_serial_puts(" base=0x");
            vibeos_x86_64_serial_print_hex((uint64_t)g_hw_pmm.base);
            vibeos_x86_64_serial_puts(" covers_to=0x");
            vibeos_x86_64_serial_print_hex((uint64_t)g_hw_pmm.base +
                                           (uint64_t)g_hw_pmm.size_bytes);
            vibeos_x86_64_serial_puts(" reserved_prefix=0x");
            vibeos_x86_64_serial_print_hex(prefix);
            vibeos_x86_64_serial_puts("\n");
            vibeos_x86_64_serial_unlock();
        } else {
            /* Not fatal, and not silent. The static pool still boots a small
             * machine, and saying so beats a boot that runs out of memory later
             * for a reason nothing recorded. */
            vibeos_x86_64_serial_puts("[MM] no frame layer; the static page pool "
                                     "is all this boot has\n");
        }
    }

    vibeos_x86_64_serial_puts("[HW] PMM online, free bytes=0x");
    vibeos_x86_64_serial_print_hex((uint64_t)vibeos_pmm_remaining(&g_hw_pmm));
    vibeos_x86_64_serial_puts("\n");
}

/* ---- the page cache, wired to the filesystem ----------------------------- */

/* A cache miss reaching the disk. One page, at an offset, through the same
 * read_at the rest of the kernel uses. */
static int hw_cache_read(void *ctx, uint32_t file_id, uint64_t offset,
                         uint64_t phys) {
    long n;

    (void)ctx;
    if (file_id == 0u || file_id > g_cached_file_count) {
        return -1;
    }
    n = vibeos_fs_read_at(&g_rootfs, &g_cached_files[file_id - 1u].node,
                          offset, (void *)(uintptr_t)phys, 4096u);
    if (n < 0) {
        return -1;
    }
    /* A short read is the end of the file, not a failure: the tail page of any
     * file that is not a multiple of 4 KiB. Zero the rest so the page holds the
     * file and nothing else - whatever the frame held before is not part of it,
     * and handing it to a program would be a leak of somebody else's data. */
    {
        uint8_t *p = (uint8_t *)(uintptr_t)phys;
        uint32_t k;
        for (k = (uint32_t)n; k < 4096u; k++) {
            p[k] = 0;
        }
    }
    return 0;
}

/* The bridge to L1.
 *
 * In this kernel a page table's physical address and the pointer the kernel
 * uses to reach it are the same number - that is what the identity map is for -
 * so an address space is fully described by its PML4 and the vmspace handle can
 * be built on the spot rather than stored. Keeping vibeos_hw_aspace_t as it was
 * means the hundred places that read `as->pml4` or load it into CR3 are
 * untouched by this phase. */
vibeos_vmspace_t hw_vm(const vibeos_hw_aspace_t *as) {
    vibeos_vmspace_t v;
    v.root_phys = (uint64_t)(uintptr_t)as->pml4;
    v.root = as->pml4;
    return v;
}

/* Install a 4 KiB mapping. Both windows, one call: the address decides.
 *
 * This is now a wrapper. The walking, the un-sharing of the kernel's low-window
 * tables, the split of a 2 MiB identity leaf, the reference on the frame and
 * the ownership mark all happen in kernel/mm/vmspace.c, which is host-tested.
 * hw_map_low_user_page remains only as a name some callers still use; it does
 * the same thing, because choosing between two mapping functions by address was
 * the caller's job and one caller chose wrong.
 *
 * The flags are passed through rather than translated, because fork installs
 * mappings carrying PTE_COW and the portable protection type has no word for
 * it. The ownership bit is still added by the layer and nowhere else. */
int hw_map_page(vibeos_hw_aspace_t *as, uint64_t va, uint64_t pa,
                       uint64_t leaf_flags) {
    vibeos_vmspace_t v = hw_vm(as);
    return vibeos_vmspace_map_raw(&v, va, pa, leaf_flags);
}

static int hw_map_low_user_page(vibeos_hw_aspace_t *as, uint64_t va, uint64_t pa,
                                uint64_t leaf_flags) {
    if (va >= VIBEOS_HW_IDENTITY_LIMIT) {
        return -1;   /* not this function's business */
    }
    return hw_map_page(as, va, pa, leaf_flags);
}

int hw_elf_read_cached(void *ctx, uint64_t off, uint32_t len, void *buf) {
    hw_elf_cache_reader_t *r = (hw_elf_cache_reader_t *)ctx;
    uint8_t *d = (uint8_t *)buf;
    uint32_t done = 0;

    if (!r || r->file_id == 0u) {
        return -1;
    }
    if (off + (uint64_t)len > r->file_len) {
        return -1;   /* past the end of the file: not this reader's to invent */
    }
    while (done < len) {
        uint64_t page_off = (off + done) & ~0xFFFull;
        uint32_t within = (uint32_t)((off + done) - page_off);
        uint32_t take = 4096u - within;
        uint64_t phys = 0;
        const uint8_t *src;
        uint32_t i;

        if (take > len - done) {
            take = len - done;
        }
        /* Held while it is copied: the cache's own reference is not ours to
         * rely on, because memory pressure evicts on another core. */
        if (vibeos_cache_get_ref(r->file_id, page_off, &phys) != 0 || phys == 0u) {
            return -1;
        }
        src = (const uint8_t *)(uintptr_t)phys;
        for (i = 0; i < take; i++) {
            d[done + i] = src[within + i];
        }
        (void)vibeos_frame_put(phys);
        done += take;
    }
    return 0;
}

int hw_map_elf_image(vibeos_hw_aspace_t *as, vibeos_vma_list_t *vmas,
                            const vibeos_elf_image_t *img, const void *elf,
                            uint32_t file_id, uint64_t file_len) {
    uint64_t va;
    hw_elf_cache_reader_t reader;

    reader.file_id = file_id;
    reader.file_len = file_len;

    for (va = img->min_vaddr; va < img->end_vaddr; va += 4096ull) {
        uint32_t flags = vibeos_elf_page_flags(img, va);
        uint64_t leaf = PTE_PRESENT | PTE_USER;
        uint8_t *page;

        if (flags == 0u) {
            continue;   /* a hole between segments stays unmapped */
        }
        if (flags & VIBEOS_ELF_W) {
            leaf |= PTE_WRITE;
        }
        if (!(flags & VIBEOS_ELF_X)) {
            leaf |= PTE_NX;   /* data, bss, rodata: never code (M-036) */
        }
        /* The page the file already holds, when this page is entirely the
         * file's and nothing may write it.
         *
         * Turned off for a day. It removes 91% of the copying and, when it
         * first landed, took eight boots from a 22/24-with-no-wedge parent to
         * 6/8 with two wedges - so it went behind an if(0) with the
         * measurement written next to it rather than being tuned on a hunch.
         *
         * Measured three times, and off after all three. What the attempts
         * bought is a much smaller space for the next one.
         *
         *   with it on:  21/24, 21/24
         *   with it off: 24/24, 23/24
         *
         * The failures are not random. Two of them were byte-identical -
         * BusyBox, the same rip, the same fault_addr=0xe4 - so this is a
         * deterministic defect reached by a specific path, not a race.
         *
         * Three explanations have been eliminated, each by a measurement
         * rather than by reading the code:
         *
         *  - Wrong content at map time. A probe compared every page mapped
         *    from the cache against what vibeos_elf_fill_page would have built
         *    for it. Zero mismatches across a whole boot.
         *  - Two sources of truth for which file is being loaded.
         *    hw_read_file_cached now reports the identity it actually used and
         *    that identity is threaded through, instead of hw_file_id(path)
         *    being recomputed at the mapping site. The ratio improved - 4242
         *    pages mapped against 237 copied, from 4067/412 - and the failure
         *    did not move.
         *  - Stray writes to a shared page. hw_cache_audit compares every
         *    resident cache page against the file at the end of the boot:
         *    1820 checked, 0 changed.
         *
         * So the bytes are right when they are mapped, they are right at the
         * end, and both sides agree about which file they came from. What is
         * left is the mapping itself - aliasing, a reference, or a lifetime -
         * and that is where the next attempt should start rather than at the
         * top. */
        if (file_id != 0u && !(flags & VIBEOS_ELF_W)) {
            uint64_t foff = 0;
            uint64_t phys = 0;

            if (vibeos_elf_page_file_offset(img, va, &foff) &&
                vibeos_cache_get_ref(file_id, foff, &phys) == 0) {
                int rc = (va < VIBEOS_HW_IDENTITY_LIMIT)
                    ? hw_map_low_user_page(as, va, phys, leaf)
                    : hw_map_page(as, va, phys, leaf);

                /* Ours, taken under the cache's lock, and given back once the
                 * mapping holds its own. This used to take none - "the cache
                 * keeps the reference it has held since it read the page" -
                 * and between the lookup and the mapping an eviction on
                 * another core could free the frame, so the mapping took its
                 * reference on a page already handed to somebody else, and
                 * the process's exit later freed that somebody's page. */
                (void)vibeos_frame_put(phys);
                if (rc != 0) {
                    return -1;
                }
                if (vmas) {
                    (void)vibeos_vma_insert(vmas, va, 4096ull,
                                            (vibeos_prot_t)(VIBEOS_PROT_READ |
                                                            VIBEOS_PROT_USER),
                                            VIBEOS_BACKING_FILE, file_id, foff);
                }
                vibeos_exec_stats()->pages_from_cache++;
                continue;
            }
        }

        page = (uint8_t *)hw_alloc_page();
        if (!page) {
            return -1;
        }
        /* From the cache when there is one, from the staged image otherwise.
         *
         * The two produce the same page - that was checked page by page across
         * a whole boot when the cache mapping was first turned on - and the
         * difference is only where the bytes come from. Preferring the cache
         * is what makes the staging buffer unnecessary for these pages: the
         * file is already in memory, one page at a time, and reading four
         * megabytes to copy a handful of them was the arrangement P4 step 3
         * exists to end.
         *
         * The fallback is not dead code. hw_read_file_cached reports no file
         * identity when the cache could not take the file - a program larger
         * than the cache, or a read that missed - and a load with no identity
         * still has to work. */
        if (file_id != 0u) {
            vibeos_elf_fill_page_via(img, hw_elf_read_cached, &reader,
                                     va, page);
        } else {
            vibeos_elf_fill_page(img, elf, va, page);
        }
        vibeos_exec_stats()->pages_copied++;
        if (va < VIBEOS_HW_IDENTITY_LIMIT) {
            if (hw_map_low_user_page(as, va, (uint64_t)(uintptr_t)page, leaf) != 0) {
                return -1;
            }
        } else if (hw_map_page(as, va, (uint64_t)(uintptr_t)page, leaf) != 0) {
            return -1;
        }
        /* The allocation gave this frame an owner and the mapping gave it a
         * second; the address space is the one that keeps it. Decision D9: a
         * caller that allocates in order to map hands the frame over and lets
         * go, exactly as alloc_page/put_page do. Forgetting this leaks the page
         * - visible in meminfo - rather than freeing one somebody is using. */
        hw_page_put((uint64_t)(uintptr_t)page);

        /* Record the page as a region, one at a time so the list merges runs
         * of alike pages and leaves the gaps between segments as gaps. One
         * region spanning the whole image would describe its holes as mapped,
         * and mprotect would then accept an address the ABI self-test requires
         * it to refuse. */
        if (vmas) {
            vibeos_prot_t rp = (vibeos_prot_t)(VIBEOS_PROT_READ | VIBEOS_PROT_USER);
            if (flags & VIBEOS_ELF_W) {
                rp = (vibeos_prot_t)(rp | VIBEOS_PROT_WRITE);
            }
            (void)vibeos_vma_insert(vmas, va, 4096ull, rp,
                                    VIBEOS_BACKING_ANON, 0, 0);
        }
    }
    return 0;
}

int hw_aspace_create(vibeos_hw_aspace_t *as) {
    vibeos_vmspace_t v;

    if (vibeos_vmspace_create(&v) != 0) {
        return -1;
    }
    as->pml4 = v.root;
    return 0;
}

/* Free an address space: the user subtree (PML4 slot 1 - pages and the tables
 * that map them) and the PML4 itself. The shared kernel identity map (slot 0)
 * is static and never freed. Must not be called while this CR3 is active. */
/* Who last tore an address space down, and which one.
 *
 * proc.as.pml4 going to zero is the one unexplained step left in the wedge,
 * and this function is its only writer. Three readings of the call sites have
 * been wrong, so the function records what it did rather than being reasoned
 * about. */
const char *g_last_destroy_why;
uint64_t g_last_destroy_pml4;
uint32_t g_last_destroy_cpu;

void hw_aspace_destroy_why(vibeos_hw_aspace_t *as, const char *why) {
    vibeos_vmspace_t v;

    if (!as->pml4) {
        return;
    }
    /* Kept exactly as it was, and for the reason CLAUDE.md records: this is the
     * only writer of as->pml4 going to zero, three readings of its call sites
     * have been wrong, and the guard in the context switch is what turned a
     * machine that stopped silently into a machine that says why. */
    g_aspace_being_destroyed = as->pml4;

    /* Everything below this line used to be four nested loops deciding, at each
     * level, whether an entry was the process's to free - by asking whether it
     * was present, whether it carried PTE_USER, whether it sat in the low
     * window. Those are permissions being asked a question about ownership, and
     * they gave the wrong answer in both directions: a PROT_NONE thread stack
     * was leaked because it is not user-reachable, and a split identity entry
     * was freed because it is present and writable, which handed the kernel's
     * own page tables back to the allocator.
     *
     * There is nothing to decide now. vibeos_vmspace_destroy releases the
     * entries carrying the ownership bit, which are exactly the ones
     * vibeos_vmspace_map installed, and leaves everything else alone because it
     * was never marked - not because of a test that could be wrong. */
    v = hw_vm(as);
    (void)vibeos_vmspace_destroy(&v);

    g_last_destroy_why = why;
    g_last_destroy_pml4 = (uint64_t)(uintptr_t)as->pml4;
    g_last_destroy_cpu = hw_this_cpu()->index;
    g_aspace_being_destroyed = 0;
    as->pml4 = 0;
}

/* Build the kernel's page tables: identity-map the first N GiB with 2 MiB
 * supervisor pages (US=0) and switch CR3 to them. */
void hw_enable_paging(void) {
    uint32_t g, e;

    vibeos_x86_64_serial_puts("[HW] building kernel page tables (identity 4GiB, supervisor-only)\n");
    for (g = 0; g < VIBEOS_HW_IDENTITY_GIB; g++) {
        for (e = 0; e < 512u; e++) {
            uint64_t phys = ((uint64_t)g * 0x40000000ull) + ((uint64_t)e * 0x200000ull);
            g_pd[g][e] = phys | PTE_PRESENT | PTE_WRITE | PTE_PS;
        }
        g_pdpt[g] = (uint64_t)(uintptr_t)&g_pd[g][0] | PTE_PRESENT | PTE_WRITE;
    }
    g_pml4[0] = (uint64_t)(uintptr_t)&g_pdpt[0] | PTE_PRESENT | PTE_WRITE;

    hw_write_cr3((uint64_t)(uintptr_t)&g_pml4[0]);

    vibeos_x86_64_serial_puts("[HW] CR3 loaded with kernel-owned tables: 0x");
    vibeos_x86_64_serial_print_hex(hw_read_cr3());
    vibeos_x86_64_serial_puts("\n[HW] PAGING_OK (kernel tables, user pages isolated)\n");
}

/* Mark an identity-mapped physical range uncacheable.
 *
 * The identity map is built above with plain write-back 2 MiB pages, which is
 * right for memory and wrong for a device's registers: reads there have side
 * effects, and a cached one returns what the register said last time. A driver
 * polling a completion bit would then spin on a stale copy forever. Whole 2 MiB
 * pages are marked, because that is the granularity the map has - the ranges
 * this is used for are device BARs, which nothing else shares.
 *
 * PCD|PWT rather than a PAT entry: it is UC- on every processor that has ever
 * run this, needs no MSR set up first, and the difference from true UC does not
 * matter for a BAR. */
void vibeos_x86_64_mark_uncacheable(uint64_t phys, uint64_t len) {
    uint64_t addr;

    if (len == 0ull || phys >= VIBEOS_HW_IDENTITY_LIMIT) {
        return;
    }
    for (addr = phys & ~0x1FFFFFull; addr < phys + len; addr += 0x200000ull) {
        uint32_t g = (uint32_t)(addr / 0x40000000ull);
        uint32_t e = (uint32_t)((addr % 0x40000000ull) / 0x200000ull);

        if (g >= VIBEOS_HW_IDENTITY_GIB) {
            break;
        }
        g_pd[g][e] |= PTE_PCD | PTE_PWT;
    }
    /* Reload CR3 to drop the TLB entries that still describe the old
     * attributes. INVLPG per page would do, but this runs once per device at
     * bring-up and correctness is worth more here than the microseconds. */
    hw_write_cr3(hw_read_cr3());
}


/* Does every resident cache page still hold what the file holds?
 *
 * The question exists because these pages stopped being private. Once a
 * read-only image page is mapped from the cache instead of copied, one frame is
 * the text of every process running that program - so a single stray write
 * reaches all of them, and it reaches them as a program misbehaving somewhere
 * far from whatever did the writing.
 *
 * Verified against the file rather than against a checksum taken at fill time:
 * a checksum proves the bytes have not changed since the kernel last looked,
 * and the file is what they are supposed to be. Slow, and run once from the
 * console at the end of a boot, which is when it is worth knowing.
 *
 * Reported as a count. That count is asserted zero by the boot gate *now* -
 * this sentence used to say so while it was false, for as long as the audit
 * had existed. The line was printed on every boot, 1821 pages compared against
 * their files, and nothing read the answer. A guarantee documented in the
 * source and provided by nobody is worse than an undocumented gap, because the
 * sentence is what the next reader trusts instead of checking. The counter
 * carries MUSTBEZERO now so check-mustbezero-asserted.py holds it there. */
static uint8_t g_cache_audit_page[4096] __attribute__((aligned(16)));

static void hw_cache_audit(uint64_t *out_checked, uint64_t *out_bad) {
    uint32_t i;
    uint64_t checked = 0, bad = 0;

    for (i = 0; i < VIBEOS_HW_CACHE_ENTRIES; i++) {
        const vibeos_cache_entry_t *e = &g_cache_table[i];
        const uint8_t *have;
        uint64_t n;
        uint32_t k;
        int differs = 0;

        if (e->file_id == 0u || e->phys == 0u) {
            continue;
        }
        if (e->file_id > g_cached_file_count) {
            continue;
        }
        n = vibeos_fs_read_at(&g_rootfs, &g_cached_files[e->file_id - 1u].node,
                              e->offset, g_cache_audit_page,
                              sizeof(g_cache_audit_page));
        if ((int64_t)n <= 0) {
            continue;   /* cannot read it now; not evidence about the page */
        }
        have = (const uint8_t *)(uintptr_t)e->phys;
        for (k = 0; k < (uint32_t)n; k++) {
            if (have[k] != g_cache_audit_page[k]) {
                differs = 1;
                break;
            }
        }
        checked++;
        if (differs) {
            bad++;
            vibeos_x86_64_serial_lock();
            vibeos_x86_64_serial_puts("[EXEC] CACHE_PAGE_CHANGED file=0x");
            vibeos_x86_64_serial_print_hex((uint64_t)e->file_id);
            vibeos_x86_64_serial_puts(" offset=0x");
            vibeos_x86_64_serial_print_hex(e->offset);
            vibeos_x86_64_serial_puts(" phys=0x");
            vibeos_x86_64_serial_print_hex(e->phys);
            vibeos_x86_64_serial_puts(" byte=0x");
            vibeos_x86_64_serial_print_hex((uint64_t)k);
            vibeos_x86_64_serial_puts(" file=0x");
            vibeos_x86_64_serial_print_hex((uint64_t)g_cache_audit_page[k]);
            vibeos_x86_64_serial_puts(" memory=0x");
            vibeos_x86_64_serial_print_hex((uint64_t)have[k]);
            vibeos_x86_64_serial_puts("\n");
            vibeos_x86_64_serial_unlock();
        }
    }
    *out_checked = checked;
    *out_bad = bad;
}

/* ---- user memory, page-table primitives, the shootdown, copy-on-write ------
 *
 * The second half of the bridge, moved from arch_hw.c's "shared with the Linux
 * ABI layer" stretch (2026-09-28): validating a user range, walking and
 * invalidating entries, telling the other cores (the IPI and its wait), the
 * copy-on-write fault and fork's copy, and the walk that asks whether a frame
 * is still mapped by anyone. Moved without change. */

hw_cow_rec_t g_cow_ring[HW_COW_RING];
static uint64_t g_cow_ring_at;
/* Copy-on-write resolutions after which the page's contents changed.
 * A copy that is a copy leaves this at zero. */
uint64_t g_cow_copy_changed;
/* Copy-on-write faults resolved, of any kind. See the fold. */
uint64_t g_cow_resolved;

/* Validate that [va, va+len) is mapped in the *calling task's* address space
 * and reachable from ring 3. Without this the kernel would happily dereference
 * any pointer a user task passes - including kernel addresses. */
int hw_user_range_ok(uint64_t va, uint64_t len, int need_write) {
    uint32_t why = HW_RANGE_OK;
    return hw_user_range_why(va, len, need_write, &why);
}

static int hw_stack_may_reach(uint64_t va);

int hw_user_range_why(uint64_t va, uint64_t len, int need_write,
                             uint32_t *why) {
    static const uint32_t shifts[3] = {39u, 30u, 21u};
    const hw_task_t *t;
    uint64_t page;

    *why = HW_RANGE_OK;
    if (len == 0) {
        return 1;
    }
    if (va + len < va) {
        *why = HW_RANGE_WRAP;
        return 0;
    }
    if (g_current_task < 0 || !hw_task_is_user_of(&g_tasks[g_current_task])) {
        *why = HW_RANGE_NO_TASK;
        return 0;
    }
    t = &g_tasks[g_current_task];

    for (page = va & ~0xFFFull; page < va + len; page += 4096ull) {
        const uint64_t *tbl = t->proc.as.pml4;
        uint64_t e = 0;
        uint32_t level;
        int grows = 0;

        for (level = 0; level < 3u; level++) {
            e = tbl[(page >> shifts[level]) & 0x1FFu];
            if ((e & PTE_PRESENT) == 0 && hw_stack_may_reach(page)) {
                grows = 1;   /* no table yet: an untouched part of the stack */
                break;
            }
            if ((e & PTE_PRESENT) == 0 || (e & PTE_USER) == 0) {
                *why = HW_RANGE_LEVEL0 + level;
                return 0;
            }
            if (level == 2u && (e & PTE_PS) != 0) {
                break; /* 2 MiB leaf */
            }
            tbl = (const uint64_t *)(uintptr_t)(e & 0x000FFFFFFFFFF000ull);
        }
        /* A page of the stack nobody has touched yet is the process's to read
         * and write: the copy that follows faults, and the fault maps it
         * (hw_stack_grow). Refused here, a read() into a large buffer on the
         * stack - which is where programs keep them - would be EFAULT for
         * every page the program had not happened to touch first. */
        if (!grows && (e & PTE_PS) == 0 && hw_stack_may_reach(page) &&
            tbl[(page >> 12) & 0x1FFu] == 0ull) {
            grows = 1;
        }
        if (grows) {
            continue;
        }
        if ((e & PTE_PS) == 0) {
            e = tbl[(page >> 12) & 0x1FFu];
            /* A page in swap is mapped: the copy that follows faults and the
             * fault brings it back (M-063). Its permissions are in the entry's
             * ignored bits, so the user and write tests below still hold. */
            if (((e & PTE_PRESENT) == 0 && (e & VIBEOS_PTE_SWAPPED) == 0) ||
                (e & PTE_USER) == 0) {
                *why = HW_RANGE_LEAF;
                return 0;
            }
        }
        /* A copy-on-write page has its write bit cleared on purpose: the
         * process may write it, and doing so faults so the page can be
         * duplicated first. The hardware bit is the mechanism, not the
         * permission, so refusing the buffer here rejects writes that are
         * perfectly legal - which made every read() into freshly forked
         * memory return EFAULT, and a shell report end of input. */
        /* A page a swap-out is writing is writable: a store faults and cancels
         * the swap-out (M-056). Refused here, a syscall writing a buffer that
         * reclaim happened to be evicting would get EFAULT for nothing - the
         * second of the three places copy-on-write taught this file about. */
        if (need_write && (e & PTE_WRITE) == 0 && (e & PTE_COW) == 0 &&
            (e & VIBEOS_PTE_SWAPOUT) == 0) {
            *why = HW_RANGE_READONLY;
            return 0;
        }
    }
    return 1;
}

int hw_copy_user_string(uint64_t uptr, char *dst, int max); /* defined below */

/* Find the leaf page-table entry for `va`, or NULL if nothing maps it.
 * Deliberately does not create tables: callers are changing or removing an
 * existing mapping, and silently materialising one would hide a bad address. */
uint64_t *hw_pte_lookup(vibeos_hw_aspace_t *as, uint64_t va) {
    static const uint32_t shifts[3] = {39u, 30u, 21u};
    uint64_t *tbl = as->pml4;
    uint32_t level;

    for (level = 0; level < 3u; level++) {
        uint64_t e = tbl[(va >> shifts[level]) & 0x1FFu];
        if ((e & PTE_PRESENT) == 0) {
            return 0;
        }
        if (level == 2u && (e & PTE_PS) != 0) {
            return 0;   /* a 2 MiB leaf; user mappings are 4 KiB */
        }
        tbl = (uint64_t *)(uintptr_t)(e & 0x000FFFFFFFFFF000ull);
    }
    {
        uint64_t *pte = &tbl[(va >> 12) & 0x1FFu];
        return (*pte & PTE_PRESENT) ? pte : 0;
    }
}

/* Drop one page from the TLB. Changing a PTE without this leaves the old
 * translation cached, so a revoked write permission is not actually revoked
 * until something else happens to flush it. */
void hw_invlpg(uint64_t va) {
    __asm__ __volatile__("invlpg (%0)" : : "r"((void *)(uintptr_t)va) : "memory");
}

/* Make every other core forget the mappings it has cached.
 *
 * invlpg reaches the calling core's TLB and nothing else. That was enough
 * while a page's permissions only ever changed for a process that was not
 * running anywhere - but fork revokes write permission on pages of a process
 * whose *other threads* may be executing on other cores right now, with the
 * old writable entry still in their TLBs. Those threads then write straight
 * through into a page the child has just been given a share of, and the
 * corruption surfaces much later, in whichever program the page ended up
 * serving.
 *
 * The wait is what makes it a barrier rather than a hint: when this returns,
 * no other core can still be writing through the permission just revoked.
 *
 * Bounded, and it says so if it gives up. A shootdown that hangs would be a
 * silent machine, which is strictly worse than the bug it is fixing - and a
 * core that never answers is itself worth knowing about. */
static void hw_tlb_shootdown(uint64_t cr3) {
    uint32_t i;
    int targets = 0;
    uint64_t spins = 0;
    uint32_t me;
    uint32_t mask = 0;
    uint64_t snap[VIBEOS_HW_MAX_CPUS];

    if (!g_apic_mode || g_cpu_online_count <= 1u) {
        return;   /* nobody else can be holding a stale entry */
    }
    me = hw_this_cpu()->index;
    cr3 &= ~0xFFFull;

    /* Only the cores actually running this address space can hold a stale
     * entry for it, and only they need telling.
     *
     * The first version broadcast to everybody and waited for all of them,
     * and it timed out about three boots in thirty-two. `syscall` clears IF
     * (SFMASK is 0x200), so every core sitting in a syscall - and with this
     * much serial output, that is most of them, most of the time - cannot
     * take the IPI at all until it returns to ring 3. Waiting on cores that
     * have nothing to do with the mapping was buying nothing and paying for
     * it in stalls.
     *
     * A single-threaded fork, which is nearly all of them, now sends no IPI
     * and waits for nothing.
     *
     * "Running" means what is in the core's CR3, not what its current task
     * says. The two differ while a task tears down its own address space from
     * the kernel's tables: its slot still names the space, the core holds
     * nothing of it, and it waits for a reclaim claim with interrupts off. The
     * first load to force reclaim targeted exactly that core from a swap-out
     * holding the claim, and the machine stopped. The same two fields the
     * unmap quarantine reads (M-061), under the same ordering argument: the
     * caller changed the entry before this, and a core that announces the
     * space after this read walks tables that already have the change.
     *
     * Chosen once, recorded, and waited on per core. There used to be one
     * machine-wide pending count, reset to zero by every shootdown - so two at
     * once wiped each other's count, and one could return before its targets
     * had flushed. Swap-out makes concurrent shootdowns ordinary. */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    for (i = 0; i < VIBEOS_HW_MAX_CPUS; i++) {
        if (i == me || !g_cpus[i].online) {
            continue;
        }
        /* The generation first: a flush after this sample is a flush after
         * the caller's change. */
        snap[i] = __atomic_load_n(&g_cpus[i].cr3_generation, __ATOMIC_ACQUIRE);
        if (__atomic_load_n(&g_cpus[i].loading_cr3, __ATOMIC_ACQUIRE) != cr3 &&
            __atomic_load_n(&g_cpus[i].loaded_cr3, __ATOMIC_ACQUIRE) != cr3) {
            continue;
        }
        mask |= 1u << i;
        targets++;
    }
    if (targets == 0) {
        return;
    }
    __atomic_fetch_add(&g_tlb_shootdowns, 1ull, __ATOMIC_RELAXED);
    __atomic_fetch_add(&vibeos_mm_stats()->tlb_targets, (uint64_t)targets,
                       __ATOMIC_RELAXED);
    for (i = 0; i < VIBEOS_HW_MAX_CPUS; i++) {
        if ((mask & (1u << i)) == 0u) {
            continue;
        }
        if (__atomic_exchange_n(&g_cpus[i].flush_req, 1u, __ATOMIC_ACQ_REL) == 0u) {
            __atomic_add_fetch(&g_tlb_flush_live, 1u, __ATOMIC_ACQ_REL);
        }
        vibeos_x86_64_lapic_ipi_one(g_cpus[i].lapic_id, 0xFEu);
    }

    /* Bounded, and it says so if it gives up. A shootdown that hangs would be
     * a silent machine, which is strictly worse than the bug it is fixing -
     * and a core that never answers is itself worth knowing about.
     *
     * Waiting with interrupts off, so while waiting this core answers anybody
     * waiting on it - two cores shooting down at each other would otherwise
     * wait forever. */
    /* Counted per target as each is *seen* to have flushed, so a shootdown
     * that stops waiting early reads as fewer flushes than targets. The
     * acknowledgement count cannot say that any more: two requests to one core
     * are answered by one flush. */
    for (;;) {
        uint32_t pending = 0;

        for (i = 0; i < VIBEOS_HW_MAX_CPUS; i++) {
            if ((mask & (1u << i)) == 0u) {
                continue;
            }
            if (__atomic_load_n(&g_cpus[i].cr3_generation, __ATOMIC_ACQUIRE) == snap[i]) {
                pending++;
            } else {
                mask &= ~(1u << i);
                __atomic_fetch_add(&vibeos_mm_stats()->tlb_flushed, 1ull,
                                   __ATOMIC_RELAXED);
            }
        }
        if (pending == 0u) {
            break;
        }
        hw_tlb_service_flush();
        __asm__ __volatile__("pause" ::: "memory");
        if (++spins > 200000000ull) {
            vibeos_mm_stats()->tlb_timeouts++;
            hw_log(VIBEOS_LOG_ERROR, 30u, (uint64_t)targets, cr3,
                   "TLB shootdown timed out; a core did not acknowledge");
            /* And then it *returned*, and the caller carried on.
             *
             * That is what an external review found, and it was right. This
             * function is not an optimisation: every caller has already
             * narrowed a permission or removed a mapping, and is relying on
             * this to make the change true on the other cores. Returning
             * quietly means a core keeps writing through a permission that has
             * been revoked - straight into a page a fork has just shared, with
             * no copy-on-write fault, and the damage surfacing later in
             * whichever program the page ends up serving.
             *
             * There was no way for a caller to know, either: the backend hook
             * is `void (*shootdown)(uint64_t)`, so none of its five call sites
             * in kernel/mm/vmspace.c *could* have checked.
             *
             * Nothing can be rolled back here. The entry was written before the
             * shootdown was asked for - it has to be, or there is nothing to
             * invalidate - so "do not complete the operation" is not available
             * by the time this is reached. The deferred reclamation that closed
             * the munmap gap does not transfer either: there a frame could be
             * parked until every core was quiescent, and here the resource in
             * danger is not a frame but a permission that is already narrowed.
             *
             * So the only honest choices are to keep waiting or to stop, and a
             * core that has not answered in two hundred million spins is not
             * about to. Stopping is this project's stated position, in its own
             * words: the difference between a machine that stops and a machine
             * that says why. A silent memory-corruption path is strictly worse
             * than a named halt.
             *
             * If this ever fires in practice, the fix is not to soften it. It
             * is the one the munmap comment already names: stop masking
             * interrupts for the whole of a syscall, so a target can answer. */
            hw_panic("TLB shootdown timed out: a core still holds a "
                     "translation the caller has already revoked");
        }
    }
}

/* Resolve a write to a copy-on-write page.
 *
 * Returns non-zero when the fault was handled and execution may resume.
 *
 * Every condition is checked rather than assumed, because being wrong here
 * turns a memory-protection violation into a silent success. The fault must be
 * a write (bit 1), from user space (bit 2), to a page that is present (bit 0)
 * - a not-present fault is a genuine bad access, not a shared page - and the
 * entry must carry our own copy-on-write bit. A read-only page without that
 * bit is read-only because the program is not allowed to write it. */
#ifndef VIBEOS_COW_FAULT_TRACE
#define VIBEOS_COW_FAULT_TRACE 0
#endif

/* Is this an address the stack may grow to? The region below the stack's top,
 * less its lowest page - the guard, which is never mapped. */
static int hw_stack_may_reach(uint64_t va) {
    return va >= VIBEOS_HW_USER_STACK_FLOOR && va < VIBEOS_HW_USER_STACK_TOP;
}

/* The stack grows: a touch of a page in its region that nothing maps is given
 * a zeroed page (docs/abi/ L3).
 *
 * 1 when the fault is dealt with and the access should be tried again, 0 when
 * it is not this function's.
 *
 * The region list is asked, not only the address: the region was put there at
 * exec, and a program that unmapped part of it, or took its write permission
 * away, has said what it wants. The mapping is made under the process's mm
 * lock - a thread of the same process may be forking, or faulting on this very
 * page, on another core - and the lock is *tried*, not waited for: this is a
 * fault handler, and returning to fault again is how it waits. If the entry is
 * no longer empty once the lock is held, somebody else mapped the page; the
 * retry finds it there.
 *
 * The frame comes from the privileged door, as a page coming back from swap
 * does: the stack is memory the process was promised at exec, and refusing it
 * at the low watermark kills the process for using what it has. It is bounded
 * - two megabytes a process - which is what makes that affordable. */
static int hw_stack_grow(hw_task_t *t, uint64_t fault_va) {
    const uint64_t page_va = fault_va & ~0xFFFull;
    vibeos_procstate_t *ps = t->ps;
    vibeos_vmspace_t sv = hw_vm(&t->proc.as);
    const vibeos_vma_t *region;
    uint64_t *entry;
    uint32_t zero = 0;
    void *page;
    int done = 0;

    if (!ps || !hw_stack_may_reach(page_va)) {
        return 0;
    }
    if (!__atomic_compare_exchange_n(&ps->mm_busy, &zero, (uint32_t)(g_current_task + 1), 0,
                                     __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
        /* Held by this task itself (ks_mm_lock records the holder's slot plus
         * one, as this claim does): a handler stored to user memory under
         * ks_mm_lock, and "fault again" would loop for ever with interrupts
         * off - mincore did, until the external review of 2026-10-07. Stop
         * with the reason instead. */
        if (zero == (uint32_t)(g_current_task + 1)) {
            hw_panic("stack fault under this task's own mm lock: a user store made while holding it");
        }
        return 1;   /* somebody is changing this address space: fault again */
    }
    region = vibeos_vma_find(&ps->vmas, page_va);
    entry = vibeos_vmspace_entry(&sv, page_va);
    if (entry && *entry != 0ull) {
        done = 1;   /* mapped while this waited for the lock */
    } else if (region && region->backing == VIBEOS_BACKING_ANON &&
               (region->prot & VIBEOS_PROT_WRITE) && (region->prot & VIBEOS_PROT_USER) &&
               (page = hw_alloc_page()) != 0) {
        /* Zeroed already: the allocator hands out nothing else. */
        if (hw_map_page(&t->proc.as, page_va, (uint64_t)(uintptr_t)page,
                        PTE_PRESENT | PTE_WRITE | PTE_USER | PTE_NX) == 0) {
            vibeos_mm_stats()->stack_grown++;
            done = 1;
        }
        hw_page_put((uint64_t)(uintptr_t)page);   /* the mapping owns it, or nobody does */
    }
    __atomic_store_n(&ps->mm_busy, 0u, __ATOMIC_RELEASE);
    return done;
}

int hw_handle_cow_fault(uint64_t fault_va, uint64_t error_code,
                               uint64_t rip) {
    hw_task_t *t;
    vibeos_vmspace_t v;
    int handled;

    if (g_current_task < 0 || !hw_task_is_user_of(&g_tasks[g_current_task])) {
        return 0;
    }
    t = &g_tasks[g_current_task];

    /* A page that is not present may be one this kernel sent to swap, and it
     * is checked before the copy-on-write test rather than after it. A swapped
     * entry has no present bit, so the present-and-write condition below would
     * reject it and the task would be killed for touching memory it owns.
     *
     * Read or write - either faults - so nothing is asked of the error code
     * beyond the page not being present. */
    if ((error_code & 0x1u) == 0u) {
        vibeos_vmspace_t sv = hw_vm(&t->proc.as);

        if (vibeos_vmspace_swap_slot(&sv, fault_va) >= 0 &&
            hw_swap_bring_in(&sv, fault_va) == 0) {
            return 1;
        }
        /* Falls through on failure rather than retrying: a retry on the same
         * entry faults again forever, and the path below reports it. */
        if (hw_stack_grow(t, fault_va)) {
            return 1;
        }
    }

    /* Present and write. The originating privilege level is deliberately not
     * required to be user: the kernel writes into user memory on a process's
     * behalf - read() filling a buffer, a syscall storing a result - and with
     * CR0.WP set those writes fault on a read-only page exactly as ring 3
     * would. Insisting on the user bit here refuses precisely the faults that
     * happen while serving a syscall, which is how a freshly forked shell dies
     * without printing anything. */
    if ((error_code & 0x3u) != 0x3u) {
        return 0;
    }

    /* The decision, the copy, the reference arithmetic and the shootdown all
     * live in L1 now, where a host test can drive them. What stays here is what
     * is genuinely architectural: reading the error code, knowing which task
     * faulted, and saying so in the log. */
    v = hw_vm(&t->proc.as);
    /* Is the copy faithful?
     *
     * A user buffer that loses its first sixteen bytes while keeping the rest
     * is either a store that never landed or a copy that dropped it, and
     * nothing recorded so far separates those. A fold of the whole page before
     * and after the resolution does: the page is readable throughout - the
     * fault is a write-protection fault on a present page - so the same bytes
     * can be read twice, once through the shared mapping and once through
     * whatever this call installed.
     *
     * A fold rather than a byte sample, because guessing which offset matters
     * is how a detector ends up measuring its own assumption. If the folds
     * differ, the copy is not a copy. If they agree, the data was already gone
     * and the store is the thing to chase.
     *
     * 512 reads twice per fault, on a path that runs a few hundred times a
     * boot. Left unconditional deliberately: this is cheap next to the copy it
     * checks, and a check that has to be turned on is one that is off when the
     * defect happens. */
    {
        const uint64_t *pg = (const uint64_t *)(uintptr_t)(fault_va & ~0xFFFull);
        uint64_t fold_before = 0ull, fold_after = 0ull;
        uint32_t q;

        for (q = 0; q < 512u; q++) {
            fold_before = (fold_before * 31ull) ^ pg[q];
        }
        handled = vibeos_vmspace_fault(&v, fault_va, 1);
        if (handled) {
            for (q = 0; q < 512u; q++) {
                fold_after = (fold_after * 31ull) ^ pg[q];
            }
            /* Every resolution, so the fold's zero can be read against how
             * many of them could have moved it.
             *
             * The sole-owner fast path grants write on the *same* frame - no
             * copy at all - and there the fold compares a frame with itself
             * and is zero by construction. mm_stats already counts the copies
             * (`copied` on the COW_STATS line); this counts the resolutions.
             * The difference between the two is the number of faults for which
             * cow_copy_changed could never have been anything but zero. */
            __sync_fetch_and_add(&g_cow_resolved, 1ull);
            if (fold_after != fold_before) {
                g_cow_copy_changed++;
                vibeos_x86_64_serial_lock();
                vibeos_x86_64_serial_puts("[MM] COW_COPY_CHANGED va=0x");
                vibeos_x86_64_serial_print_hex(fault_va);
                vibeos_x86_64_serial_puts(" rip=0x");
                vibeos_x86_64_serial_print_hex(rip);
                vibeos_x86_64_serial_puts(" pid=0x");
                vibeos_x86_64_serial_print_hex((uint64_t)hw_task_pid_of(t));
                vibeos_x86_64_serial_puts(" before=0x");
                vibeos_x86_64_serial_print_hex(fold_before);
                vibeos_x86_64_serial_puts(" after=0x");
                vibeos_x86_64_serial_print_hex(fold_after);
                vibeos_x86_64_serial_puts("\n");
                vibeos_x86_64_serial_unlock();
            }
        }
    }
    /* Off by default, and on when a run is chasing this.
     *
     * A line per copy-on-write fault is hundreds of lines in a boot: it floods
     * the log the gate reads, and it changes the timing of the very defect it
     * is looking for. An unhandled fault is rare and always worth a line, so
     * that half is unconditional.
     *
     * Build with -DVIBEOS_COW_FAULT_TRACE=1 to see every one. What it answered
     * once already: the page that loses a wide store faults exactly once, so
     * the store is lost after a successful resolution rather than to a second
     * fault nobody handled. */
    /* One line, not three, and it carries the faulting rip.
     *
     * The open question is whether a page that loses a wide store faults once
     * or twice - if once, the store was lost after a successful resolution; if
     * twice, the second fault is the interesting one. Neither the address nor
     * the outcome can answer that on its own, and a diagnostic split across
     * several calls comes back interleaved from different cores and reads as a
     * contradiction. */
    /* Kept whether or not it is printed.
     *
     * The trace above floods - hundreds of lines a boot - and its own comment
     * says it changes the timing of the defect it is looking for. So every
     * fault is recorded into a small ring instead, and the ring is dumped by
     * whoever finds a corrupted user buffer, filtered to the page that was
     * corrupted. That is this project's rule about taking the state at the
     * crash rather than going back for it: by the time a write reports zeros,
     * the fault that produced them is long over.
     *
     * Sixteen entries, oldest overwritten. The question it exists to answer is
     * how many times the page under a lost store faulted and with what error
     * code - a not-present fault and a write-protection fault are different
     * defects and the address alone cannot tell them apart. */
    {
        uint32_t slot = (uint32_t)(__sync_fetch_and_add(&g_cow_ring_at, 1ull) &
                                   (HW_COW_RING - 1u));
        g_cow_ring[slot].va = fault_va;
        g_cow_ring[slot].rip = rip;
        g_cow_ring[slot].err = error_code;
        g_cow_ring[slot].pid = (uint64_t)hw_task_pid_of(t);
        g_cow_ring[slot].handled = (uint64_t)handled;
        g_cow_ring[slot].cpu = (uint64_t)vibeos_x86_64_cpu_id();
    }

    if (VIBEOS_COW_FAULT_TRACE || !handled) {
        vibeos_x86_64_serial_lock();
        vibeos_x86_64_serial_puts("[MM] COW_FAULT va=0x");
        vibeos_x86_64_serial_print_hex(fault_va);
        vibeos_x86_64_serial_puts(" rip=0x");
        vibeos_x86_64_serial_print_hex(rip);
        vibeos_x86_64_serial_puts(" err=0x");
        vibeos_x86_64_serial_print_hex(error_code);
        vibeos_x86_64_serial_puts(" pid=0x");
        vibeos_x86_64_serial_print_hex((uint64_t)hw_task_pid_of(t));
        vibeos_x86_64_serial_puts(handled ? " handled\n" : " NOT-handled\n");
        vibeos_x86_64_serial_unlock();
    }
    if (handled) {
        hw_log(VIBEOS_LOG_DEBUG, 43u, fault_va,
               (uint64_t)(uintptr_t)t->proc.as.pml4,
               "copy-on-write fault resolved (a0 = address, a1 = address space)");
    }
    return handled;
}

/* fork, in one call.
 *
 * What used to be here was three functions and about a hundred and forty lines:
 * one walking the high window, one walking the low window, and one deciding
 * per page how to share it. They disagreed with each other in a way that only
 * showed up in a threaded program - the high-window walk took every entry that
 * was present, the low-window walk took only entries marked PTE_USER, and a
 * PROT_NONE thread guard is present and not user-reachable. A child forked
 * from a threaded process therefore inherited an address space with holes
 * where its guards belonged.
 *
 * There is one walk now, over the entries carrying the ownership mark, and it
 * is the same walk the inspection count uses so the two cannot drift. The
 * sharing rules, the parent's revoked write permission and the single
 * shootdown at the end all moved with it, into code a host test can drive. */
int hw_aspace_copy_user(vibeos_hw_aspace_t *dst, vibeos_hw_aspace_t *src) {
    vibeos_vmspace_t d = hw_vm(dst);
    vibeos_vmspace_t sp = hw_vm(src);

    return vibeos_vmspace_clone_cow(&d, &sp);
}

/* Copy a NUL-terminated string from user space, validating each byte's page. */
int hw_copy_user_string(uint64_t uptr, char *dst, int max) {
    int i;
    for (i = 0; i < max - 1; i++) {
        if (!linux_user_ok(uptr + (uint64_t)i, 1, 0)) {
            return -1;
        }
        /* Fault-safe: the range check and the read are two instants, and a
         * sibling thread can munmap the page between them (H-019). */
        if (vibeos_uaccess_copy(&dst[i],
                (const void *)(uintptr_t)(uptr + (uint64_t)i), 1u) != 0) {
            return -1;
        }
        if (dst[i] == 0) {
            return 0;
        }
    }
    dst[max - 1] = 0;
    return 0;
}

/* Is this an address a process is allowed to own? There are two user windows -
 * the high one VibeOS programs are linked into and the low one a Linux
 * executable is linked into - and the answer lives in one place so a new
 * caller cannot accidentally know about only one of them. */
int hw_user_addr_ok(uint64_t va) {
    if (va >= VIBEOS_HW_USER_BASE && va < VIBEOS_HW_USER_BASE + 0x8000000000ull) {
        return 1;
    }
    return va >= VIBEOS_HW_LOW_USER_BASE && va < VIBEOS_HW_LOW_USER_LIMIT;
}

/* Signal delivery and rt_sigreturn moved to linux_signal.c. Building a
 * frame on a user stack and taking it back again is Linux ABI, not x86. */


/* Turn a ring-3 CPU exception into the death of one task. The signal numbers
 * are the ones Linux reports for these vectors, so a shell that prints
 * "Segmentation fault" is printing the same thing it would there. */
/* Is this frame still mapped by any live user task?
 *
 * Freeing a page somebody is still running on has now been diagnosed four
 * times in this project, from the far end each time, and fixed once. The
 * evidence always arrives late: a musl heap in knots, a stack full of poison,
 * a child reading a third program's data. This asks the question at the moment
 * the mistake is made.
 *
 * Walks the user portion of every live address space. That is not cheap, which
 * is why the caller samples rather than checking every free - a bug that has
 * shown up once every twenty boots does not need catching on the first
 * attempt, it needs catching at all, with the culprit named.
 */
/* Follow a page-table entry, or refuse to.
 *
 * This walk reads the tables of every live task without holding the scheduler
 * lock, because it runs on the frame layer's release path and taking that lock
 * there would invert the order everything else uses. The consequence is that it
 * can read a table that another core has just freed - and a freed frame that
 * has been handed out again holds arbitrary bytes, some of which have the
 * present bit set and an address field pointing anywhere at all.
 *
 * Following one of those faulted the kernel. The detector built to catch a
 * page vanishing under a process was itself panicking the machine, roughly
 * twice in twenty-four boots, and the failures were being counted as the
 * defect it was hunting.
 *
 * So every step is bounded: an entry that does not name an address inside the
 * identity map is not a page table, whatever its present bit says. A diagnostic
 * that can crash the kernel is worse than no diagnostic. */
static const uint64_t *hw_walk_step(uint64_t entry) {
    uint64_t next;

    if ((entry & PTE_PRESENT) == 0u) {
        return 0;
    }
    next = entry & 0x000FFFFFFFFFF000ull;
    if (next == 0u || next + 4096ull > VIBEOS_HW_IDENTITY_LIMIT) {
        return 0;   /* not reachable through the identity map: not a table */
    }
    return (const uint64_t *)(uintptr_t)next;
}

/* out_va: the virtual address the surviving mapper holds it at.
 *
 * "A frame is still mapped" says a reference was lost; *where* says by whom and
 * as what. A CI failure carrying `during cow-fault mappers=1 owners=0` could be
 * a stack page, a program's text, or a page table, and those are three different
 * defects - the address is what separates them, and the run that produced it is
 * gone by the time anybody reads the log. */
static int hw_frame_still_mapped(uint64_t phys, uint32_t *out_pid,
                                 uint32_t *out_mappers, uint64_t *out_va) {
    uint32_t mappers = 0;
    int t;
    /* Address spaces already walked.
     *
     * Threads share one set of page tables across several task slots, so
     * walking per task counted the same entry once per thread - and reported
     * mappers=2 owners=1 for a perfectly healthy two-threaded process. That
     * was read as a lost reference and chased as a defect. Counting mappings
     * means counting address spaces, not tasks. */
    const uint64_t *seen[VIBEOS_HW_MAX_TASKS];
    int nseen = 0;


    for (t = 0; t < (int)VIBEOS_HW_MAX_TASKS; t++) {
        const uint64_t *pml4;
        uint32_t slot;

        if (!hw_task_is_user_of(&g_tasks[t]) || hw_slot_state(t) == HW_TASK_FREE ||
            hw_slot_state(t) == HW_TASK_ZOMBIE) {
            continue;
        }
        pml4 = g_tasks[t].proc.as.pml4;
        /* Read without the scheduler lock, so it may already have been freed.
         * The same bound as every other step: a pointer outside the identity
         * map is not a page table. */
        if (pml4 && ((uint64_t)(uintptr_t)pml4 + 4096ull) > VIBEOS_HW_IDENTITY_LIMIT) {
            continue;
        }
        if (!pml4 || pml4 == g_aspace_being_destroyed) {
            continue;   /* this is the space being torn down; it owns nothing now */
        }
        {
            int k, dup = 0;
            for (k = 0; k < nseen; k++) {
                if (seen[k] == pml4) { dup = 1; break; }
            }
            if (dup) {
                continue;   /* another thread of a process already counted */
            }
            if (nseen < (int)VIBEOS_HW_MAX_TASKS) {
                seen[nseen++] = pml4;
            }
        }
        /* Slot 0 carries the low user window, slot 1 the high one. */
        for (slot = 0; slot < 2u; slot++) {
            const uint64_t *pdpt;
            uint32_t i;

            pdpt = hw_walk_step(pml4[slot]);
            if (!pdpt) {
                continue;
            }
            for (i = 0; i < 512u; i++) {
                const uint64_t *pd;
                uint32_t j;

                pd = hw_walk_step(pdpt[i]);
                if (!pd) {
                    continue;
                }
                for (j = 0; j < 512u; j++) {
                    const uint64_t *pt;
                    uint32_t k;

                    if ((pd[j] & PTE_PS) != 0) {
                        continue;   /* a 2 MiB leaf, not a table */
                    }
                    pt = hw_walk_step(pd[j]);
                    if (!pt) {
                        continue;
                    }
                    for (k = 0; k < 512u; k++) {
                        /* The ownership mark, not PTE_USER.
                         *
                         * PTE_USER asks whether ring 3 can reach the page,
                         * which is a different question and misses exactly the
                         * cases that matter: a PROT_NONE thread guard is owned
                         * and unreachable, so a frame leaked or freed under one
                         * was invisible to this walk. */
                        if ((pt[k] & PTE_PRESENT) == 0 ||
                            (pt[k] & VIBEOS_PTE_OWNED) == 0) {
                            continue;
                        }
                        if ((pt[k] & 0x000FFFFFFFFFF000ull) == phys) {
                            if (out_pid) {
                                *out_pid = hw_task_pid_of(&g_tasks[t]);
                            }
                            if (out_va) {
                                /* Rebuilt from the walk's own indices: slot
                                 * selects the user window, then pdpt/pd/pt. */
                                *out_va = ((uint64_t)slot << 39) |
                                          ((uint64_t)i << 30) |
                                          ((uint64_t)j << 21) |
                                          ((uint64_t)k << 12);
                            }
                            mappers++;
                        }
                    }
                }
            }
        }
    }
    if (out_mappers) {
        *out_mappers = mappers;
    }
    return mappers != 0u;
}

/* ---- the totals kernel/mm/usage.c asks for ---------------------------------
 * Moved from the end of arch_hw.c (2026-09-28), where they sat under a banner
 * about APIC and SMP. */

/* What the portable inspection layer (kernel/mm/usage.c) asks the architecture
 * for. Weak stubs there return zero; these are the real answers, and they are
 * everything today's allocator can honestly report. The rest of the picture -
 * the state histogram, the per-process split - needs the frame table and the
 * address-space layer, and reports zero until those exist rather than being
 * guessed at. */
uint64_t vibeos_mm_bytes_total(void) {
    return vibeos_frame_total() * 4096ull;
}

uint64_t vibeos_mm_bytes_free(void) {
    /* One number, kept by the layer that hands frames out, instead of a bump
     * remainder plus a walk of a free list. The walk was honest and it was also
     * the reason "how much memory is free" could disagree with "how many frames
     * are free" - two answers to one question, which is how this subsystem got
     * its reputation. */
    return vibeos_frame_free_count() * 4096ull;
}

uint64_t vibeos_mm_bytes_reserved(void) {
    /* Two different reservations, and both are real memory a person cannot use.
     *
     * The low user window is taken out before the allocator starts, so nothing
     * of the kernel's lives where a Linux process shadows it. The rest is what
     * the bootstrap bump allocator had already handed out when the frame layer
     * took over - early page tables, the staging buffers, the descriptor table
     * itself. That second part used to be invisible: it was simply missing from
     * every total, which is exactly the kind of gap this command exists to
     * close. */
    uint64_t prefix = 0;

    if (g_frame_layer_ready) {
        prefix = (uint64_t)g_hw_pmm.offset_bytes;
    }
    return (VIBEOS_HW_LOW_USER_LIMIT - VIBEOS_HW_LOW_USER_BASE) + prefix;
}
