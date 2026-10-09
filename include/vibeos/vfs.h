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
 * failure of it - see docs/archive/storage_plan.md.
 *
 * One thing is already known to be missing: writing a whole file at a time is
 * FAT's shape, not a filesystem's. A real write path takes an offset. It is
 * left alone here because changing it in the same step as the abstraction
 * would mean neither is verified.
 *
 * docs/abi/ L1 is where it grew: a node carries what stat reports, and the
 * operations a POSIX file system needs - write and truncate at an offset,
 * create, rmdir, rename, links, attributes, statfs, sync - were added beside the
 * original six. Every one of them is optional. A driver fills in what its
 * on-disk format can do, and the wrappers answer for the rest as Linux does:
 * EROFS from a filesystem that writes nothing, EPERM from one that writes but
 * cannot represent the thing asked for (a symbolic link on FAT). The new
 * operations return 0 or a negated errno (vibeos/abi_linux.h); the original
 * six keep their -1.
 */

#include <stdint.h>

#define VIBEOS_FS_NAME_MAX 64u

/* File types and permission bits: st_mode as POSIX numbers it, which is what
 * Linux reports and what every filesystem here is translated into. */
#define VIBEOS_S_IFMT   0170000u
#define VIBEOS_S_IFIFO  0010000u
#define VIBEOS_S_IFCHR  0020000u
#define VIBEOS_S_IFDIR  0040000u
#define VIBEOS_S_IFBLK  0060000u
#define VIBEOS_S_IFREG  0100000u
#define VIBEOS_S_IFLNK  0120000u
#define VIBEOS_S_IFSOCK 0140000u
/* Set-user-id and set-group-id: chown clears them on a regular file. */
#define VIBEOS_S_ISUID  0004000u
#define VIBEOS_S_ISGID  0002000u
#define VIBEOS_S_ISVTX  0001000u   /* on a directory: a name goes only for its file's owner */

typedef struct {
    /* Driver-private identity for the file. FAT puts the first cluster here.
     * Opaque above this line: the syscall layer must never interpret it. */
    uint64_t id;
    uint64_t size;
    int is_dir;
    /* What stat reports (L1). vibeos_fs_lookup zeroes the node before asking
     * the driver - a field a driver never heard of must not arrive holding
     * whatever was on the stack - and supplies what it left out: a type from
     * is_dir, permissions 0755 or 0644, one link. is_dir and the type always
     * agree afterwards. */
    uint32_t mode;                /* VIBEOS_S_IF* | permission bits */
    uint32_t nlink;
    uint32_t uid;
    uint32_t gid;
    uint32_t rdev;                /* a device node's number (vibeos/devfs.h)  */
    uint64_t atime_ns;            /* nanoseconds on vibeos_fs_now_ns's clock */
    uint64_t mtime_ns;
    uint64_t ctime_ns;
} vibeos_fs_node_t;

/* What setattr may change: the fields named in `valid`. */
#define VIBEOS_ATTR_MODE  0x01u   /* permission bits only; the type is fixed */
#define VIBEOS_ATTR_UID   0x02u
#define VIBEOS_ATTR_GID   0x04u
#define VIBEOS_ATTR_ATIME 0x08u
#define VIBEOS_ATTR_MTIME 0x10u

typedef struct {
    uint32_t valid;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint64_t atime_ns;
    uint64_t mtime_ns;
} vibeos_fs_attr_t;

/* What statfs reports. `magic` is the filesystem's own number where Linux has
 * one (TMPFS_MAGIC, MSDOS_SUPER_MAGIC, ...), which some programs branch on. */
typedef struct {
    uint64_t magic;
    uint32_t block_size;
    uint64_t blocks;
    uint64_t blocks_free;
    uint64_t files;
    uint64_t files_free;
    uint32_t name_max;
    int read_only;
} vibeos_fs_statfs_t;

