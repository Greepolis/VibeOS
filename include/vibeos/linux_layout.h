#ifndef VIBEOS_LINUX_LAYOUT_H
#define VIBEOS_LINUX_LAYOUT_H

/* Linux's x86-64 structures and numbers, as this kernel reads and writes them
 * (docs/abi/ A5, invariant 8).
 *
 * Every structure a Linux handler copies to or from user memory is declared
 * here, and every constant a handler compares a user's argument against. They
 * used to be byte offsets and bare numbers in each handler - `rec[18]`,
 * `STAT_OFF_UID 28u`, `domain != 2u` for AF_INET - with TIOCGPGRP and AT_FDCWD
 * spelled twice, and each was exactly as right as whoever typed it.
 *
 * Nothing here is taken from Linux's headers: the kernel is freestanding and
 * builds on hosts that have none. What makes the copy trustworthy is
 * tests/kernel/linux_layout_tests.c, which compiles against the host's own
 * uapi headers and compares every field's offset and size, every structure's
 * size and every constant with Linux's. check-linux-layout.py fails the build
 * when something is declared here that the test does not compare - so a field
 * added without its check is refused rather than trusted.
 *
 * Linux's own names are used for fields, so the test reads as a comparison of
 * like with like. Padding is named and compared too: a pad that moves means a
 * field after it moved.
 *
 * Personality-neutral values that happen to be Linux's - the file layer's
 * VIBEOS_O_* flags, the errno numbers, signal numbers - stay where they are and
 * are compared by the same test. */

#include <stdint.h>

/* ---- structures --------------------------------------------------------------------- */

/* struct stat (asm/stat.h), what fstat and newfstatat write. */
typedef struct {
    uint64_t st_dev;
    uint64_t st_ino;
    uint64_t st_nlink;
    uint32_t st_mode;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t pad0;
    uint64_t st_rdev;
    int64_t st_size;
    int64_t st_blksize;
    int64_t st_blocks;
    uint64_t st_atime;
    uint64_t st_atime_nsec;
    uint64_t st_mtime;
    uint64_t st_mtime_nsec;
    uint64_t st_ctime;
    uint64_t st_ctime_nsec;
    int64_t unused[3];
} linux_stat_t;

/* struct __kernel_timespec (linux/time_types.h): clock_gettime. */
typedef struct {
    int64_t tv_sec;
    int64_t tv_nsec;
} linux_timespec_t;

/* struct new_utsname (linux/utsname.h): uname. */
typedef struct {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
} linux_utsname_t;

/* struct sysinfo (linux/sysinfo.h). Its trailing _f[] is zero bytes long on a
 * 64-bit machine, so the size is what alignment makes of mem_unit. */
typedef struct {
    int64_t uptime;
    uint64_t loads[3];
    uint64_t totalram;
    uint64_t freeram;
    uint64_t sharedram;
    uint64_t bufferram;
    uint64_t totalswap;
    uint64_t freeswap;
    uint16_t procs;
    uint16_t pad;
    uint64_t totalhigh;
    uint64_t freehigh;
    uint32_t mem_unit;
} linux_sysinfo_t;

/* struct sockaddr_in (linux/in.h). Port and address are in network order. */
typedef struct {
    uint16_t sin_family;
    uint16_t sin_port;
    uint32_t sin_addr;
    uint8_t sin_zero[8];
} linux_sockaddr_in_t;

/* struct iovec (linux/uio.h): readv and writev. */
typedef struct {
    uint64_t iov_base;
    uint64_t iov_len;
} linux_iovec_t;

/* struct rlimit64 (linux/resource.h): prlimit64. */
typedef struct {
    uint64_t rlim_cur;
    uint64_t rlim_max;
} linux_rlimit64_t;

/* The kernel's struct sigaction (asm/signal.h on x86-64), which is not the C
 * library's: handler, flags, restorer, then an 8-byte mask. rt_sigaction. */
typedef struct {
    uint64_t sa_handler;
    uint64_t sa_flags;
    uint64_t sa_restorer;
    uint64_t sa_mask;
} linux_sigaction_t;

/* One getdents64 record: this header, the name and its NUL, padded to 8.
 * Linux does not export struct linux_dirent64, so the test compares it with the
 * C library's struct dirent64, which readdir hands out as the kernel wrote it. */
