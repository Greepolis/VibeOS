#ifndef VIBEOS_EXEC_STATS_H
#define VIBEOS_EXEC_STATS_H

#include <stdint.h>

/* Why an exec was refused. Phase X-P0 of docs/exec/.
 *
 * The whole point of this enum is that "cannot load" is not a diagnosis. The
 * loader had fourteen `return -1` sites and a single message for all of them,
 * so a program that failed to start told you only that it had failed to start -
 * and the difference between "the file is not there", "the file is there and
 * short", and "the file is fine and there was no memory" is the difference
 * between three unrelated investigations.
 *
 * This mattered more than it sounds. A failed FAT lookup used to look like the
 * end of a file, so a flaky sector produced a short image that claimed to be
 * complete; the loader then parsed whatever the previous program had left in
 * the shared staging buffer. That defect is fixed, but nothing in the exec path
 * would have *said* so, because every one of those outcomes printed the same
 * sentence.
 *
 * Kept portable and separate from the architecture layer so the counters can be
 * asserted by a host test as well as by the boot gate.
 */
typedef enum vibeos_exec_fail {
    VIBEOS_EXEC_OK = 0,
    VIBEOS_EXEC_NOT_FOUND,        /* the path did not resolve                 */
    VIBEOS_EXEC_SHORT_READ,       /* fewer bytes than the directory claimed   */
    VIBEOS_EXEC_BAD_HEADER,       /* the parser refused the image             */
    VIBEOS_EXEC_BAD_WINDOW,       /* it does not fit either user window       */
    VIBEOS_EXEC_NO_INTERP,        /* named an interpreter that is not there   */
    VIBEOS_EXEC_INTERP_CHAIN,     /* an interpreter that needs an interpreter */
    VIBEOS_EXEC_NO_STAGING,       /* no staging buffer for the image          */
    VIBEOS_EXEC_TOO_LARGE,        /* larger than the staging buffer           */
    VIBEOS_EXEC_NO_MEMORY,        /* an allocation or a mapping failed        */
    VIBEOS_EXEC_ARGS_TOO_LARGE,   /* argv and envp exceed the stack mapped    */
    VIBEOS_EXEC_NO_ASPACE,        /* could not create an address space        */
    VIBEOS_EXEC_REASON_COUNT
} vibeos_exec_fail_t;

/* One counter per reason, indexed by the enum. A boot asserts the shape of this
 * array rather than a total: the gate knows which refusals a boot is supposed
 * to perform (a service that is meant to fail to start is a test, not a bug),
 * and any other reason appearing is the failure. */
typedef struct vibeos_exec_stats {
    uint64_t refused[VIBEOS_EXEC_REASON_COUNT];
    uint64_t loaded;              /* images that started                      */
} vibeos_exec_stats_t;

vibeos_exec_stats_t *vibeos_exec_stats(void);
void vibeos_exec_stats_reset(void);

/* The name printed in the log and in the summary. Stable text: the boot gate
 * matches on it, so it is part of the interface rather than prose. */
const char *vibeos_exec_fail_name(vibeos_exec_fail_t why);

/* Count a refusal and hand back its name, so a caller can do both in the one
 * expression that precedes its `return`. Returns "?" for a value outside the
 * enum rather than reading past the array. */
const char *vibeos_exec_refuse(vibeos_exec_fail_t why);

/* Print the counters. Lives in kernel/exec/view.c; declared here so the CLI
 * does not have to know which file answers. */
void vibeos_exec_print_stats(void);

#endif /* VIBEOS_EXEC_STATS_H */
