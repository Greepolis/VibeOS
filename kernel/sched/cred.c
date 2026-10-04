/* Credentials. See include/vibeos/cred.h. */

#include "vibeos/cred.h"
#include "vibeos/abi_linux.h"

void vibeos_cred_root(vibeos_cred_t *c) {
    unsigned char *raw = (unsigned char *)c;
    uint32_t i;

    for (i = 0; i < (uint32_t)sizeof(*c); i++) {
        raw[i] = 0;
    }
}

static int privileged(const vibeos_cred_t *c) {
    return c->euid == 0u;
}

int vibeos_cred_in_group(const vibeos_cred_t *c, uint32_t gid, int real) {
    uint32_t i;

    if (gid == (real ? c->gid : c->fsgid)) {
        return 1;
    }
    for (i = 0; i < c->ngroups && i < VIBEOS_NGROUPS; i++) {
        if (c->groups[i] == gid) {
            return 1;
        }
    }
    return 0;
}

int vibeos_cred_may(const vibeos_cred_t *c, int real, uint32_t mode, uint32_t uid, uint32_t gid,
                    uint32_t want) {
    const uint32_t who = real ? c->uid : c->fsuid;
    uint32_t bits;

    if (who == 0u) {
        /* The superuser reads and writes anything. Running is different: a
         * file nobody may run is not a program, for root either - but a
         * directory can always be searched. */
        if ((want & VIBEOS_MAY_EXEC) && (mode & 0170000u) != 0040000u && (mode & 0111u) == 0u) {
            return -VIBEOS_EACCES;
        }
        return 0;
    }
    /* One class decides, the first that applies: an owner denied by the
     * owner's bits is denied even where "other" would be allowed. */
    if (who == uid) {
        bits = (mode >> 6) & 7u;
    } else if (vibeos_cred_in_group(c, gid, real)) {
        bits = (mode >> 3) & 7u;
    } else {
        bits = mode & 7u;
    }
    return (bits & want) == want ? 0 : -VIBEOS_EACCES;
}

/* ---- changing who one is ---------------------------------------------------------
 *
 * The rules are Linux's, for a kernel without capabilities. An unprivileged
 * process can only move between ids it already holds: that is what makes
 * "drop privileges, do the work, take them back" possible with the saved id
 * and "drop them for good" possible by setting all three. */

int vibeos_cred_setuid(vibeos_cred_t *c, uint32_t uid) {
    if (privileged(c)) {
        c->uid = c->euid = c->suid = c->fsuid = uid;   /* for good: no way back */
        return 0;
    }
    if (uid != c->uid && uid != c->suid) {
        return -VIBEOS_EPERM;
    }
    c->euid = c->fsuid = uid;
    return 0;
}

int vibeos_cred_setgid(vibeos_cred_t *c, uint32_t gid) {
    if (privileged(c)) {
        c->gid = c->egid = c->sgid = c->fsgid = gid;
        return 0;
    }
    if (gid != c->gid && gid != c->sgid) {
        return -VIBEOS_EPERM;
    }
    c->egid = c->fsgid = gid;
    return 0;
}

int vibeos_cred_setreuid(vibeos_cred_t *c, uint32_t ruid, uint32_t euid) {
    const uint32_t old_r = c->uid, old_e = c->euid, old_s = c->suid;

    if (!privileged(c)) {
        if (ruid != VIBEOS_ID_KEEP && ruid != old_r && ruid != old_e) {
            return -VIBEOS_EPERM;
        }
        if (euid != VIBEOS_ID_KEEP && euid != old_r && euid != old_e && euid != old_s) {
            return -VIBEOS_EPERM;
        }
    }
    if (ruid != VIBEOS_ID_KEEP) {
        c->uid = ruid;
    }
    if (euid != VIBEOS_ID_KEEP) {
        c->euid = euid;
    }
    /* The saved id follows the effective one whenever the real one was set,
     * or the effective one was set to something other than the old real id:
     * otherwise a process could keep a way back that this call is used to
     * give up. */
    if (ruid != VIBEOS_ID_KEEP || (euid != VIBEOS_ID_KEEP && euid != old_r)) {
        c->suid = c->euid;
    }
    c->fsuid = c->euid;
    return 0;
}

