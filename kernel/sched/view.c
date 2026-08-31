/* The task table, printed. Phase S-P0 of docs/sched/.
 *
 * This lives outside the architecture layer on purpose, and it is the first
 * piece of the scheduler to do so. The rule for the rest of the rewrite is
 * visible here in miniature: deciding *what to say about a task* is portable,
 * and only *reading the machine's table* is not. arch_hw.c answers
 * vibeos_task_describe and knows nothing about formatting.
 *
 * It matters more than tidiness. Every task defect this project has had was
 * diagnosed by adding a print statement to an 8000-line file, booting, and
 * reading serial output - and three of the fields printed below exist only
 * because somebody had to do exactly that.
 */

#include "vibeos/task_stats.h"
#include "vibeos/arch_x86_64.h"

static void view_hex(uint64_t v) {
    vibeos_x86_64_serial_print_hex(v);
}

static void view_str(const char *s) {
    vibeos_x86_64_serial_puts(s ? s : "-");
}

void vibeos_task_print_table(void) {
    uint32_t slots = vibeos_task_slots();
    uint32_t i;

    if (slots == 0u) {
        vibeos_x86_64_serial_puts("[TASKS] no task table on this build\n");
        return;
    }

    /* One line per task, assembled under one lock.
     *
     * A line built from a dozen serial_puts calls is a dozen critical
     * sections, and this output is read next to three other cores writing
     * their own. That has already produced a diagnostic that read as a
     * contradiction, and a boot gate that reported failures which had not
     * happened. */
    vibeos_x86_64_serial_lock();
    vibeos_x86_64_serial_puts("[TASKS] slot gen state pid tgid ppid cr3 "
                              "flags cr3_set_by ready_by aspace_killed_by exe\n");
    vibeos_x86_64_serial_unlock();

    for (i = 0; i < slots; i++) {
        vibeos_task_desc_t d;

        if (vibeos_task_describe(i, &d) != 0) {
            continue;
        }
        vibeos_x86_64_serial_lock();
        vibeos_x86_64_serial_puts("[TASKS] ");
        view_hex((uint64_t)d.slot);
        vibeos_x86_64_serial_puts(" ");
        view_hex((uint64_t)d.generation);
        vibeos_x86_64_serial_puts(" ");
        view_str(d.state_name);
        vibeos_x86_64_serial_puts(" pid=");
        view_hex((uint64_t)d.pid);
        vibeos_x86_64_serial_puts(" tgid=");
        view_hex((uint64_t)d.tgid);
        vibeos_x86_64_serial_puts(" ppid=");
        view_hex((uint64_t)d.ppid);
        vibeos_x86_64_serial_puts(" cr3=");
        view_hex(d.cr3);
        vibeos_x86_64_serial_puts(d.is_user ? " user" : " kernel");
        vibeos_x86_64_serial_puts(d.is_thread ? " thread" : "");
        vibeos_x86_64_serial_puts(d.on_cpu ? " on-cpu" : "");
        vibeos_x86_64_serial_puts(" cr3_by=");
        view_str(d.cr3_set_by);
        vibeos_x86_64_serial_puts(" ready_by=");
        view_str(d.ready_by);
        vibeos_x86_64_serial_puts(" aspace=");
        view_str(d.aspace_killed_by);
        if (d.exe[0]) {
            vibeos_x86_64_serial_puts(" exe=");
            view_str(d.exe);
        }
        vibeos_x86_64_serial_puts("\n");
        vibeos_x86_64_serial_unlock();
    }
}

void vibeos_task_print_stats(void) {
    const vibeos_task_stats_t *s = vibeos_task_stats();

    vibeos_x86_64_serial_lock();
    vibeos_x86_64_serial_puts("[TASKS] created=0x");
    view_hex(s->created);
    vibeos_x86_64_serial_puts(" exited=0x");
    view_hex(s->exited);
    vibeos_x86_64_serial_puts(" reaped=0x");
    view_hex(s->reaped);
    vibeos_x86_64_serial_puts(" forks=0x");
    view_hex(s->forks);
    vibeos_x86_64_serial_puts(" threads=0x");
    view_hex(s->threads);
    vibeos_x86_64_serial_puts(" execs=0x");
    view_hex(s->execs);
    vibeos_x86_64_serial_puts(" slot_refused=0x");
    view_hex(s->slot_refused);
    vibeos_x86_64_serial_puts("\n");
    vibeos_x86_64_serial_unlock();

    /* The three that must be zero, on their own line and last, so the eye
     * lands on them - and so the boot gate can match one pattern. */
    vibeos_x86_64_serial_lock();
    vibeos_x86_64_serial_puts("[TASKS] MUSTBEZERO use_after_publish=0x");
    view_hex(s->use_after_publish);
    vibeos_x86_64_serial_puts(" tenancy_mismatch=0x");
    view_hex(s->tenancy_mismatch);
    vibeos_x86_64_serial_puts(" cr3_without_owner=0x");
    view_hex(s->cr3_without_owner);
    vibeos_x86_64_serial_puts("\n");
    vibeos_x86_64_serial_unlock();
}
