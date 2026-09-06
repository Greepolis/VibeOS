#ifndef VIBEOS_FSMOUNT_H
#define VIBEOS_FSMOUNT_H

/* The filesystem abstraction.
 *
 * Until this existed, the syscall layer called the FAT driver directly from
 * twenty places, so "filesystem" and "FAT" were the same word in this kernel.
 * Everything here is about breaking that identity: the syscalls talk to a
 * mounted volume through function pointers, and a driver is whatever fills
 * them in.
 *
 * The shape is deliberately the shape the syscalls already need - lookup, read
 * at an offset, write a whole file, enumerate a directory, unlink, mkdir - so
 * that porting FAT onto it is a refactor with no behaviour change and the boot
 * gate can say whether it worked. Designing a wider interface now would mean
 * inventing requirements, and the requirements are about to arrive on their
 * own: ext2 has inodes, block groups and indirect blocks, and fitting it in
 * here is what will tell us whether this interface was designed or merely
 * extracted from FAT. Expect it to change then. That is the plan, not a
 * failure of it - see docs/storage_plan.md.
 *
 * One thing is already known to be missing: writing a whole file at a time is
 * FAT's shape, not a filesystem's. A real write path takes an offset. It is
 * left alone here because changing it in the same step as the abstraction
 * would mean neither is verified.
 */

#include <stdint.h>

#define VIBEOS_FS_NAME_MAX 64u

typedef struct {
    /* Driver-private identity for the file. FAT puts the first cluster here.
     * Opaque above this line: the syscall layer must never interpret it. */
    uint64_t id;
    uint64_t size;
    int is_dir;
} vibeos_fs_node_t;

typedef struct {
    /* Resolve a path. Returns 0 and fills `out` on success. */
    int (*lookup)(void *fs, const char *path, vibeos_fs_node_t *out);

    /* Read from an already-resolved node. Returns bytes read, or negative on
     * failure. A short return means end of file; a failure means the volume
     * could not be read, and the two must not be confused - conflating them is
     * exactly the bug that made a truncated program look like a whole one. */
    long (*read_at)(void *fs, const vibeos_fs_node_t *node, uint64_t offset,
                    void *buf, uint32_t len);

    /* Replace a file's contents, creating it if needed. */
    long (*write_file)(void *fs, const char *path, const void *buf, uint32_t len);

    /* One directory entry by index. Returns 0 while entries remain. */
    int (*list)(void *fs, const char *path, uint32_t index,
                char *name, uint32_t name_cap, uint64_t *out_size, int *out_is_dir);

    int (*unlink)(void *fs, const char *path);
    int (*mkdir)(void *fs, const char *path);
} vibeos_fs_ops_t;

typedef struct {
    const vibeos_fs_ops_t *ops;
    void *fs;             /* driver state, passed back to every operation */
    const char *type;     /* "fat", "ext2", ... - for reporting, not dispatch */
    int mounted;
} vibeos_fsmount_t;

/* There is one volume. A mount table with paths belongs with partition support
 * (stage four), and adding it before there is a second filesystem to mount
 * would be building a mechanism against an imagined requirement. */
int vibeos_fs_mount(vibeos_fsmount_t *mnt, const vibeos_fs_ops_t *ops,
                     void *fs, const char *type);
void vibeos_fs_unmount(vibeos_fsmount_t *mnt);
int vibeos_fs_is_mounted(const vibeos_fsmount_t *mnt);
const char *vibeos_fs_type(const vibeos_fsmount_t *mnt);

/* Every call refuses politely on an unmounted volume rather than following a
 * null pointer: these are reached straight from syscalls, and a program asking
 * about a filesystem that is not there is ordinary, not exceptional. */
int vibeos_fs_lookup(vibeos_fsmount_t *mnt, const char *path,
                      vibeos_fs_node_t *out);
long vibeos_fs_read_at(vibeos_fsmount_t *mnt, const vibeos_fs_node_t *node,
                        uint64_t offset, void *buf, uint32_t len);
long vibeos_fs_write_file(vibeos_fsmount_t *mnt, const char *path,
                           const void *buf, uint32_t len);
int vibeos_fs_list(vibeos_fsmount_t *mnt, const char *path, uint32_t index,
                    char *name, uint32_t name_cap, uint64_t *out_size,
                    int *out_is_dir);
int vibeos_fs_unlink(vibeos_fsmount_t *mnt, const char *path);
int vibeos_fs_mkdir(vibeos_fsmount_t *mnt, const char *path);

/* ---- the mount table (I4b step 4) -----------------------------------------
 *
 * There was one global mount, and that was the structural reason only one
 * filesystem could run: not a missing driver, a missing *place to put* a
 * second one. Every syscall took a `vibeos_fsmount_t *` that only ever had one
 * value.
 *
 * A small fixed array. No allocation on these paths - they are reached from
 * syscalls and from the boot, and a table that allocates fails exactly when
 * the machine is short of memory, which is when a program is most likely to be
 * opening files.
 *
 * ## Resolution is longest-prefix, and the order it is stored in does not
 * matter
 *
 * A path belongs to the mount with the longest matching prefix, so "/usr/lib"
 * beats "/usr" beats "/". Scanning for the longest rather than the first is
 * what makes the table order-independent - a resolver that took the first
 * match would give different answers depending on the order things happened to
 * mount in, which is the kind of defect that appears months later on a machine
 * with one extra volume.
 *
 * ## Two mounts may not claim one path
 *
 * Refused, not overwritten. Overwriting would leave the first filesystem
 * mounted and unreachable: its files still open, its blocks still dirty, and
 * nothing able to name it to unmount it.
 */
#define VIBEOS_FS_MOUNTS_MAX 8u
#define VIBEOS_FS_MOUNT_PATH_MAX 32u

/* Attach a mounted volume at `path`. `path` must start with '/'.
 *
 * Returns 0, or negative when the path is taken, malformed, or the table is
 * full. A full table is a configuration this build cannot express, not a
 * transient failure, so it is reported rather than retried. */
int vibeos_fs_attach(const char *path, vibeos_fsmount_t *mnt);

/* Detach whatever is at `path`. Does not unmount: the caller owns the mount. */
int vibeos_fs_detach(const char *path);

/* Which mount owns `path`, and what is left of the path inside it.
 *
 * `out_tail` points into `path`; it is never allocated and never modified. A
 * path that resolves to the root mount comes back with the whole path minus
 * the mount prefix, so a driver always sees a path relative to its own root -
 * which is what lets the same driver be mounted twice in different places.
 *
 * Returns 0 on a match. */
int vibeos_fs_resolve(const char *path, vibeos_fsmount_t **out_mnt,
                      const char **out_tail);

/* How many mounts are attached, and the nth one's path. For reporting: a
 * machine that cannot say what it has mounted cannot be asked to prove it
 * mounted more than one thing. */
uint32_t vibeos_fs_mount_count(void);
const char *vibeos_fs_mount_path(uint32_t index);
vibeos_fsmount_t *vibeos_fs_mount_at(uint32_t index);

/* Forget every attachment. For tests. */
void vibeos_fs_detach_all(void);

/* Read a whole file by path. Common enough - exec, the boot loader - to be
 * worth expressing once rather than at each caller, and it is the one place
 * that has to insist a short read is a failure rather than a smaller file. */
long vibeos_fs_read_file(vibeos_fsmount_t *mnt, const char *path,
                          void *buf, uint32_t cap);

#endif