int vibeos_cred_setregid(vibeos_cred_t *c, uint32_t rgid, uint32_t egid) {
    const uint32_t old_r = c->gid, old_e = c->egid, old_s = c->sgid;

    if (!privileged(c)) {
        if (rgid != VIBEOS_ID_KEEP && rgid != old_r && rgid != old_e) {
            return -VIBEOS_EPERM;
        }
        if (egid != VIBEOS_ID_KEEP && egid != old_r && egid != old_e && egid != old_s) {
            return -VIBEOS_EPERM;
        }
    }
    if (rgid != VIBEOS_ID_KEEP) {
        c->gid = rgid;
    }
    if (egid != VIBEOS_ID_KEEP) {
        c->egid = egid;
    }
    if (rgid != VIBEOS_ID_KEEP || (egid != VIBEOS_ID_KEEP && egid != old_r)) {
        c->sgid = c->egid;
    }
    c->fsgid = c->egid;
    return 0;
}

static int held(uint32_t id, uint32_t a, uint32_t b, uint32_t d) {
    return id == VIBEOS_ID_KEEP || id == a || id == b || id == d;
}

int vibeos_cred_setresuid(vibeos_cred_t *c, uint32_t ruid, uint32_t euid, uint32_t suid) {
    if (!privileged(c) && (!held(ruid, c->uid, c->euid, c->suid) ||
                           !held(euid, c->uid, c->euid, c->suid) ||
                           !held(suid, c->uid, c->euid, c->suid))) {
        return -VIBEOS_EPERM;
    }
    if (ruid != VIBEOS_ID_KEEP) {
        c->uid = ruid;
    }
    if (euid != VIBEOS_ID_KEEP) {
        c->euid = euid;
    }
    if (suid != VIBEOS_ID_KEEP) {
        c->suid = suid;
    }
    c->fsuid = c->euid;
    return 0;
}

int vibeos_cred_setresgid(vibeos_cred_t *c, uint32_t rgid, uint32_t egid, uint32_t sgid) {
    if (!privileged(c) && (!held(rgid, c->gid, c->egid, c->sgid) ||
                           !held(egid, c->gid, c->egid, c->sgid) ||
                           !held(sgid, c->gid, c->egid, c->sgid))) {
        return -VIBEOS_EPERM;
    }
    if (rgid != VIBEOS_ID_KEEP) {
        c->gid = rgid;
    }
    if (egid != VIBEOS_ID_KEEP) {
        c->egid = egid;
    }
    if (sgid != VIBEOS_ID_KEEP) {
        c->sgid = sgid;
    }
    c->fsgid = c->egid;
    return 0;
}

/* -1 is nobody's id, and never becomes one: setfsuid(-1) is how a program asks
 * for the current id without changing it. The superuser's call used to store
 * it, and the next question was answered with -1 (LTP's setfsuid02 and
 * setfsgid01, L2 step 7). */
uint32_t vibeos_cred_setfsuid(vibeos_cred_t *c, uint32_t uid) {
    const uint32_t old = c->fsuid;

    if (uid != VIBEOS_ID_KEEP &&
        (privileged(c) || uid == c->uid || uid == c->euid || uid == c->suid || uid == c->fsuid)) {
        c->fsuid = uid;
    }
    return old;
}

uint32_t vibeos_cred_setfsgid(vibeos_cred_t *c, uint32_t gid) {
    const uint32_t old = c->fsgid;

    if (gid != VIBEOS_ID_KEEP &&
        (privileged(c) || gid == c->gid || gid == c->egid || gid == c->sgid || gid == c->fsgid)) {
        c->fsgid = gid;
    }
    return old;
}

int vibeos_cred_setgroups(vibeos_cred_t *c, uint32_t n, const uint32_t *list) {
    uint32_t i;

    if (!privileged(c)) {
        return -VIBEOS_EPERM;
    }
    if (n > VIBEOS_NGROUPS) {
        return -VIBEOS_EINVAL;
    }
    for (i = 0; i < n; i++) {
        c->groups[i] = list[i];
    }
    c->ngroups = n;
    return 0;
}
