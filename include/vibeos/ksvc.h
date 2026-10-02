#ifndef VIBEOS_KSVC_H
#define VIBEOS_KSVC_H

/* Kernel services: what a syscall personality may ask of the kernel under it.
 *
 * Written for the Linux handlers (docs/abi/phases.md, A2), and deliberately not
 * named after them: a second personality - Windows is planned - is another
 * directory beside kernel/abi/linux that includes this header, and the kernel
 * underneath does not change. Nothing here is a Linux type or a Linux number
 * except the errno a handler returns, which is the personality's business.
 *
 * The Linux handlers used to include the architecture's private header and reach
 * into its task table, its locks and its trap frame directly - about seven
 * hundred references, and the reason none of them could be tested anywhere but
 * in a booted guest. This is the whole of what they need, declared once:
 *
 *   - the current task, and any task by slot: who it is (vibeos_task_t), its
 *     process (vibeos_procstate_t), its image, its ABI;
 *   - the scheduler's lock, task states, lookups by id, allocation and release;
 *   - waiting: parking until the next tick, whether a signal must end a wait;
 *   - user memory: the range checks, strings, the fault-safe copy;
 *   - the address space: the per-process mutation lock, mapping a page, the
 *     unmap quarantine, describing a page;
 *   - the network stack, the console, the log (the filesystems are reached
 *     through the mount table, vibeos/path.h, which is portable);
 *   - and the few operations whose substance is the machine's registers - a
 *     new task's saved state, a program's entry, a handler frame, the thread
 *     pointer. Linux keeps the same things in arch/: copy_thread,
 *     start_thread, setup_rt_frame, arch_prctl - and Windows needs the same
 *     four for a thread, an image, an APC and the TEB.
 *
 * The architecture implements it (kernel/arch/x86_64/ksvc.c); the host tests
 * implement it again (tests/kernel/ksvc_fake.c), which is how the handlers run
 * in a test binary. A handler that needs something not listed here
 * adds it here - which is the point: the list is the cost of the seam, and it
 * is written down.
 *
 * Every `slot` is an index in the task table, as `ks_current()` returns it. */

#include <stdint.h>
#include "vibeos/procstate.h"
#include "vibeos/task_ident.h"
#include "vibeos/task.h"
#include "vibeos/abi.h"
#include "vibeos/log.h"
#include "vibeos/vfs.h"
#include "vibeos/inet.h"
#include "vibeos/vmspace.h"
#include "vibeos/pageinfo.h"
#include "vibeos/exec_stats.h"

/* A saved user register state: the trap frame a syscall arrived with. Opaque
 * here; only the functions below that name it read or write it. */
typedef struct ks_regs ks_regs_t;

/* ---- tasks ------------------------------------------------------------------ */

int ks_current(void);                          /* slot, or negative for none   */
uint32_t ks_slots(void);                       /* how many slots there are     */
vibeos_task_t *ks_id(int slot);
vibeos_procstate_t *ks_ps(int slot);           /* 0 once a task has let go     */
void ks_set_ps(int slot, vibeos_procstate_t *ps);
vibeos_image_t *ks_image(int slot);
uint32_t ks_seq(int slot);                     /* which tenancy of the slot    */
const vibeos_abi_t *ks_abi(int slot);
uint64_t ks_cr3(int slot);                     /* diagnostics only             */
/* Where a task was last made runnable, or where its address space was kept
 * rather than destroyed: static strings, read by the context-switch guard. */
void ks_mark_ready(int slot, const char *where);
void ks_mark_aspace(int slot, const char *where);

vibeos_lock_t *ks_sched_lock(void);
int ks_set_state(int slot, vibeos_task_state_t to, const char *why);
int ks_task_by_pid(uint32_t pid);              /* under ks_sched_lock          */
int ks_task_by_tid(uint32_t tid);
int ks_task_alloc_for_user(const char *what);  /* a RESERVED slot, or negative */
void ks_task_release(int slot);
uint32_t ks_next_pid(void);
void ks_task_exit(uint64_t code);              /* does not return              */
void ks_task_exit_group(uint64_t code);        /* does not return              */

