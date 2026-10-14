#ifndef VIBEOS_ABI_NT_H
#define VIBEOS_ABI_NT_H

/* The NT personality's registry (docs/abi/windows.md W1): every service in
 * kernel/abi/nt_services.def, and the rows that implement some of them.
 *
 * The counterpart of abi_linux.h, with one difference that matters: the
 * numbers are ours (the .def's order), so there is no foreign numbering to
 * honour and no number without a service. A number past the table is not a
 * service at all - Windows answers STATUS_INVALID_SYSTEM_SERVICE for that. */

#include <stdint.h>

#include "vibeos/abi.h"

typedef enum {
    VIBEOS_NT_DONE = 0,
    VIBEOS_NT_PARTIAL = 1,
    VIBEOS_NT_MISSING = 2,
    VIBEOS_NT_REFUSED = 3
} vibeos_nt_state_t;

typedef struct {
    uint32_t nr;
    const char *name;
    uint32_t args;            /* how many: four in registers, the rest on the stack */
    vibeos_nt_state_t state;
    const char *step;         /* W1..W7, REG, SEC, NO */
} vibeos_nt_service_t;

/* The numbers, by name: VIBEOS_NT_NR_NtWriteFile and so on. */
enum {
#define NT(nr, name, args, state, step) VIBEOS_NT_NR_##name = (nr),
#include "../../kernel/abi/nt_services.def"
#undef NT
    VIBEOS_NT_SERVICES
};

/* The most arguments any service takes; vibeos_call_t holds this many. */
#define VIBEOS_NT_MAX_ARGS 16u

/* Windows' answer for a number that names no service. */
#define VIBEOS_STATUS_INVALID_SYSTEM_SERVICE 0xC000001Cu

const vibeos_nt_service_t *vibeos_nt_service(uint64_t nr);   /* 0 past the table */

/* The personality: bound to a task by exec of a PE image. */
const vibeos_abi_t *vibeos_abi_nt(void);

/* Rows, registered once at boot as the Linux tables are. A number claimed
 * twice, past the table, or a pointer descriptor naming an argument the service
 * does not take is refused. */
int vibeos_abi_nt_register(const vibeos_row_t *rows, uint32_t count);
void vibeos_abi_nt_reset(void);   /* boot and tests */

/* Every DONE or PARTIAL service has a row and no other does: the first number
 * for which that is false, or VIBEOS_NT_SERVICES when the two agree. */
uint32_t vibeos_abi_nt_disagreement(void);

/* The services: the architecture's entry hands over the number, the four
 * register arguments in a[0..3] (r10, rdx, r8, r9 on x86-64) and the frame,
 * whose stack pointer is where the rest are. NTSTATUS, zero-extended. */
struct ks_regs;
long nt_syscall(struct ks_regs *frame, uint64_t nr, const uint64_t a[6]);
void vibeos_nt_abi_init(void);

/* ---- what a process finds in its own memory when it starts -------------------
 *
 * Five pages the loader maps read-write at a fixed address: the PEB, the
 * process parameters with their strings, and the first thread's TEB. Built here,
 * portably, into pages the caller has zeroed and will map; every pointer stored
 * in them is the user address the program will see. */
#define VIBEOS_NT_BLOCK_PAGES 5u
#define VIBEOS_NT_BLOCK_PEB 0x0000u      /* one page                         */
#define VIBEOS_NT_BLOCK_PARAMS 0x1000u   /* two pages: fields, then strings  */
#define VIBEOS_NT_BLOCK_TEB 0x3000u      /* two pages: x64's TEB is 0x1838   */

typedef struct {
    uint64_t image_base;      /* the program, as PEB.ImageBaseAddress          */
    uint64_t stack_base;      /* the top of the first thread's stack           */
    uint64_t stack_limit;     /* the lowest address it may grow to             */
    uint32_t pid, tid;
    uint32_t processors;
    uint64_t std_handle[3];   /* input, output, error                          */
    const char *image_path;   /* as exec was given it                          */
    const char *const *argv;  /* joined into the command line                  */
} vibeos_nt_start_t;

/* 0, or -1 if the strings do not fit in the parameters' pages. `pages` are the
 * kernel's pointers to the five pages, in order; `base` is where the first will
 * be mapped. */
int vibeos_nt_build_blocks(uint8_t *const pages[VIBEOS_NT_BLOCK_PAGES], uint64_t base,
                           const vibeos_nt_start_t *s);

/* The counts the boot reports: services asked for that are not written yet,
 * services refused by decision, and the last of the first kind. */
extern volatile uint64_t g_nt_unimplemented;
extern volatile uint64_t g_nt_refused;
extern volatile uint64_t g_nt_last_nr;

#endif
