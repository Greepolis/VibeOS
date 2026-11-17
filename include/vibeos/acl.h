/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef VIBEOS_ACL_H
#define VIBEOS_ACL_H

/* POSIX access control lists (E4-011), as the VFS carries them: the value of
 * attribute VIBEOS_XATTR_ACL_ACCESS or _ACL_DEFAULT is a version word and then
 * entries of a tag, permission bits and an id, little-endian - the layout
 * POSIX's draft and every Unix that implements it use, so a personality hands
 * it through and a filesystem converts it to its own format on the disk (ext4's
 * is shorter). What a list *means* to a permission check is not here yet: it
 * is stored, read back and kept in step with the mode, and nothing is decided
 * by it (docs/filesystems/ext4/progress.md E4-011; enforcement belongs
 * to the security milestone). */

#include <stdint.h>

#include "vibeos/vfs.h"

#define VIBEOS_ACL_VERSION 2u
#define VIBEOS_ACL_USER_OBJ 0x01u
#define VIBEOS_ACL_USER 0x02u
#define VIBEOS_ACL_GROUP_OBJ 0x04u
#define VIBEOS_ACL_GROUP 0x08u
#define VIBEOS_ACL_MASK 0x10u
#define VIBEOS_ACL_OTHER 0x20u
#define VIBEOS_ACL_UNDEFINED_ID 0xFFFFFFFFu
#define VIBEOS_ACL_HDR 4u
#define VIBEOS_ACL_ENTRY 8u

/* Whether `val` is a well-formed list: one owner, one owning group and one
 * other entry, in tag order, named users and groups in id order and none
 * twice, a mask whenever anybody is named, permissions of three bits. 0, or
 * -EINVAL. An empty list (the header alone) is valid: it means "none". */
int vibeos_acl_valid(const uint8_t *val, uint32_t len);

/* The permission bits a list stands for - the owner's, the mask's (or the
 * owning group's when there is none) and other's - and whether the mode alone
 * says everything it does (1: nobody named and no mask). */
int vibeos_acl_mode(const uint8_t *val, uint32_t len, uint32_t *perm);

/* Set a list on `node`: validated (-EINVAL), a default list only on a
 * directory (-EACCES elsewhere), an empty one removed. What an access list does
 * to the mode - Linux's posix_acl_update_mode: the mode takes the bits it
 * stands for, and a list the mode says all of is not kept - is the
 * filesystem's to do, in the same transaction as the list, since a power cut
 * between the two would leave a file that matches neither. The caller has
 * decided who may. */
int vibeos_acl_set(vibeos_fsmount_t *mnt, const vibeos_fs_node_t *node, uint32_t index, const uint8_t *val,
                   uint32_t len);

/* A chmod on a list, as Linux's posix_acl_chmod: its owner, mask (or owning
 * group) and other entries take the new bits. For a filesystem to apply to the
 * list it keeps, in the transaction that changes the mode. `val` is valid. */
void vibeos_acl_chmod(uint8_t *val, uint32_t len, uint32_t mode);

#endif