/* ---- locks and waiting --------------------------------------------------------
 *
 * ks_lock masks interrupts until the matching unlock; the preemptible form
 * leaves them as they were, for a critical section that can be long. `fn` names
 * the holder in a deadlock report. */
void ks_lock(vibeos_lock_t *l, const char *fn);
void ks_unlock(vibeos_lock_t *l);
void ks_lock_preemptible(vibeos_lock_t *l);
void ks_unlock_preemptible(vibeos_lock_t *l);
void ks_irq_off(void);
void ks_irq_on(void);
void ks_idle(void);                    /* interrupts on, sleep until the next one  */
void ks_block_point(void);
/* Give the core away until the clock has moved: what a wait with a deadline
 * does between looks at the time. Always returns, a tick later at most. */
void ks_wait_tick(void);             /* a task giving up the core to wait: idle  */
void ks_wake_waiters(void);            /* data, room or an end of file appeared    */
int ks_signal_interrupts(int slot);    /* must a wait end, for a signal?           */
int ks_signal_raise(int slot, uint32_t sig);
int ks_signal_default_kills(uint32_t sig);

/* ---- time ------------------------------------------------------------------- */

uint64_t ks_ticks(void);
uint32_t ks_hz(void);

/* ---- user memory ---------------------------------------------------------------
 *
 * ks_user_ok is the one place a user range is judged; see linux_user_ok. The
 * copy is fault-safe in both directions: 0, or -1 if the user side faulted - which
 * a sibling thread's munmap can cause at any moment after the range was checked. */
int ks_user_ok(uint64_t base, uint64_t len, int write);
int ks_user_addr_ok(uint64_t va);
int ks_user_range_why(uint64_t va, uint64_t len, int write, uint32_t *why);
const char *ks_user_range_why_name(uint32_t why);
int ks_copy_user_string(uint64_t uptr, char *dst, int max);
int vibeos_uaccess_copy(void *dst, const void *src, uint64_t len);

/* ---- the address space ----------------------------------------------------------
 *
 * ks_mm_lock is the process's one-mutation-at-a-time lock (see its definition).
 * ks_map_anon maps one fresh zeroed page at `va` with `prot`; VIBEOS_PROT_NONE is
 * a guard - allocated, owned, and reachable from ring 3 by nobody. */
void ks_mm_lock(vibeos_procstate_t *ps);
void ks_mm_unlock(vibeos_procstate_t *ps);
vibeos_vmspace_t ks_vm(int slot);
int ks_map_anon(int slot, uint64_t va, vibeos_prot_t prot);
/* Map a page that already exists - one a filesystem handed out with
 * share_page - at `va`. The mapping takes its own reference; the caller still
 * has the one the page came with, and gives it back with ks_page_unhold. */
int ks_map_page(int slot, uint64_t va, void *page, vibeos_prot_t prot);
void ks_page_unhold(void *page);
int ks_map_user_pages(int slot, uint64_t va, uint64_t pages);
/* May a program place a mapping of `len` bytes at `base`, an address it chose?
 * The architecture's policy on where user memory may be: a personality asks,
 * for a mapping at a fixed address, and never decides. */
int ks_user_fixed_ok(uint64_t base, uint64_t len);
void ks_tlb_drain(void);
void ks_tlb_flush_page(uint64_t va);
void ks_pageinfo(int slot, uint64_t va, vibeos_pageinfo_t *out);
void *ks_page_alloc(void);
void ks_page_free(void *page, const char *why);
/* The layout's fixed points, which brk and prlimit report. */
uint64_t ks_heap_base(void);
uint64_t ks_mmap_base(void);
uint64_t ks_stack_bytes(void);

