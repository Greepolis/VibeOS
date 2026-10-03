#ifndef VIBEOS_KSVC_FAKE_H
#define VIBEOS_KSVC_FAKE_H

/* A kernel for syscall handlers to run on in a host test.
 *
 * The other implementation of include/vibeos/ksvc.h: the architecture's is
 * kernel/arch/x86_64/ksvc.c. Written for the Linux handlers (docs/abi/ A2) and
 * for whichever personality comes next - nothing in it knows Linux except that
 * a syscall is entered through a function the personality names.
 *
 * What it models, and how a test drives it:
 *
 *   - a task table (the portable task layer's, really) with user tasks made by
 *     kf_spawn, each with its own process state and image; kf_set_current picks
 *     which one is "running";
 *   - user memory: a host arena whose addresses are user addresses. A range
 *     outside it is refused by ks_user_ok, and kf_fault makes part of it fault
 *     in vibeos_uaccess_copy - which is a sibling thread's munmap arriving
 *     between the check and the copy;
 *   - anything that would not return - a wait with nobody to wake it, an exit,
 *     a panic - returns to the test instead, through kf_call's result;
 *   - a filesystem of a few files in memory, a network stack whose frames are
 *     captured, and the console captured as text.
 *
 * Every call into a handler goes through kf_call, which sets up the escape. */

#include <stdint.h>
#include "vibeos/ksvc.h"

#define KF_SLOTS 32u
#define KF_USER_BYTES (256u * 1024u)

/* How kf_call ended, beside the handler's own return value. */
typedef enum {
    KF_RETURNED = 0,
    KF_BLOCKED,        /* waited with nothing that could ever wake it */
    KF_EXITED,         /* ks_task_exit / ks_task_exit_group */
    KF_PANICKED
} kf_outcome_t;

/* What a handler saw of its registers: enough for the calls that read or write
 * them (fork, execve, the signal frame). */
struct ks_regs {
    uint64_t ip, sp, ret, arg0, arg1, arg2, flags;
    uint64_t other[10];   /* r8-r15, rbp, rbx, rcx: carried, never looked at */
};

/* The fake's vector registers: what ks_fpu_save writes out and ks_fpu_restore
 * reads back. */
extern unsigned char g_kf_fpu[512];
/* Why a pending signal was raised, as the fake keeps it. */
const vibeos_siginfo_t *kf_siginfo(int slot, uint32_t sig);

typedef long (*kf_entry_t)(struct ks_regs *frame, uint64_t nr, const uint64_t a[6]);

void kf_reset(void);
int kf_spawn(uint32_t pid, uint32_t sid);           /* a user task; its slot   */
void kf_set_current(int slot);

/* User memory. kf_ualloc hands out user addresses in the arena (zeroed). */
uint64_t kf_ualloc(uint64_t len);
void *kf_uptr(uint64_t uaddr);
void kf_fault(uint64_t uaddr, uint64_t len);        /* this range now faults   */

/* The user stack pointer the next call arrives with (then back to 0). */
void kf_next_sp(uint64_t sp);

/* Call `entry` as the current task. `out` receives the outcome. */
long kf_call(kf_entry_t entry, uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2,
             uint64_t a3, uint64_t a4, uint64_t a5, kf_outcome_t *out);

/* The filesystem: files and directories by absolute path. */
void kf_fs_add(const char *path, const void *data, uint32_t len, int is_dir);

/* The network: up, with this address, and a peer whose ARP entry is known.
 * Frames the stack transmits are counted and the last one kept. */
void kf_net_up(uint32_t ip, uint32_t peer_ip);
void kf_net_deliver_udp(uint32_t src_ip, uint16_t sport, uint16_t dport,
                        const void *payload, uint32_t len);
uint32_t kf_net_tx_count(void);
uint32_t kf_net_udp_sent(uint32_t *last_payload_len);

/* What was written to the console, and how many lock/unlock calls did not pair. */
const char *kf_console(void);
/* Read and write memory a handler mapped (mmap, brk) - through the current
 * task's page tables, as a program would reach it. 0, or -1 if the address is
 * not mapped, or not writable for a poke. */
int kf_peek(uint64_t va, void *out, uint64_t n);
int kf_poke(uint64_t va, const void *in, uint64_t n);

/* Type at the console: the bytes wait in the keyboard's queue until read. */
void kf_type(const char *s);
int kf_lock_imbalance(void);
/* Run `fn` once, right after the next release of an address-space lock. */
void kf_on_mm_unlock(void (*fn)(vibeos_procstate_t *ps));
uint64_t kf_exit_code(void);
/* The frame the last call returned with: where it resumes (rt_sigreturn). */
const struct ks_regs *kf_last_frame(void);
const char *kf_panic_reason(void);
uint32_t kf_illegal_transitions(void);

#endif