typedef struct {
    uint64_t d_ino;
    int64_t d_off;
    uint16_t d_reclen;
    uint8_t d_type;
    char d_name[];
} linux_dirent64_t;

/* ---- constants ---------------------------------------------------------------------- */

/* clone() flags that decide whether it is a fork or a thread (linux/sched.h). */
#define LINUX_CLONE_VM             0x00000100u
#define LINUX_CLONE_FS             0x00000200u
#define LINUX_CLONE_FILES          0x00000400u
#define LINUX_CLONE_SIGHAND        0x00000800u
#define LINUX_CLONE_THREAD         0x00010000u
#define LINUX_CLONE_SYSVSEM        0x00040000u
#define LINUX_CLONE_SETTLS         0x00080000u
#define LINUX_CLONE_PARENT_SETTID  0x00100000u
#define LINUX_CLONE_CHILD_CLEARTID 0x00200000u
#define LINUX_CLONE_CHILD_SETTID   0x01000000u

/* *at calls (linux/fcntl.h). AT_FDCWD arrives zero-extended: VIBEOS_ARG_INT. */
#define LINUX_AT_FDCWD       (-100)
#define LINUX_AT_REMOVEDIR   0x200
#define LINUX_AT_EMPTY_PATH  0x1000

/* fcntl commands (asm-generic/fcntl.h, linux/fcntl.h). */
#define LINUX_F_DUPFD          0
#define LINUX_F_GETFD          1
#define LINUX_F_SETFD          2
#define LINUX_F_GETFL          3
#define LINUX_F_SETFL          4
#define LINUX_F_GETLK          5
#define LINUX_F_SETLK          6
#define LINUX_F_SETLKW         7
#define LINUX_F_DUPFD_CLOEXEC  1030

/* close_range flags (linux/close_range.h). */
#define LINUX_CLOSE_RANGE_UNSHARE (1u << 1)
#define LINUX_CLOSE_RANGE_CLOEXEC (1u << 2)

/* getdents64 record types (the C library's dirent.h). */
#define LINUX_DT_DIR 4u
#define LINUX_DT_REG 8u

/* futex operations (linux/futex.h). */
#define LINUX_FUTEX_WAIT 0
#define LINUX_FUTEX_WAKE 1

/* mmap protection and flags (asm-generic/mman-common.h). */
#define LINUX_PROT_NONE     0x0
#define LINUX_PROT_WRITE    0x2
#define LINUX_PROT_EXEC     0x4
#define LINUX_MAP_FIXED     0x10
#define LINUX_MAP_ANONYMOUS 0x20

/* prctl and arch_prctl (linux/prctl.h, asm/prctl.h). */
#define LINUX_PR_SET_NAME 15
#define LINUX_PR_GET_NAME 16
#define LINUX_ARCH_SET_GS 0x1001
#define LINUX_ARCH_SET_FS 0x1002
#define LINUX_ARCH_GET_FS 0x1003
#define LINUX_ARCH_GET_GS 0x1004

/* Sockets (the C library's sys/socket.h: uapi does not carry these). */
#define LINUX_AF_INET     2u
#define LINUX_SOCK_STREAM 1u
#define LINUX_SOCK_DGRAM  2u

/* wait4 options (linux/wait.h). The last three are Linux's __WNOTHREAD,
 * __WALL and __WCLONE; a leading double underscore is the C library's. */
#define LINUX_WNOHANG    0x00000001u
#define LINUX_WUNTRACED  0x00000002u
#define LINUX_WCONTINUED 0x00000008u
#define LINUX_WNOTHREAD  0x20000000u
#define LINUX_WALL       0x40000000u
#define LINUX_WCLONE     0x80000000u

/* rt_sigprocmask (asm-generic/signal-defs.h). */
#define LINUX_SIG_BLOCK   0
#define LINUX_SIG_UNBLOCK 1
#define LINUX_SIG_SETMASK 2

/* prlimit64 resources (asm-generic/resource.h). */
#define LINUX_RLIMIT_STACK  3
#define LINUX_RLIMIT_NOFILE 7
#define LINUX_RLIM64_INFINITY 0xFFFFFFFFFFFFFFFFull

#endif