/* ---- processes ------------------------------------------------------------------
 *
 * A fork builds the child in four steps the handler orders: a copy of the address
 * space (under the parent's mm lock), a kernel stack, a process state, and the
 * registers. A thread shares the address space and takes only the last two. */
vibeos_procstate_t *ks_procstate_new(void);
void ks_procstate_put(vibeos_procstate_t *ps);
int ks_fork_aspace(int child, int parent);        /* 0, or -1 with nothing kept */
void ks_drop_aspace(int slot);
int ks_alloc_kstack(int slot);                    /* 0, or -1                   */
void ks_fork_regs(int child, int parent, const ks_regs_t *frame);
void ks_thread_regs(int child, int parent, const ks_regs_t *frame,
                    uint64_t stack, uint64_t tls);

/* execve's halves: the staging buffer and its reader, the loader, and the
 * switch to the new image once it is committed. */
uint8_t *ks_exec_buffer(uint32_t *cap);
long ks_read_file_cached(const char *path, void *buf, uint32_t cap, uint32_t *out_id);
int ks_exec_refuse(vibeos_exec_fail_t why, const char *path, const char *detail);
int ks_image_create(vibeos_image_t *img, vibeos_procstate_t *ps,
                    const unsigned char *elf, uint64_t len, uint64_t staged,
                    const char *const *argv, const char *const *envp,
                    const char *path, uint32_t file_id);
void ks_image_drop(vibeos_image_t *img, const char *why);
/* Make `img` the task's image and load it; the old address space is destroyed
 * unless another task still runs in it. */
void ks_exec_switch(int slot, const vibeos_image_t *img);
void ks_exec_regs(int slot, ks_regs_t *frame, uint64_t entry, uint64_t sp);

/* ---- registers ------------------------------------------------------------------ */

uint64_t ks_regs_sp(const ks_regs_t *frame);
uint64_t ks_regs_ret(const ks_regs_t *frame);
/* Make the frame one that issues syscall `nr` again when it is resumed: the
 * number back where the instruction reads it, and the instruction pointer back
 * on the instruction. The arguments are still in their registers - a handler
 * does not write them. */
void ks_regs_restart(ks_regs_t *frame, uint64_t nr);
/* Signal frames: pushed at `sp` holding the whole register state and the mask
 * to restore, and read back by rt_sigreturn. Both 0, or -1 (and nothing
 * changed) if the user side is unusable. */
uint64_t ks_sigframe_size(void);
int ks_sigframe_push(const ks_regs_t *frame, uint64_t sp, uint64_t blocked,
                     uint64_t restorer);
int ks_sigframe_pop(ks_regs_t *frame, uint64_t base, uint64_t *blocked);
void ks_regs_enter_handler(ks_regs_t *frame, uint64_t handler, uint64_t sp,
                           uint32_t sig);
uint64_t ks_tls_get(int slot);
void ks_tls_set(int slot, uint64_t base);

/* ---- devices ----------------------------------------------------------------------- */

vibeos_inet_t *ks_net(void);          /* 0 while the network is down              */
vibeos_lock_t *ks_net_lock(void);
int ks_console_getc(void);            /* next console byte, or -1                 */
void ks_console_echo(char c);         /* one byte to the screen                   */
uint32_t ks_foreground_pgid(void);
void ks_set_foreground_pgid(uint32_t pgid);

/* The serial console. A multi-part line is bracketed by ks_con_lock/unlock, or
 * another core writes into the middle of it. */
void ks_con_lock(void);
void ks_con_unlock(void);
void ks_con_puts(const char *s);
void ks_con_putc(char c);
void ks_con_hex(uint64_t v);
void ks_log(vibeos_log_level_t level, uint32_t code, uint64_t a0, uint64_t a1,
            const char *msg);
void ks_panic(const char *why);
uint32_t ks_cpu_id(void);
uint64_t ks_cr3_now(void);
/* Every recorded copy-on-write fault on `page`, appended to the line the caller
 * is writing under ks_con_lock. */
void ks_con_cow_faults(uint64_t page);

#endif
