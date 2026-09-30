#ifndef VIBEOS_PROCSTATE_H
#define VIBEOS_PROCSTATE_H

/* What a process is, as distinct from the machine that runs it.
 *
 * These three structures lived in kernel/arch/x86_64/arch_hw_internal.h, and that
 * was the reason the Linux syscall handlers had to include the architecture's
 * private header: a handler that reads a signal disposition or claims a
 * descriptor holds one of these. None of them is x86-64. A lock is a word and an
 * owner; a process's state is its program break, its regions, its dispositions
 * and its descriptor table; an image is where a program was loaded and where it
 * starts. What *is* x86-64 - taking the lock with interrupts masked, walking the
 * tables `root` names - stays behind the functions that act on them
 * (kernel/abi/linux/linux_kernel.h, docs/abi/ phase A2).
 *
 * The architecture keeps its old names for them (hw_lock_t, hw_procstate_t,
 * hw_proc_t) as typedefs, so the move changed no line of arch code. */

#include <stdint.h>
#include "vibeos/fdtable.h"
#include "vibeos/vma.h"
#include "vibeos/path.h"

/* Signals 1..64; index 0 is unused so the numbering matches Linux. */
#define VIBEOS_NSIG 65

typedef struct vibeos_lock {
    volatile int locked;
    uint64_t flags;   /* caller's RFLAGS, restored on release */
    /* Who holds it, for when nobody lets go.
     *
     * A spin loop that never gives up turns a deadlock into silence, and
     * silence is the most expensive failure this project has: the machine
     * stops, the log stops, and the evidence is a wedge report naming whatever
     * static function happened to precede the address. Two of those cost days.
     *
     * These are written after the lock is taken and cleared before it is
     * released, so the bound below can say which lock, whose it is, and what
     * they were doing - which turns a wedge into a panic with a name. */
    volatile int owner_cpu;
    const char *volatile owner_fn;
} vibeos_lock_t;

/* The root of an address space's page tables. Opaque above the architecture:
 * portable code passes it to the functions that walk it and never reads it. */
typedef struct vibeos_hw_aspace {
    uint64_t *pml4;
} vibeos_hw_aspace_t;

/* A loaded program: where it lives and where it starts. */
typedef struct vibeos_image {
    vibeos_hw_aspace_t as;
    uint64_t entry;
    /* Where the interpreter was mapped, or 0 for a program that has none.
     * The interpreter relocates itself from this, so it is not a diagnostic:
     * without it a dynamic program faults on its first relocation. */
    uint64_t interp_base;
    /* The program break, the mapping cursor and the region list used to live
     * here, and clone copied them along with the rest of this struct - so every
     * thread had its own. They belong to the process and live in
     * vibeos_procstate_t now. */
    uint64_t user_sp;   /* entry rsp, atop the startup block */
    /* What execve was given. A program that wants to find itself reads
     * /proc/self/exe, and answering from the real path is the difference
     * between a correct answer and a plausible one. */
    char exe_path[VIBEOS_PATH_MAX];
} vibeos_image_t;

/* What belongs to a process rather than to one of its threads.
 *
 * Referenced, never copied: fork and exec create one, a thread takes a
 * reference, and exit gives it back. See g_procstate in the architecture for
 * the five defects that copying produced. */
typedef struct vibeos_procstate {
    volatile uint32_t refs;      /* tasks pointing here; 0 means free        */
    volatile uint32_t mm_busy;   /* one address-space mutation at a time:     */
                                 /* brk, and fork's walk of this process      */
    uint64_t brk_cur;            /* current program break                    */
    volatile uint64_t mmap_cur;  /* next anonymous address, claimed by CAS   */
    /* What this process asked for, as opposed to what happens to be mapped.
     * munmap and mprotect consult this; the page tables are the consequence,
     * not the record. See kernel/mm/vma.c. */
    vibeos_vma_list_t vmas;
    uint64_t sig_handler[VIBEOS_NSIG];
    uint64_t sig_restorer[VIBEOS_NSIG];
    uint64_t sig_flags[VIBEOS_NSIG];
    uint64_t sig_mask[VIBEOS_NSIG];
    /* exit_group: claimed by the first caller, whose code the leader reports. */
    volatile uint32_t exit_group_claimed;
    volatile uint32_t exit_group;
    uint64_t exit_group_code;
    /* Open files: the table (fd 3 up) and what 0, 1 and 2 are redirected to.
     * The process's, like everything above: every thread of it sees one table,
     * which is what CLONE_FILES means and what a C library asks for. Each
     * thread used to get a copy at clone, so a descriptor one thread opened did
     * not exist in the others, and a close in one left the rest holding it.
     *
     * files_lock serialises the changes to *which* descriptors exist - claim,
     * close, dup2, and the copies fork and exec take - so two threads opening
     * at once cannot be handed one slot. It is not held across I/O: two threads
     * reading one descriptor at the same moment share its position without
     * ordering, which is a difference from Linux recorded here rather than
     * hidden.
     *
     * files_users counts the threads still using the table, apart from refs:
     * the table is closed by the thread that brings it to zero, before that
     * thread switches away, while refs outlives it (exit gives the process
     * reference back only after the switch). */
    vibeos_fdtable_t files;
    vibeos_lock_t files_lock;
    volatile uint32_t files_users;
    /* Where relative paths start, and above what ".." cannot climb (docs/abi/
     * A4). Absolute and normal, both "/" for a process nobody changed. The
     * process's, like the descriptors: every thread sees one working directory,
     * which is what CLONE_FS means - and fork and exec carry them over. Read
     * and written under files_lock, so a chdir in one thread cannot hand
     * another half a path. */
    char cwd[VIBEOS_PATH_MAX];
    char root[VIBEOS_PATH_MAX];
} vibeos_procstate_t;

#endif