/* rename: fail with EEXIST rather than replace an existing name. */
#define VIBEOS_RENAME_NOREPLACE 0x1u

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

    /* ---- L1: every one optional, 0 or a negated errno ---- */

    /* Write at an offset into a node, growing it as needed (a gap reads as
     * zeros). Bytes written, or a negated errno. */
    long (*write_at)(void *fs, const vibeos_fs_node_t *node, uint64_t offset,
                     const void *buf, uint32_t len);
    /* Set a node's size, dropping or zero-filling. */
    int (*truncate)(void *fs, const vibeos_fs_node_t *node, uint64_t size);
    /* A new regular file with `mode`'s permission bits; -EEXIST if the name is
     * taken. `out` describes it. */
    int (*create)(void *fs, const char *path, uint32_t mode, vibeos_fs_node_t *out);
    /* A node that is not a regular file - `mode` carries its type - for a
     * filesystem that can keep one; -EPERM for a type it cannot (docs/abi/ L5:
     * a local socket's name is a socket node). Optional. */
    int (*mknod)(void *fs, const char *path, uint32_t mode, vibeos_fs_node_t *out);
    /* Remove an empty directory: -ENOTEMPTY otherwise. */
    int (*rmdir)(void *fs, const char *path);
    /* Move a name, replacing what `to` names unless NOREPLACE: a file over a
     * file, an empty directory over a directory. Both inside this filesystem. */
    int (*rename)(void *fs, const char *from, const char *to, uint32_t flags);
    /* A second name for an existing file. */
    int (*link)(void *fs, const char *existing, const char *path);
    /* A symbolic link at `path` whose contents are `target`, unresolved. */
    int (*symlink)(void *fs, const char *target, const char *path);
    /* A symbolic link's contents, not terminated; bytes copied. */
    long (*readlink)(void *fs, const char *path, char *buf, uint32_t cap);
    int (*setattr)(void *fs, const char *path, const vibeos_fs_attr_t *attr);
    /* The same two questions asked of a node rather than a name (M-080): what
     * a call on a descriptor needs, because the name the description was
     * opened by may since name another file, or nothing. -ENOENT for a node
     * that is gone. Optional; without them a descriptor's call walks its path
     * and refuses a different node with ESTALE. */
    int (*getattr)(void *fs, const vibeos_fs_node_t *node, vibeos_fs_node_t *out);
    int (*setattr_node)(void *fs, const vibeos_fs_node_t *node, const vibeos_fs_attr_t *attr);
    int (*statfs)(void *fs, vibeos_fs_statfs_t *out);
    /* Everything written is on the medium when this returns. */
    int (*sync)(void *fs);
    /* The page that holds byte `offset` of a file - a page multiple - for a
     * mapping to share (docs/abi/ L3): the filesystem's own page, made if the
     * file has a hole there, so a store through the mapping is a store into
     * the file and a write to the file is seen through the mapping. It comes
     * with a reference, taken while the filesystem could still vouch for the
     * page; the caller gives it back when its own are in place.
     *
     * 0 and the page; 1 when `offset` is at or past the end of the last page
     * that holds any of the file - there is nothing there to share; a negated
     * errno otherwise. Only a filesystem that keeps a file in pages has this:
     * one that keeps it in clusters on a disk has no page to give, and a
     * mapping of its files is private or refused. */
    int (*share_page)(void *fs, const vibeos_fs_node_t *node, uint64_t offset, void **page);
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

/* ---- L1's operations, through wrappers that answer for a missing one ----
 *
 * 0 or a negated errno. A driver without the operation gets EROFS if it
 * writes nothing at all, EPERM if it writes but cannot do this; the exceptions
 * are named at each. */

/* 1 if the filesystem can change anything at all. */
int vibeos_fs_writable(const vibeos_fsmount_t *mnt);
/* -EOPNOTSUPP from a writable filesystem without in-place writes: the caller
 * falls back to write_file, which is the difference between FAT today and a
 * refusal. */
long vibeos_fs_write_at(vibeos_fsmount_t *mnt, const vibeos_fs_node_t *node,
                         uint64_t offset, const void *buf, uint32_t len);
int vibeos_fs_truncate(vibeos_fsmount_t *mnt, const vibeos_fs_node_t *node, uint64_t size);
/* -ENODEV from a filesystem that has no pages to share. */
int vibeos_fs_share_page(vibeos_fsmount_t *mnt, const vibeos_fs_node_t *node, uint64_t offset,
                         void **page);
int vibeos_fs_create(vibeos_fsmount_t *mnt, const char *path, uint32_t mode,
                     vibeos_fs_node_t *out);
