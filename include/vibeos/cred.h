#ifndef VIBEOS_CRED_H
#define VIBEOS_CRED_H

#include <stdint.h>

/* Who a process is (docs/abi/ L2 step 1).
 *
 * Until this there was one identity and it was root: setuid(0) succeeded,
 * anything else was refused, and no file operation asked who was asking. A
 * program that gives up its privileges - which is how a server, a login and
 * every test that checks a refusal work - could not run at all.
 *
 * Four user ids and four group ids, as POSIX and Linux have them:
 *
 *   real       whose process it is; what access() answers for
 *   effective  whose privileges it has now; euid 0 is the superuser
 *   saved      what the effective one can be set back to
 *   fs         what file operations are judged by; follows the effective one
 *
 * and a list of supplementary groups. The rules for changing them are the
 * ones Linux applies without capabilities: "privileged" is euid 0.
 *
 * Pure functions over a struct the caller owns: no state here, and nothing
 * Linux's in the interface - a Windows personality would map its own notion
 * of an owner onto the same ids, or carry its own beside them.
 */

#define VIBEOS_NGROUPS 32u
#define VIBEOS_ID_KEEP 0xFFFFFFFFu   /* "leave this one as it is" in the set calls */

typedef struct {
    uint32_t uid, euid, suid, fsuid;
    uint32_t gid, egid, sgid, fsgid;
    uint32_t ngroups;
    uint32_t groups[VIBEOS_NGROUPS];
} vibeos_cred_t;

/* What an access wants of a file, as the permission bits spell it. */
#define VIBEOS_MAY_EXEC  1u
#define VIBEOS_MAY_WRITE 2u
#define VIBEOS_MAY_READ  4u

/* Root: every id zero, no supplementary groups. */
void vibeos_cred_root(vibeos_cred_t *c);

/* Is `gid` one of the caller's groups: the group that file operations are
 * judged by (or the real one, when `real`), or a supplementary one. */
int vibeos_cred_in_group(const vibeos_cred_t *c, uint32_t gid, int real);

/* May the caller do `want` (VIBEOS_MAY_*) to a file with these permission bits
 * and this owner and group? 0, or -EACCES. Judged by the fs ids, or by the real
 * ones when `real` - which is what access() asks. The owner's bits apply to the
 * owner even when the group's or everybody's would allow more; the superuser
 * may read and write anything, and run what somebody may run. */
int vibeos_cred_may(const vibeos_cred_t *c, int real, uint32_t mode, uint32_t uid, uint32_t gid,
                    uint32_t want);

/* The set calls. Each returns 0 or -EPERM, and changes nothing when refused.
 * VIBEOS_ID_KEEP leaves an id alone where the call has that meaning. */
int vibeos_cred_setuid(vibeos_cred_t *c, uint32_t uid);
int vibeos_cred_setgid(vibeos_cred_t *c, uint32_t gid);
int vibeos_cred_setreuid(vibeos_cred_t *c, uint32_t ruid, uint32_t euid);
int vibeos_cred_setregid(vibeos_cred_t *c, uint32_t rgid, uint32_t egid);
int vibeos_cred_setresuid(vibeos_cred_t *c, uint32_t ruid, uint32_t euid, uint32_t suid);
int vibeos_cred_setresgid(vibeos_cred_t *c, uint32_t rgid, uint32_t egid, uint32_t sgid);
/* These two never fail: they return the id that was there, changed or not. */
uint32_t vibeos_cred_setfsuid(vibeos_cred_t *c, uint32_t uid);
uint32_t vibeos_cred_setfsgid(vibeos_cred_t *c, uint32_t gid);
/* -EPERM unless privileged, -EINVAL for more groups than there is room for. */
int vibeos_cred_setgroups(vibeos_cred_t *c, uint32_t n, const uint32_t *list);

#endif /* VIBEOS_CRED_H */
