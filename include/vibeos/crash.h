#ifndef VIBEOS_CRASH_H
#define VIBEOS_CRASH_H

#include <stdint.h>

/* C6: crash records, as a module.
 *
 * A process that dies from a fault used to leave two numbers behind, rip and
 * cr2, printed once and gone. Every hard bug in this kernel was then diagnosed
 * by going back for the registers, the stack, and which program the task was
 * actually running - and by then the process no longer existed. So the state is
 * taken at the fault and kept, and `crash` on the console prints the last one.
 *
 * The capture is the architecture's: it knows what a trap frame is and how to
 * read a user stack without faulting again. Keeping the record, and saying what
 * is in it, is the same answer everywhere, and that is what lives here.
 *
 * Registers are name/value pairs the architecture fills in, so the dump names
 * them without this module knowing what a register file looks like.
 *
 * Serialised by its own lock (vibeos_crash_set_lock). It had none: the recorder
 * read the ring's cursor, filled the slot and advanced the cursor as three
 * separate steps, so two processes faulting at once on two cores - which is what
 * a bad library does to every program linked against it - wrote one record over
 * the other and advanced past a slot nobody had filled.
 *
 * A ring of four, not one: services restart, and the interesting crash is
 * frequently not the most recent. */

#define VIBEOS_CRASH_RECORDS 4u
#define VIBEOS_CRASH_STACK_WORDS 16u
#define VIBEOS_CRASH_REGS 16u
#define VIBEOS_CRASH_EXE 64u

typedef struct {
    const char *name;    /* a string literal: it outlives the record */
    uint64_t value;
} vibeos_crash_reg_t;

typedef struct {
    uint32_t pid;
    uint32_t sig;
    uint64_t vector;
    uint64_t error_code;
    uint64_t fault_addr;
    vibeos_crash_reg_t regs[VIBEOS_CRASH_REGS];
    uint32_t nregs;
    uint64_t stack[VIBEOS_CRASH_STACK_WORDS];
    uint32_t stack_words;   /* how many were readable; the rest is off-map */
    char exe[VIBEOS_CRASH_EXE];
} vibeos_crash_t;

/* One complete line, as klog's sinks take them. */
typedef int (*vibeos_crash_write_fn)(void *ctx, const char *line, uint32_t len);

void vibeos_crash_set_lock(void (*lock)(void), void (*unlock)(void));
void vibeos_crash_reset(void);

/* Keep a copy of `rec`. Returns how many have been recorded since boot,
 * including this one. */
uint64_t vibeos_crash_record(const vibeos_crash_t *rec);
uint64_t vibeos_crash_count(void);

/* The most recent record, copied out whole under the lock; -1 when nothing has
 * crashed. `back` counts backwards from it: 0 is the latest. */
int vibeos_crash_get(uint32_t back, vibeos_crash_t *out);

/* The most recent record in full, one call per line, ending "[CRASH] end". Says
 * so plainly when nothing has crashed - more useful than an empty record. The
 * caller brackets the dump on its device if it wants it unbroken. */
void vibeos_crash_dump(vibeos_crash_write_fn out, void *ctx);

#endif