/* A node of `mode`'s type; -EPERM from a filesystem that cannot keep it. */
int vibeos_fs_mknod(vibeos_fsmount_t *mnt, const char *path, uint32_t mode, vibeos_fs_node_t *out);
int vibeos_fs_rmdir(vibeos_fsmount_t *mnt, const char *path);
int vibeos_fs_rename(vibeos_fsmount_t *mnt, const char *from, const char *to, uint32_t flags);
int vibeos_fs_link(vibeos_fsmount_t *mnt, const char *existing, const char *path);
int vibeos_fs_symlink(vibeos_fsmount_t *mnt, const char *target, const char *path);
/* -EINVAL when the filesystem has no symbolic links: nothing on it is one. */
long vibeos_fs_readlink(vibeos_fsmount_t *mnt, const char *path, char *buf, uint32_t cap);
int vibeos_fs_setattr(vibeos_fsmount_t *mnt, const char *path, const vibeos_fs_attr_t *attr);
/* By node (M-080). -ENOSYS when the filesystem cannot be asked by node, so the
 * caller can fall back to its path. `path` is only what a listener is told. */
int vibeos_fs_getattr(vibeos_fsmount_t *mnt, const vibeos_fs_node_t *node, vibeos_fs_node_t *out);
int vibeos_fs_setattr_node(vibeos_fsmount_t *mnt, const vibeos_fs_node_t *node, const char *path,
                           const vibeos_fs_attr_t *attr);
/* A driver without statfs is described from what the wrapper knows: its
 * writability and nothing else. */
int vibeos_fs_statfs(vibeos_fsmount_t *mnt, vibeos_fs_statfs_t *out);
/* 0 from a driver without sync: it has nothing held back. */
int vibeos_fs_sync(vibeos_fsmount_t *mnt);

/* The clock timestamps are read from, supplied by the architecture - a
 * registration, not a weak symbol (CLAUDE.md). Uptime until the kernel has a
 * wall clock (L2); 0 before one is registered. */
void vibeos_fs_set_clock(uint64_t (*now_ns)(void));

/* What happened to a file, said once where every filesystem's operation passes
 * (docs/abi/ L4 step 6): here for the operations on names, and in the regular
 * file type for open, read, write and close. The numbering is the file layer's;
 * it happens to be Linux's inotify numbering, which a personality maps rather
 * than assumes. One listener, registered - Linux's inotify - and nothing is
 * looked up for anybody while there is none. */
#define VIBEOS_FSN_ACCESS        0x0001u
#define VIBEOS_FSN_MODIFY        0x0002u
#define VIBEOS_FSN_ATTRIB        0x0004u
#define VIBEOS_FSN_CLOSE_WRITE   0x0008u
#define VIBEOS_FSN_CLOSE_NOWRITE 0x0010u
#define VIBEOS_FSN_OPEN          0x0020u
#define VIBEOS_FSN_MOVED_FROM    0x0040u
#define VIBEOS_FSN_MOVED_TO      0x0080u
#define VIBEOS_FSN_CREATE        0x0100u
#define VIBEOS_FSN_DELETE        0x0200u
#define VIBEOS_FSN_DELETE_SELF   0x0400u
#define VIBEOS_FSN_MOVE_SELF     0x0800u
/* The node's link count changed (a link made or removed): the node hears it,
 * its directory does not - which is how Linux tells it, apart from a chmod. */
#define VIBEOS_FSN_NLINK         0x1000u
/* `path` is inside `mnt`; `id` is the node the event is about (gone already for
 * a delete); `cookie` ties the two halves of a rename, 0 otherwise. */
typedef void (*vibeos_fs_notify_fn)(vibeos_fsmount_t *mnt, const char *path, uint64_t id, uint32_t event,
                                    uint32_t cookie, int is_dir);
void vibeos_fs_set_notify(vibeos_fs_notify_fn fn);
int vibeos_fs_notify_active(void);
void vibeos_fs_notify(vibeos_fsmount_t *mnt, const char *path, uint64_t id, uint32_t event, uint32_t cookie,
                      int is_dir);
uint64_t vibeos_fs_now_ns(void);

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

/* The table's lock, supplied by the architecture, as the pipe and frame layers'
 * are. Every function below takes it for exactly as long as it touches the
 * table.
 *
 * Until 2026-09-28 the table had no lock and did not need one by accident: every
 * caller ran during single-core bring-up. Attach appends, detach moves the last
 * entry into the hole it leaves, and resolve walks the entries - so a resolve
 * beside a detach can read an entry half copied and hand back the wrong
 * mount for a path, which is the shape of the page cache's defect in CLAUDE.md
 * ("a layer that is serialised by accident is not serialised"). A call made with
 * no lock registered is counted as VIBEOS_MBZ_MOUNT_UNLOCKED, which the boot
 * gate asserts is zero: a lock that is configured by nobody is caught rather
 * than trusted. */
void vibeos_fs_set_lock(void (*lock)(void), void (*unlock)(void));

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
