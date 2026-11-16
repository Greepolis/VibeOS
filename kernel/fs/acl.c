/* SPDX-License-Identifier: GPL-3.0-or-later */
/* POSIX access control lists over the VFS's attributes (E4-011): their form
 * and their relation to the mode. See vibeos/acl.h. */

#include "vibeos/acl.h"

#include "vibeos/abi_linux.h"

static uint32_t rd16(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr16(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

int vibeos_acl_valid(const uint8_t *val, uint32_t len) {
    uint32_t n, i, state = 0, last_id = 0, named = 0, have_mask = 0;

    if (!val || len < VIBEOS_ACL_HDR || (len - VIBEOS_ACL_HDR) % VIBEOS_ACL_ENTRY != 0u ||
        rd32(val) != VIBEOS_ACL_VERSION) {
        return -VIBEOS_EINVAL;
    }
    n = (len - VIBEOS_ACL_HDR) / VIBEOS_ACL_ENTRY;
    if (n == 0u) {
        return 0;
    }
    /* Linux's posix_acl_valid: the tags climb, the named ones by id. */
    for (i = 0; i < n; i++) {
        const uint8_t *e = val + VIBEOS_ACL_HDR + i * VIBEOS_ACL_ENTRY;
        uint32_t tag = rd16(e), perm = rd16(e + 2u), id = rd32(e + 4u);

        if (perm & ~7u) {
            return -VIBEOS_EINVAL;
        }
        switch (tag) {
        case VIBEOS_ACL_USER_OBJ:
            if (state != 0u) {
                return -VIBEOS_EINVAL;
            }
            state = 1;
            break;
        case VIBEOS_ACL_USER:
            if (state != 1u || (named & 1u && id <= last_id)) {
                return -VIBEOS_EINVAL;
            }
            named |= 1u;
            last_id = id;
            break;
        case VIBEOS_ACL_GROUP_OBJ:
            if (state != 1u) {
                return -VIBEOS_EINVAL;
            }
            state = 2;
            break;
        case VIBEOS_ACL_GROUP:
            if (state != 2u || (named & 2u && id <= last_id)) {
                return -VIBEOS_EINVAL;
            }
            named |= 2u;
            last_id = id;
            break;
        case VIBEOS_ACL_MASK:
            if (state != 2u) {
                return -VIBEOS_EINVAL;
            }
            state = 3;
            have_mask = 1;
            break;
        case VIBEOS_ACL_OTHER:
            if (state != 2u && state != 3u) {
                return -VIBEOS_EINVAL;
            }
            state = 4;
            break;
        default:
            return -VIBEOS_EINVAL;
        }
    }
    if (state != 4u || (named != 0u && !have_mask)) {
        return -VIBEOS_EINVAL;
    }
    return 0;
}

int vibeos_acl_mode(const uint8_t *val, uint32_t len, uint32_t *perm) {
    uint32_t n = (len - VIBEOS_ACL_HDR) / VIBEOS_ACL_ENTRY, i, u = 0, g = 0, o = 0, m = 0, has_m = 0;
    int equiv = 1;

    for (i = 0; i < n; i++) {
        const uint8_t *e = val + VIBEOS_ACL_HDR + i * VIBEOS_ACL_ENTRY;
        uint32_t p = rd16(e + 2u);

        switch (rd16(e)) {
        case VIBEOS_ACL_USER_OBJ:
            u = p;
            break;
        case VIBEOS_ACL_GROUP_OBJ:
            g = p;
            break;
        case VIBEOS_ACL_MASK:
            /* A mask is a list the mode cannot say, even with nobody named:
             * Linux's posix_acl_equiv_mode keeps it. */
            m = p;
            has_m = 1;
            equiv = 0;
            break;
        case VIBEOS_ACL_OTHER:
            o = p;
            break;
        default:
            equiv = 0;
            break;
        }
    }
    *perm = (u << 6) | ((has_m ? m : g) << 3) | o;
    return equiv;
}

int vibeos_acl_set(vibeos_fsmount_t *mnt, const vibeos_fs_node_t *node, uint32_t index, const uint8_t *val,
                   uint32_t len) {
    int r;

    /* Linux's order: the value's shape (posix_acl_from_xattr), then whether
     * the file can have such a list - none on a file that cannot is nothing
     * to do, a list there is EACCES, however malformed - and only then the
     * list itself (posix_acl_valid). */
    if (!val || len < VIBEOS_ACL_HDR || (len - VIBEOS_ACL_HDR) % VIBEOS_ACL_ENTRY != 0u ||
        (val[0] | (val[1] << 8) | (val[2] << 16) | ((uint32_t)val[3] << 24)) != VIBEOS_ACL_VERSION ||
        (index != VIBEOS_XATTR_ACL_ACCESS && index != VIBEOS_XATTR_ACL_DEFAULT)) {
        return -VIBEOS_EINVAL;
    }
    if (index == VIBEOS_XATTR_ACL_DEFAULT && (node->mode & VIBEOS_S_IFMT) != VIBEOS_S_IFDIR) {
        return len == VIBEOS_ACL_HDR ? 0 : -VIBEOS_EACCES;
    }
    r = vibeos_acl_valid(val, len);
    if (r != 0) {
        return r;
    }
    if (len == VIBEOS_ACL_HDR) {
        r = vibeos_fs_removexattr(mnt, node, index, "", 0);
        return r == -VIBEOS_ENODATA ? 0 : r;
    }
    return vibeos_fs_setxattr(mnt, node, index, "", 0, val, len, 0);
}

void vibeos_acl_chmod(uint8_t *buf, uint32_t len, uint32_t mode) {
    uint32_t n = (len - VIBEOS_ACL_HDR) / VIBEOS_ACL_ENTRY, i, has_m = 0;

    for (i = 0; i < n; i++) {
        has_m |= rd16(buf + VIBEOS_ACL_HDR + i * VIBEOS_ACL_ENTRY) == VIBEOS_ACL_MASK;
    }
    for (i = 0; i < n; i++) {
        uint8_t *e = buf + VIBEOS_ACL_HDR + i * VIBEOS_ACL_ENTRY;
        uint32_t tag = rd16(e);

        if (tag == VIBEOS_ACL_USER_OBJ) {
            wr16(e + 2u, (mode >> 6) & 7u);
        } else if (tag == VIBEOS_ACL_MASK || (tag == VIBEOS_ACL_GROUP_OBJ && !has_m)) {
            wr16(e + 2u, (mode >> 3) & 7u);
        } else if (tag == VIBEOS_ACL_OTHER) {
            wr16(e + 2u, mode & 7u);
        }
    }
}
