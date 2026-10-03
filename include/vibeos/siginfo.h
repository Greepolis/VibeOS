#ifndef VIBEOS_SIGINFO_H
#define VIBEOS_SIGINFO_H

#include <stdint.h>

/* Why a signal was raised, kept with it until it is delivered (docs/abi/ L2).
 *
 * Facts, not a personality's encoding: who sent it, which fault, how a child
 * ended. The Linux layer turns this into a siginfo_t and its si_code numbers
 * at delivery; a Windows personality would turn the same record into an
 * exception record. The architecture fills in a fault, a kill handler fills in
 * a sender, and neither has to know the other's numbering. */
typedef enum {
    VIBEOS_SIG_FROM_KERNEL = 0,   /* the kernel raised it: SIGPIPE, ^C, exit_group */
    VIBEOS_SIG_FROM_PROCESS,      /* kill()                                        */
    VIBEOS_SIG_FROM_THREAD,       /* tkill(), tgkill()                             */
    VIBEOS_SIG_FROM_QUEUE,        /* sigqueueinfo: code, pid, uid, value as given  */
    VIBEOS_SIG_FROM_FAULT,        /* a CPU exception in the program                */
    VIBEOS_SIG_FROM_CHILD,        /* a child ended                                 */
    VIBEOS_SIG_FROM_TIMER         /* a POSIX timer: code = its id, status = overrun, addr = value */
} vibeos_sig_from_t;

/* How a child ended, for VIBEOS_SIG_FROM_CHILD. */
#define VIBEOS_CHILD_EXITED 1u
#define VIBEOS_CHILD_KILLED 2u

typedef struct vibeos_siginfo {
    uint32_t from;        /* vibeos_sig_from_t                                    */
    int32_t code;         /* QUEUE: the sender's code; CHILD: VIBEOS_CHILD_*      */
    uint32_t pid;         /* the sender, or the child                             */
    uint32_t uid;         /* the sender's real user, or the child's               */
    int32_t status;       /* CHILD: the exit code, or the signal that ended it    */
    uint32_t trapno;      /* FAULT: the exception vector                          */
    uint64_t fault_err;   /* FAULT: the error code the CPU pushed                 */
    uint64_t addr;        /* FAULT: the address; QUEUE: the value                 */
    uint64_t utime;       /* CHILD: ticks it ran                                  */
} vibeos_siginfo_t;

#endif
