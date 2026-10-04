/* Starting userland: the supervisor's manifest, init and the first tasks, the
 * scheduler armed, the other cores woken, and the wait until every user task
 * has retired.
 *
 * vibeos_kmain calls vibeos_x86_64_hw_start_userland once it has said BOOT_OK;
 * hardware bring-up (arch_hw.c's vibeos_x86_64_hw_early_init) has finished by
 * then and left the boot information behind for this. The order is the one
 * CLAUDE.md records under "BOOT_OK used to be printed after userland had
 * finished": the machine first, the announcement second, userland third.
 *
 * Lifted out of arch_hw.c whole (2026-09-28), from under a banner that said
 * "APIC + SMP bring-up". Nothing changed in the move. */

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

/* The embedded user program (generated blob).
 *
 * There was a second ELF loader here - kernel/arch/x86_64/elf_load.c, declared
 * just below and called by nothing since it was written. It is deleted rather
 * than repaired: a review found an integer overflow in its program-header
 * check, and repairing dead code buys a fix for a path nobody can reach while
 * leaving the reason it was dangerous - that it exists at all - in place.
 *
 * The loader this kernel actually uses is kernel/core/elf.c, which is
 * host-tested and is what exec goes through. Two loaders is two places that
 * have to be right about ELF, and this project has spent whole phases removing
 * second places.
 */
extern const unsigned char vibeos_user_hello_elf[];
extern const unsigned long vibeos_user_hello_elf_len;
/* The scheduler demo's two tasks (generated blob). */
extern const unsigned char vibeos_user_task_elf[];
extern const unsigned long vibeos_user_task_elf_len;

static void hw_runtime_copy_string(char *dst, uint32_t capacity, const char *src) {
    uint32_t i;
    if (!dst || !src || capacity == 0) {
        return;
    }
    for (i = 0; i + 1u < capacity && src[i] != 0; i++) {
        dst[i] = src[i];
    }
    dst[i] = 0;
}

static void hw_runtime_supervisor_init(void) {
    vibeos_service_manifest_t manifests[3] = {0};
    uint32_t i;
    if (vibeos_service_supervisor_init(&g_runtime_supervisor) != 0) {
        return;
    }
    for (i = 0; i < 3; i++) {
        manifests[i].abi_major = VIBEOS_NATIVE_ABI_MAJOR;
        manifests[i].struct_size = sizeof(manifests[i]);
        manifests[i].service_id = i + 1u;
        manifests[i].restart_policy = VIBEOS_NATIVE_RESTART_ON_FAILURE;
        manifests[i].restart_limit = 3u;
        manifests[i].startup_timeout_ms = 5000u;
        manifests[i].health_timeout_ms = 1000u;
    }
    manifests[0].dependency_mask = 0;
    manifests[1].dependency_mask = 1u;
    manifests[2].dependency_mask = 1u;
    hw_runtime_copy_string(manifests[0].name, sizeof(manifests[0].name), "init");
    hw_runtime_copy_string(manifests[1].name, sizeof(manifests[1].name), "shell");
    hw_runtime_copy_string(manifests[2].name, sizeof(manifests[2].name), "logd");
    hw_runtime_copy_string(manifests[0].image_path, sizeof(manifests[0].image_path), "/sbin/init");
    hw_runtime_copy_string(manifests[1].image_path, sizeof(manifests[1].image_path), "/bin/sh");
    hw_runtime_copy_string(manifests[2].image_path, sizeof(manifests[2].image_path), "/sbin/logd");
    if (vibeos_service_supervisor_load(&g_runtime_supervisor, manifests, 3) == 0 &&
        vibeos_service_supervisor_start_ready(&g_runtime_supervisor) == 0) {
        g_runtime_supervisor_ready = 1;
        vibeos_x86_64_serial_puts("[INIT] native supervisor manifest ready\n");
    }
}

/* Held between hardware bring-up and the moment userland is started, which
 * are now two separate steps with the portable kernel in between. */
const vibeos_boot_info_t *g_saved_boot_info;

static void hw_sched_bringup(const vibeos_boot_info_t *boot_info) {
    const unsigned char *init_elf = vibeos_user_hello_elf;
    uint64_t init_len = vibeos_user_hello_elf_len;
    int hello_id, a_id, b_id, kern_id;
    /* Argument vectors for the first processes. The two scheduler-demo tasks
     * differ only by argv[0], which is how they pick the letter they print. */
    static const char *const init_argv[] = {"init", 0};
    static const char *const task_a_argv[] = {"0", 0};
    static const char *const task_b_argv[] = {"1", 0};

    /* Init program source, most real first: the on-disk filesystem (virtio-blk +
     * FAT), then the bootloader's EFI module, then the built-in copy. */
    if (g_disk_init_len > 0) {
        init_elf = g_disk_init_elf;
        init_len = (uint64_t)g_disk_init_len;
        vibeos_x86_64_serial_puts("[SCHED] init program from on-disk filesystem (INIT.ELF)\n");
    } else if (boot_info && boot_info->initrd_base != 0 && boot_info->initrd_size > 0 &&
               boot_info->initrd_base + boot_info->initrd_size <= 0x100000000ull) {
        init_elf = (const unsigned char *)(uintptr_t)boot_info->initrd_base;
        init_len = boot_info->initrd_size;
        vibeos_x86_64_serial_puts("[SCHED] init program from bootloader EFI module\n");
    } else {
        vibeos_x86_64_serial_puts("[SCHED] init program from built-in image\n");
    }

    /* Bring up the Linux personality so the portable translation model sees
     * every syscall the on-metal front end serves. */

    /* Keyboard is live (IRQ1 unmasked). Seed a test line so the blocking read()
     * path is exercised on the non-interactive CI console; real keystrokes fill
     * the same ring on hardware. */
    vibeos_x86_64_serial_puts("[KBD] keyboard armed (IRQ1); seeding read() self-test input\n");
    (void)vibeos_input_inject("vibeos\n"
                                  "mkdir DOCS\n"
                                  "write DOCS/NOTES.TXT persistent hello\n"
                                  "cat DOCS/NOTES.TXT\n"
                                  "ls DOCS\n"
                                  "write TMP.TXT scratch\b\b\bch\n"  /* backspace editing */
                                  "rm TMP.TXT\n"
                                  "EFI/BOOT/TASK.ELF\n"
                                  "net\n"
                                  "ping 10.0.2.2\n"
                                  "EFI/BOOT/NET.ELF\n"
                                  "EFI/BOOT/MUSL.ELF\n"
                                  "EFI/BOOT/PIE.ELF\n"
                                  "EFI/BOOT/DYN.ELF\n"
                                  "EFI/BOOT/THREADS.ELF\n"
                                  "EFI/BOOT/TFORK.ELF\n"
                                  "EFI/BOOT/SIGNAL.ELF\n"
                                  /* The reclaim load, alone: the commands here
                                   * run one at a time. It holds the machine at
                                   * its low watermark, where every other
                                   * program's allocations may be refused -
                                   * correctly - so started from init beside the
                                   * thread tests it failed them one boot in
                                   * two: pthread_create, a thread's mmap, an
                                   * exec from a thread, all refused. A load
                                   * that starves its neighbours tests them
                                   * rather than reclaim. Not last, either: it
                                   * empties the page cache, and the BusyBox
                                   * commands after it are what give the exec
                                   * cache audit something to compare. */
                                  "EFI/BOOT/SVC_RECL.ELF\n"
                                  "EFI/BOOT/BUSYBOX.ELF echo BUSYBOX_ECHO_OK\n"
                                  "EFI/BOOT/BUSYBOX.ELF cat DOCS/NOTES.TXT\n"
                                  "EFI/BOOT/BUSYBOX.ELF ls EFI/BOOT\n"
                                  "EFI/BOOT/BUSYBOX.ELF sh -c \"echo BUSYBOX_SH_OK; cat DOCS/NOTES.TXT\"\n"
                                  /* Everything from here is typed at BusyBox's
                                   * shell, not ours: it replaces this process
                                   * and reads the rest of the console itself. */
                                  "sh\n"
                                  "echo ASH_INTERACTIVE_OK\n"
                                  "cat DOCS/NOTES.TXT\n"
                                  "ls /EFI/BOOT\n"
                                  /* A working directory (docs/abi/ A4): cd,
                                   * the kernel's answer to getcwd - pwd -P,
                                   * because plain pwd answers from $PWD
                                   * without asking, and an external pwd cannot
                                   * be named: BusyBox dispatches on argv[0],
                                   * which would be BUSYBOX.ELF - and a relative
                                   * cat, which only finds the file if the
                                   * kernel resolved it from where cd went.
                                   * CD_OK is printed only if all three worked. */
                                  "cd -P /DOCS && pwd -P && cat NOTES.TXT && echo CD_OK\n"
                                  "cd /\n"
                                  /* /tmp is tmpfs (docs/abi/ L1): a file
                                   * written there and read back. The contents
                                   * are the shell's arithmetic, so the gate
                                   * cannot mistake the echoed command for
                                   * the file. */
                                  "echo TMPFS_RT_$((1+1)) > /tmp/rt.txt && cat /tmp/rt.txt\n"
                                  /* The write path (L1 step 3): twenty lines of
                                   * forty-one bytes - past the 512 a descriptor
                                   * used to hold - then five more appended with
                                   * >>, and the count of what the file holds.
                                   * 825, which the command does not contain. */
                                  "for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do "
                                  "echo 0123456789012345678901234567890123456789; done > /tmp/big.txt; "
                                  "echo tail >> /tmp/big.txt; wc -c < /tmp/big.txt\n"
                                  /* And on the boot volume, which is FAT
                                   * (step 4): twenty-one lines this time, so
                                   * the count - 866 - is not /tmp's; then a
                                   * file whose name is not 8.3, written and
                                   * read back by that name. The gate also
                                   * reads it off the disk image with mtools
                                   * once the machine has stopped. */
                                  "for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21; do "
                                  "echo 0123456789012345678901234567890123456789; done > /fatbig.txt; "
                                  "echo tail >> /fatbig.txt; wc -c < /fatbig.txt\n"
                                  "echo LFN_$((2+3)) > '/written with a long name.txt' && "
                                  "cat '/written with a long name.txt'\n"
                                  /* A file of two clusters, opened again
                                   * with O_TRUNC. Nothing here reads the
                                   * result: a truncate that keeps the chain
                                   * leaves a file that reads perfectly, and
                                   * only the gate's consistency check of the
                                   * volume can see the clusters it kept. */
                                  "cat /fatbig.txt /fatbig.txt /fatbig.txt > /fatcut.txt; "
                                  "echo cut > /fatcut.txt\n"
                                  /* Names and metadata (L1 step 5), as the
                                   * programs that use them call them: mv is
                                   * rename, ln is link and symlink, chmod is
                                   * fchmodat, and stat prints what the kernel
                                   * told it - mode 600, two links, two bytes -
                                   * only if every step before it worked. */
                                  "cd /tmp && echo x > mv1 && mv mv1 mv2 && ln -s mv2 lnk && "
                                  "ln mv2 hard && chmod 600 mv2 && mkdir dd && rmdir dd && "
                                  "cat lnk > mv3 && stat -c META_%a_%h_%s hard; cd /\n"
                                  /* And on FAT, where the gate's check of
                                   * the volume afterwards is what says a
                                   * rename and an rmdir left it whole. */
                                  "mv /fatcut.txt '/moved by rename.txt' && mkdir /fatdir && "
                                  "rmdir /fatdir && echo FATMV_$(wc -c < '/moved by rename.txt')\n"
                                  /* A directory listed (L1 step 6): a name
                                   * longer than the fifteen bytes getdents64
                                   * used to cut it at, the two dots every
                                   * directory has, and a link that find
                                   * takes for one from the entry's type. */
                                  "mkdir /tmp/ls && cd /tmp/ls && echo > 'a file name longer than fifteen bytes' && "
                                  "ln -s nowhere dangling && ls && echo DIRS_$(ls -a | wc -l)_$(find . -type l | wc -l); cd /\n"
                                  /* The corpus's file workloads (L1 step
                                   * 8): BusyBox's applets over a small tree,
                                   * SQLite on a file, a Lua script. The
                                   * script is staged by the boot gate, which
                                   * compares what it prints here with what
                                   * the same script printed on Linux. On a
                                   * volume without it the shell says so and
                                   * carries on. */
                                  "sh /corpus/run.sh\n"
                                  /* The last thing the self-test says. The boot
                                   * harness waits for this before driving the
                                   * kernel CLI, so a slower build cannot have
                                   * its script cut short by a halt that arrived
                                   * while it was still working. */
                                  "ls /EFI/BOOT | wc -l\n"
                                  "echo PIPE_OK\n"
                                  "echo VIBEOS_SELFTEST_DONE\n"
                                  "exit\n");

    hello_id = hw_task_spawn_user(init_elf, init_len, init_argv);
    a_id = hw_task_spawn_user(vibeos_user_task_elf, vibeos_user_task_elf_len,
                              task_a_argv);
    b_id = hw_task_spawn_user(vibeos_user_task_elf, vibeos_user_task_elf_len,
                              task_b_argv);
    if (hello_id < 0 || a_id < 0 || b_id < 0) {
        vibeos_x86_64_serial_puts("[SCHED] failed to spawn initial tasks\n");
        return;
    }
    /* The program started first adopts every process whose parent ends
     * (hw_orphans_to_init), as Linux's init does. */
    g_init_pid = hw_task_pid_of(&g_tasks[hello_id]);
    hw_runtime_supervisor_init();
    if (g_runtime_supervisor_ready) {
        hw_task_set_service(hello_id, 1u);
        (void)vibeos_service_supervisor_bind_pid(&g_runtime_supervisor, 1u,
                                                 hw_task_pid_of(&g_tasks[hello_id]));
    }
    g_console_foreground_pgid = hw_task_pgid_of(&g_tasks[hello_id]);

    /* Printed before the scheduler is armed, so this line cannot be split by a
     * preemption. */
    vibeos_x86_64_serial_puts("[SCHED] scheduler live: kernel task + 3 user tasks, own address spaces\n");

    __asm__ __volatile__("cli");
    kern_id = hw_task_adopt_kernel();
    if (kern_id < 0) {
        __asm__ __volatile__("sti");
        vibeos_x86_64_serial_puts("[SCHED] failed to adopt kernel task\n");
        return;
    }
    (void)hw_task_create_idle(&g_cpus[0]);
    /* Before the first switch: a scheduler that starts accounting after it
     * starts scheduling reports a machine that idled through its own boot. */
    /* Sized for every core this kernel can have, not the ones online right
     * now: the application processors come up after this point, and sizing it
     * to the current count silently refused three quarters of the ticks while
     * balancing perfectly. */
    (void)vibeos_account_init((uint32_t)VIBEOS_HW_MAX_TASKS, VIBEOS_HW_MAX_CPUS);
    /* Four slots held back, and eight children to any one task.
     *
     * Four is one per core, which is the smallest reserve that lets every core
     * still start something. Eight is comfortably more than anything in this
     * boot forks - the shell's deepest pipeline is three - so the limit binds
     * on a bomb and on nothing else. Both are numbers chosen rather than
     * discovered, and if a workload ever needs more the place to argue about it
     * is here. */
    (void)vibeos_forkguard_init((uint32_t)VIBEOS_HW_MAX_TASKS, 4u, 8u);

    /* The user-access recovery, exercised on every boot (H-003, H-010).
     *
     * A copy from a user address that nothing maps must come back as an error,
     * not stop the machine - that is the whole contract, and the last attempt
     * at it measured green for three boots while never once being exercised,
     * because its only caller always had the page. So it is forced here: the
     * top of the high user window, from the kernel's own address space, before
     * any task runs. */
    {
        uint8_t probe[8];
        int r = vibeos_uaccess_copy(probe,
                                    (const void *)(uintptr_t)(VIBEOS_HW_USER_BASE + 0x7F00000000ull),
                                    sizeof(probe));
        vibeos_x86_64_serial_puts(r != 0
            ? "[HW] uaccess recovery ok: an unmapped user read returned an error\n"
            : "[HW] uaccess recovery WRONG: an unmapped user read succeeded\n");
    }
    g_sched_running = 1;
    __asm__ __volatile__("sti");

    /* With a run queue in place, wake the other cores. */
    hw_smp_bringup();

    /* Wait for the spawned tasks to finish before the kernel task goes on to
     * the console (init-style child reaping). The kernel task is preempted
     * while it waits, so the user tasks make progress; hlt idles until the next
     * timer tick instead of spinning.
     *
     * BLOCKED counts as alive, and leaving it out was a defect with a long
     * reach. This asked only for READY or RUNNING, so a moment when every user
     * task happened to be waiting - in waitpid after a fork, on a pipe, on a
     * futex - read as "nothing is running, we are done". The machine then
     * printed "all user tasks retired" with tasks very much not retired, went
     * on to the console, and kmain sampled the frame accounting whose comment
     * states outright that every user process has exited by this point.
     *
     * It hid because the ordinary boot's programs are short: the window has to
     * open while something is still blocked, and at 120 stress rounds the run
     * is usually over first. Raising that to 12000 for the plan's soak made it
     * fire on nearly every boot - the log shows fork after fork retiring after
     * the CLI is already up - and the frame numbers reported from mid-flight
     * looked like a leak that grew with the workload. It was not a leak; it
     * was a measurement taken while the thing being measured was still running.
     *
     * ZOMBIE is deliberately not alive: it holds a slot, not an address space,
     * and something has to reap it - which is what this loop's caller goes on
     * to do. */
    for (;;) {
        int i;
        int alive = 0;
        for (i = 0; i < VIBEOS_HW_MAX_TASKS; i++) {
            if (hw_task_is_user_of(&g_tasks[i]) &&
                (hw_slot_state(i) == HW_TASK_READY ||
                 hw_slot_state(i) == HW_TASK_RUNNING ||
                 hw_slot_state(i) == HW_TASK_BLOCKED)) {
                alive = 1;
            }
        }
        if (!alive) {
            break;
        }
        /* The memory clobber forces the task states to be re-read after each
         * idle period: they are updated by interrupt/syscall context. */
        __asm__ __volatile__("hlt" ::: "memory");
    }
    vibeos_x86_64_serial_puts("[SCHED] all user tasks retired; kernel task continues\n");
    /* Every user slot that is not free, and its state, in one line (M-068). A
     * boot announced this while svc-reclaim was still running; the wait counts
     * READY, RUNNING and BLOCKED as alive, so whatever that task was, it was
     * something else for the instant the wait looked. */
    {
        int i;

        vibeos_x86_64_serial_lock();
        vibeos_x86_64_serial_puts("[SCHED] RETIRED_SLOTS");
        for (i = 0; i < VIBEOS_HW_MAX_TASKS; i++) {
            if (hw_task_is_user_of(&g_tasks[i]) && hw_slot_state(i) != HW_TASK_FREE) {
                vibeos_x86_64_serial_puts(" slot=0x");
                vibeos_x86_64_serial_print_hex((uint64_t)i);
                vibeos_x86_64_serial_puts(":0x");
                vibeos_x86_64_serial_print_hex((uint64_t)hw_slot_state(i));
            }
        }
        vibeos_x86_64_serial_puts("\n");
        vibeos_x86_64_serial_unlock();
    }

    {
        vibeos_x86_64_serial_puts("[MM] COW_STATS exclusive_lost=0x");
        vibeos_x86_64_serial_print_hex(vibeos_mm_stats()->cow_exclusive_lost);
        vibeos_x86_64_serial_puts(" shared=0x");
        vibeos_x86_64_serial_print_hex(g_cow_shared);
        vibeos_x86_64_serial_puts(" copied=0x");
        vibeos_x86_64_serial_print_hex(g_cow_copied);
        vibeos_x86_64_serial_puts(" tlb_shootdowns=0x");
        vibeos_x86_64_serial_print_hex(g_tlb_shootdowns);
        vibeos_x86_64_serial_puts(" tlb_acks=0x");
        vibeos_x86_64_serial_print_hex(g_tlb_acks);
        vibeos_x86_64_serial_puts(" tlb_targets=0x");
        vibeos_x86_64_serial_print_hex(vibeos_mm_stats()->tlb_targets);
        vibeos_x86_64_serial_puts(" tlb_flushed=0x");
        vibeos_x86_64_serial_print_hex(vibeos_mm_stats()->tlb_flushed);
        vibeos_x86_64_serial_puts(" bad_unlocks=0x");
        vibeos_x86_64_serial_print_hex(vibeos_x86_64_serial_bad_unlocks());
        vibeos_x86_64_serial_puts("\n");
        vibeos_x86_64_serial_puts("[ABI] abi=");
        vibeos_x86_64_serial_puts(vibeos_abi_linux()->name);
        vibeos_x86_64_serial_puts(" vocabulary=0x");
        vibeos_x86_64_serial_print_hex((uint64_t)VIBEOS_OP_COUNT - 1u);
        /* The hot paths. One critical section for the whole line: it is built
         * from ten calls that each take the console lock on their own, and a
         * line assembled from ten of those is ten critical sections. */
        /* Does anything else own a frame the desktop is rendering into?
         *
         * This is the hypothesis the case file actually states - that the
         * frames are handed out twice - and it is the one neither the canary
         * nor the poison can see, because a double hand-out frees nothing and
         * corrupts no bookkeeping. It is asked directly: for every frame in the
         * back buffer, how many page-table entries point at it? The desktop
         * does not map its buffer into any address space, so the honest answer
         * is zero for all of them, and anything else names a process sharing
         * memory with the screen.
         *
         * Outside the frame layer's lock, deliberately: vibeos_frame_owners
         * takes it, and a diagnostic that calls a public accessor from inside
         * the lock it needs is how a previous investigation deadlocked the
         * machine it was explaining, mid-line, at "owners=0x". */
        if (g_gui_back_base != 0ull) {
            uint64_t p;
            for (p = g_gui_back_base; p < g_gui_back_end; p += 4096ull) {
                /* Greater than one, not non-zero.
                 *
                 * The first version of this asked for owners != 0 and reported
                 * 1000 of 1000 frames shared on every boot, which is the same
                 * shape as the 3019 use-after-frees that cost a phase: a
                 * detector that fires on everything is reporting its own
                 * baseline. frame_take sets owners to 1 when it hands a frame
                 * out, so one owner *is* the allocated state. A second owner is
                 * a page-table entry somebody else installed. */
                if (vibeos_frame_owners(p) > 1u) {
                    g_gui_back_shared++;
                }
                /* And the other half of the same question. Owners counts page
                 * tables pointing at the frame, which is the process case; a
                 * frame the layer believes is FREE is the kernel case, and it
                 * is the one that lets the *next* allocation - a block buffer,
                 * an argv page - be handed the screen. Neither the poison nor
                 * the canary can see it: nothing was freed early and nothing
                 * overran, the layer simply lost track. */
                if (vibeos_frame_state(p) != VIBEOS_FRAME_ALLOCATED) {
                    g_gui_back_lost++;
                }
            }
        }
        /* guard_broken used to lead this line; it is the GUI's own report now.
         * Which left backbuf_shared first after the word, printed as a
         * must-be-zero for a phase and asserted by nobody. */
        vibeos_x86_64_serial_puts("\n[GUI] MUSTBEZERO backbuf_shared=0x");
        vibeos_x86_64_serial_print_hex(g_gui_back_shared);
        vibeos_x86_64_serial_puts(" MUSTBEZERO backbuf_lost=0x");
        vibeos_x86_64_serial_print_hex(g_gui_back_lost);
        vibeos_x86_64_serial_puts(" ring3_write_nul=0x");
        vibeos_x86_64_serial_print_hex(g_ring3_write_nul);
        vibeos_x86_64_serial_puts(" cow_copy_changed=0x");
        vibeos_x86_64_serial_print_hex(g_cow_copy_changed);
        vibeos_x86_64_serial_puts(" cow_resolved=0x");
        vibeos_x86_64_serial_print_hex(g_cow_resolved);
        vibeos_x86_64_serial_puts("\n[NET] MUSTBEZERO sock_stale_parent=0x");
        vibeos_x86_64_serial_print_hex(g_net.sock_stale_parent);
        vibeos_x86_64_serial_puts(" sock_fd_aba=0x");
        vibeos_x86_64_serial_print_hex(g_net.sock_fd_aba);
        /* The ABI surface, which had no counter. (The mouse's line, which sat
         * here, is the driver's own report now - see kernel/io/device.c.) */
        vibeos_x86_64_serial_puts("\n[ABI] MUSTBEZERO unexpected_unimplemented=0x");
        vibeos_x86_64_serial_print_hex(g_abi_unimplemented - g_abi_probes);
        vibeos_x86_64_serial_puts(" probes=0x");
        vibeos_x86_64_serial_print_hex(g_abi_probes);
        vibeos_x86_64_serial_puts(" last_nr=0x");
        vibeos_x86_64_serial_print_hex(g_abi_last_nr);
        /* Refused is expected and proved non-zero by hello's iopl; deferred is
         * asserted zero with its number, like the unimplemented count. */
        vibeos_x86_64_serial_puts(" refused=0x");
        vibeos_x86_64_serial_print_hex(g_abi_refused);
        vibeos_x86_64_serial_puts(" deferred=0x");
        vibeos_x86_64_serial_print_hex(g_abi_deferred);
        vibeos_x86_64_serial_puts(" deferred_nr=0x");
        vibeos_x86_64_serial_print_hex(g_abi_deferred_nr);
        /* The device registry (C7): how many drivers the linker collected, and
         * whether an input interrupt ever woke a reader. Both asserted: an empty
         * table used to show up only as a wedge, and the wake as nothing. */
        vibeos_x86_64_serial_puts("\n[DEV] registered=0x");
        vibeos_x86_64_serial_print_hex((uint64_t)vibeos_device_count());
        vibeos_x86_64_serial_puts(" input_irq_wakes=0x");
        vibeos_x86_64_serial_print_hex(g_input_irq_wakes);
        vibeos_x86_64_serial_puts(" MUSTBEZERO stray_vectors=0x");
        vibeos_x86_64_serial_print_hex(g_device_stray_irqs);
        /* The registry the parsers, the journal, the log sink and the scheduler
         * report through (kernel/core/mbz.c): total, and which one and what it
         * saw first. One line so the witness cannot be separated from the count. */
        vibeos_x86_64_serial_puts("\n[MBZ] MUSTBEZERO total=0x");
        vibeos_x86_64_serial_print_hex(vibeos_mbz_total());
        vibeos_x86_64_serial_puts(" first=");
        vibeos_x86_64_serial_puts(vibeos_mbz_first() == VIBEOS_MBZ_COUNT
                                      ? "none" : vibeos_mbz_name(vibeos_mbz_first()));
        vibeos_x86_64_serial_puts(" witness=0x");
        vibeos_x86_64_serial_print_hex(vibeos_mbz_first() == VIBEOS_MBZ_COUNT
                                           ? 0u : vibeos_mbz_witness(vibeos_mbz_first()));
        /* The log's sinks: how many lines each was given, how many it refused
         * (klog_line_lost in the registry above) and how many were raised from
         * inside its own write. The gate reads serial_lines and checks every
         * ln= from one to it is in the log - which is how a device that dropped
         * a line and said it had not is seen from outside it. */
        vibeos_x86_64_serial_puts("\n[KLOG] sinks=0x");
        vibeos_x86_64_serial_print_hex(vibeos_klog_sink_count());
        {
            uint32_t k;
            for (k = 0; k < vibeos_klog_sink_count(); k++) {
                vibeos_klog_sink_stats_t ks;
                if (vibeos_klog_sink_stats(k, &ks) != 0) {
                    continue;
                }
                vibeos_x86_64_serial_puts(" ");
                vibeos_x86_64_serial_puts(ks.name);
                vibeos_x86_64_serial_puts("_lines=0x");
                vibeos_x86_64_serial_print_hex(ks.offered);
                vibeos_x86_64_serial_puts(" ");
                vibeos_x86_64_serial_puts(ks.name);
                vibeos_x86_64_serial_puts("_lost=0x");
                vibeos_x86_64_serial_print_hex(ks.lost);
                vibeos_x86_64_serial_puts(" ");
                vibeos_x86_64_serial_puts(ks.name);
                vibeos_x86_64_serial_puts("_reentered=0x");
                vibeos_x86_64_serial_print_hex(ks.reentered);
            }
        }
        vibeos_x86_64_serial_puts("\n[PERF] syscalls=0x");
        vibeos_x86_64_serial_print_hex(g_perf_syscall.count);
        vibeos_x86_64_serial_puts(" syscall_cycles=0x");
        vibeos_x86_64_serial_print_hex(g_perf_syscall.cycles);
        vibeos_x86_64_serial_puts(" syscall_min=0x");
        vibeos_x86_64_serial_print_hex(g_perf_syscall.min);
        vibeos_x86_64_serial_puts(" faults=0x");
        vibeos_x86_64_serial_print_hex(g_perf_fault.count);
        vibeos_x86_64_serial_puts(" fault_cycles=0x");
        vibeos_x86_64_serial_print_hex(g_perf_fault.cycles);
        vibeos_x86_64_serial_puts(" fault_min=0x");
        vibeos_x86_64_serial_print_hex(g_perf_fault.min);
        vibeos_x86_64_serial_puts(" switches=0x");
        vibeos_x86_64_serial_print_hex(g_perf_switch.count);
        vibeos_x86_64_serial_puts("\n");
    }
}

/* Start init and everything under it. Called from vibeos_kmain, after the
 * portable subsystems are up and the kernel has said so. Does not return until
 * every user task has retired. */
void vibeos_x86_64_hw_start_userland(void) {
    hw_sched_bringup(g_saved_boot_info);
    hw_tlbq_selftest();
}
