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

/* struct statx_timestamp and struct statx (linux/stat.h). Linux keeps adding
 * fields at the end, into what used to be spare, so only the part this kernel
 * fills is declared field by field; `rest` is whatever follows, up to the size
 * - which is 256 bytes and has not changed. */
typedef struct {
    int64_t tv_sec;
    uint32_t tv_nsec;
    int32_t pad;
} linux_statx_timestamp_t;

typedef struct {
    uint32_t stx_mask;
    uint32_t stx_blksize;
    uint64_t stx_attributes;
    uint32_t stx_nlink;
    uint32_t stx_uid;
    uint32_t stx_gid;
    uint16_t stx_mode;
    uint16_t spare0;
    uint64_t stx_ino;
    uint64_t stx_size;
    uint64_t stx_blocks;
    uint64_t stx_attributes_mask;
    linux_statx_timestamp_t stx_atime;
    linux_statx_timestamp_t stx_btime;
    linux_statx_timestamp_t stx_ctime;
    linux_statx_timestamp_t stx_mtime;
    uint32_t stx_rdev_major;
    uint32_t stx_rdev_minor;
    uint32_t stx_dev_major;
    uint32_t stx_dev_minor;
    uint64_t rest[14];
} linux_statx_t;

/* struct statfs (asm-generic/statfs.h): statfs and fstatfs. */
typedef struct {
    int64_t f_type;
    int64_t f_bsize;
    int64_t f_blocks;
    int64_t f_bfree;
    int64_t f_bavail;
    int64_t f_files;
    int64_t f_ffree;
    int32_t f_fsid[2];
    int64_t f_namelen;
    int64_t f_frsize;
    int64_t f_flags;
    int64_t f_spare[4];
} linux_statfs_t;

/* struct open_how (linux/openat2.h): openat2. */
typedef struct {
    uint64_t flags;
    uint64_t mode;
    uint64_t resolve;
} linux_open_how_t;

/* struct utimbuf (linux/utime.h) and struct __kernel_old_timeval
 * (linux/time_types.h): utime, and utimes and futimesat. */
typedef struct {
    int64_t actime;
    int64_t modtime;
} linux_utimbuf_t;

typedef struct {
    int64_t tv_sec;
    int64_t tv_usec;
} linux_timeval_t;

/* struct flock (asm-generic/fcntl.h): fcntl's record locks. */
typedef struct {
    int16_t l_type;
    int16_t l_whence;
    int64_t l_start;
    int64_t l_len;
    int32_t l_pid;
} linux_flock_t;

/* One getdents record, the call before getdents64: this header, the name and
 * its NUL, padding, and the type in the record's last byte. Linux exports no
 * declaration of it and no C library uses it, so the test asks the host's
 * kernel for a directory and reads the answer through this structure. */
typedef struct {
    uint64_t d_ino;
    uint64_t d_off;
    uint16_t d_reclen;
    char d_name[];
} linux_dirent_t;

/* struct termios (asm-generic/termbits.h) - the kernel's, which is shorter
 * than the C library's - and struct winsize (asm-generic/termios.h): the
 * terminal's modes and size, by ioctl. */
typedef struct {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t c_line;
    uint8_t c_cc[19];
} linux_termios_t;

typedef struct {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
} linux_winsize_t;

/* struct pollfd (asm-generic/poll.h): poll. */
typedef struct {
    int32_t fd;
    int16_t events;
    int16_t revents;
} linux_pollfd_t;

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
#define LINUX_CLONE_PIDFD          0x00001000u
#define LINUX_CLONE_INTO_CGROUP    0x200000000ull
#define LINUX_CLONE_NEWNS          0x00020000u
/* clone()'s low byte: the signal the parent is sent when the child ends. */
#define LINUX_CSIGNAL              0x000000ffu
#define LINUX_CLONE_ARGS_SIZE_VER0 64u

/* struct clone_args (linux/sched.h): clone3's arguments, version 2. */
typedef struct {
    uint64_t flags;
    uint64_t pidfd;
    uint64_t child_tid;
    uint64_t parent_tid;
    uint64_t exit_signal;
    uint64_t stack;
    uint64_t stack_size;
    uint64_t tls;
    uint64_t set_tid;
    uint64_t set_tid_size;
    uint64_t cgroup;
} linux_clone_args_t;

/* waitid (linux/wait.h): which children, and what to report. */
#define LINUX_P_ALL       0
#define LINUX_P_PID       1
#define LINUX_P_PGID      2
#define LINUX_P_PIDFD     3
#define LINUX_WSTOPPED    0x00000002u
#define LINUX_WEXITED     0x00000004u
#define LINUX_WNOWAIT     0x01000000u
/* pidfd_open's one flag (linux/pidfd.h): O_NONBLOCK. */
#define LINUX_PIDFD_NONBLOCK 0x00000800u

/* *at calls (linux/fcntl.h). AT_FDCWD arrives zero-extended: VIBEOS_ARG_INT. */
#define LINUX_AT_FDCWD       (-100)
#define LINUX_AT_SYMLINK_NOFOLLOW 0x100
#define LINUX_AT_REMOVEDIR   0x200
#define LINUX_AT_EMPTY_PATH  0x1000
#define LINUX_AT_EACCESS     0x200   /* faccessat2; the same bit as AT_REMOVEDIR */
#define LINUX_AT_SYMLINK_FOLLOW 0x400
#define LINUX_AT_NO_AUTOMOUNT 0x800
#define LINUX_AT_STATX_SYNC_TYPE 0x6000

/* access (the C library's unistd.h). */
#define LINUX_R_OK 4
#define LINUX_W_OK 2
#define LINUX_X_OK 1

/* renameat2 (linux/fs.h). */
#define LINUX_RENAME_NOREPLACE (1u << 0)
#define LINUX_RENAME_EXCHANGE  (1u << 1)
#define LINUX_RENAME_WHITEOUT  (1u << 2)

/* statx (linux/stat.h): what stat has always reported, and the bit that must
 * be clear in a mask. */
#define LINUX_STATX_BASIC_STATS 0x000007ffu
#define LINUX_STATX_RESERVED    0x80000000u

/* utimensat (the C library's sys/stat.h): "now" and "leave it", in tv_nsec. */
#define LINUX_UTIME_NOW  ((1l << 30) - 1l)
#define LINUX_UTIME_OMIT ((1l << 30) - 2l)

/* Every open flag Linux has (asm-generic/fcntl.h): openat2 refuses a bit outside
 * these, where open ignores it. Linux keeps this as VALID_OPEN_FLAGS, inside
 * the kernel; the test compares it with the flags themselves, or-ed. */
#define LINUX_OPEN_VALID 0x7fffc3u

/* openat2's resolve flags (linux/openat2.h). */
#define LINUX_RESOLVE_NO_XDEV       0x01
#define LINUX_RESOLVE_NO_MAGICLINKS 0x02
#define LINUX_RESOLVE_NO_SYMLINKS   0x04
#define LINUX_RESOLVE_BENEATH       0x08
#define LINUX_RESOLVE_IN_ROOT       0x10
#define LINUX_RESOLVE_CACHED        0x20

/* statfs's f_flags (the C library's sys/statvfs.h). */
#define LINUX_ST_RDONLY 1

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
#define LINUX_F_OFD_GETLK      36
#define LINUX_F_OFD_SETLK      37
#define LINUX_F_OFD_SETLKW     38

/* struct flock's l_type, and flock's operations (asm-generic/fcntl.h). */
#define LINUX_F_RDLCK 0
#define LINUX_F_WRLCK 1
#define LINUX_F_UNLCK 2
#define LINUX_LOCK_SH 1
#define LINUX_LOCK_EX 2
#define LINUX_LOCK_NB 4
#define LINUX_LOCK_UN 8

/* setxattr's flags and limits (linux/xattr.h, linux/limits.h). */
#define LINUX_XATTR_CREATE   1
#define LINUX_XATTR_REPLACE  2
#define LINUX_XATTR_NAME_MAX 255
#define LINUX_XATTR_SIZE_MAX 65536

/* fallocate modes (linux/falloc.h) and posix_fadvise advice (linux/fadvise.h):
 * the one mode honoured, and the largest advice there is. */
#define LINUX_FALLOC_FL_KEEP_SIZE 0x01
#define LINUX_POSIX_FADV_NOREUSE  5

/* ioctl requests (asm-generic/ioctls.h): the terminal's, and the four any
 * descriptor answers. The process-group pair is the file layer's
 * (VIBEOS_IOCTL_*). */
#define LINUX_TCGETS     0x5401u
#define LINUX_TCSETS     0x5402u
#define LINUX_TCSETSW    0x5403u
#define LINUX_TCSETSF    0x5404u
#define LINUX_TIOCGWINSZ 0x5413u
#define LINUX_TIOCSWINSZ 0x5414u
#define LINUX_FIONREAD   0x541Bu
#define LINUX_FIONBIO    0x5421u
#define LINUX_FIONCLEX   0x5450u
#define LINUX_FIOCLEX    0x5451u

/* poll's events (asm-generic/poll.h). */
#define LINUX_POLLIN   0x0001
#define LINUX_POLLOUT  0x0004
#define LINUX_POLLERR  0x0008
#define LINUX_POLLHUP  0x0010
#define LINUX_POLLNVAL 0x0020
#define LINUX_POLLRDNORM 0x0040
#define LINUX_POLLWRNORM 0x0100

/* close_range flags (linux/close_range.h). */
#define LINUX_CLOSE_RANGE_UNSHARE (1u << 1)
#define LINUX_CLOSE_RANGE_CLOEXEC (1u << 2)

/* getdents64 record types (the C library's dirent.h): the file type in the
 * mode, moved down - what the C library calls IFTODT. */
#define LINUX_DT_DIR 4u
#define LINUX_DT_REG 8u
#define LINUX_DT_LNK 10u
#define LINUX_DT_OF(mode) (((mode) & 0170000u) >> 12)

/* futex operations (linux/futex.h). */
#define LINUX_FUTEX_WAIT 0
#define LINUX_FUTEX_WAKE 1

/* mmap protection and flags (asm-generic/mman-common.h). */
#define LINUX_PROT_NONE     0x0
#define LINUX_PROT_WRITE    0x2
#define LINUX_PROT_EXEC     0x4
#define LINUX_MAP_SHARED    0x01
#define LINUX_MAP_PRIVATE   0x02
/* Not set: the word may be shared between processes, and is named by what it
 * is rather than where the caller maps it. */
#define LINUX_FUTEX_PRIVATE_FLAG 128
#define LINUX_MAP_SHARED_VALIDATE 0x03
#define LINUX_MAP_TYPE      0x0f
#define LINUX_MAP_FIXED     0x10
#define LINUX_MAP_ANONYMOUS 0x20
#define LINUX_MAP_FIXED_NOREPLACE 0x100000
#define LINUX_MAP_LOCKED    0x2000
/* The flags Linux's mmap has always accepted (its LEGACY_MAP_MASK, the huge
 * page sizes included) and MAP_FIXED_NOREPLACE. MAP_SHARED_VALIDATE exists to
 * refuse everything else; 0x200 and 0x400 are in nobody's list. */
#define LINUX_MAP_KNOWN     0xfc17f97fu
/* clock_nanosleep: the clocks one can sleep on, and "until", not "for". */
#define LINUX_CLOCK_REALTIME  0
#define LINUX_CLOCK_MONOTONIC 1
#define LINUX_CLOCK_BOOTTIME  7
#define LINUX_TIMER_ABSTIME   1

/* The rest of Linux's clocks (linux/time.h): the two CPU-time clocks, and the
 * variants of the machine's clock that differ on Linux by what they promise -
 * here they are all the one clock the timer keeps. */
#define LINUX_CLOCK_PROCESS_CPUTIME_ID 2
#define LINUX_CLOCK_THREAD_CPUTIME_ID  3
#define LINUX_CLOCK_MONOTONIC_RAW      4
#define LINUX_CLOCK_REALTIME_COARSE    5
#define LINUX_CLOCK_MONOTONIC_COARSE   6
#define LINUX_CLOCK_REALTIME_ALARM     8
#define LINUX_CLOCK_BOOTTIME_ALARM     9
#define LINUX_CLOCK_TAI                11

/* Timers (docs/abi/ L2 step 3). */
/* struct itimerval (linux/time.h): setitimer, getitimer. */
typedef struct {
    linux_timeval_t it_interval;
    linux_timeval_t it_value;
} linux_itimerval_t;

/* struct __kernel_itimerspec (linux/time_types.h): timer_settime. */
typedef struct {
    linux_timespec_t it_interval;
    linux_timespec_t it_value;
} linux_itimerspec_t;

/* struct sigevent (asm-generic/siginfo.h): how timer_create is to say so. The
 * thread id is a member of a union Linux names through a macro, so ours has
 * its own name; the rest is padding to 64 bytes. */
typedef struct {
    uint64_t sigev_value;
    int32_t sigev_signo;
    int32_t sigev_notify;
    int32_t notify_tid;
    uint8_t rest[44];
} linux_sigevent_t;

/* struct tms (linux/times.h): times(). Clock ticks, in USER_HZ. */
typedef struct {
    int64_t tms_utime;
    int64_t tms_stime;
    int64_t tms_cutime;
    int64_t tms_cstime;
} linux_tms_t;

/* struct timezone (linux/time.h): gettimeofday's second argument, always zero. */
typedef struct {
    int32_t tz_minuteswest;
    int32_t tz_dsttime;
} linux_timezone_t;

#define LINUX_ITIMER_REAL    0
#define LINUX_ITIMER_VIRTUAL 1
#define LINUX_ITIMER_PROF    2
#define LINUX_SIGEV_SIGNAL    0
#define LINUX_SIGEV_NONE      1
#define LINUX_SIGEV_THREAD    2
#define LINUX_SIGEV_THREAD_ID 4
#define LINUX_SI_TIMER        (-2)
/* madvise, mlock, mremap, memfd_create (L3 step 4). */
#define LINUX_MADV_NORMAL      0
#define LINUX_MADV_RANDOM      1
#define LINUX_MADV_SEQUENTIAL  2
#define LINUX_MADV_WILLNEED    3
#define LINUX_MADV_DONTNEED    4
#define LINUX_MADV_FREE        8
#define LINUX_MADV_DONTDUMP    16
#define LINUX_MADV_DODUMP      17
#define LINUX_MADV_COLD        20
#define LINUX_MADV_PAGEOUT     21
#define LINUX_MADV_POPULATE_READ  22
#define LINUX_MADV_POPULATE_WRITE 23
#define LINUX_MLOCK_ONFAULT    0x01
#define LINUX_MCL_CURRENT      1
#define LINUX_MCL_FUTURE       2
#define LINUX_MCL_ONFAULT      4
#define LINUX_MREMAP_MAYMOVE   1
#define LINUX_MREMAP_FIXED     2
#define LINUX_MFD_CLOEXEC       0x0001u
#define LINUX_MFD_ALLOW_SEALING 0x0002u
#define LINUX_MFD_NAME_MAX      249u    /* NAME_MAX less "memfd:" */
#define LINUX_MS_ASYNC      1
#define LINUX_MS_INVALIDATE 2
#define LINUX_MS_SYNC       4

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

/* What a handler is handed (docs/abi/ L2): the frame Linux builds on the
 * stack - a return address, a ucontext and a siginfo - is read by C libraries
 * and by programs. musl's thread cancellation rewrites the saved rip in it, and
 * a SIGSEGV handler that recovers does the same; rt_sigreturn reads it back.
 * Until L2 the frame here was private to this kernel and nothing could. */

/* stack_t (asm/signal.h): sigaltstack, and uc_stack. */
typedef struct {
    uint64_t ss_sp;
    int32_t ss_flags;
    uint64_t ss_size;
} linux_stack_t;

/* struct sigcontext (asm/sigcontext.h), x86-64. */
typedef struct {
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rdi, rsi, rbp, rbx, rdx, rax, rcx, rsp, rip, eflags;
    uint16_t cs, gs, fs, ss;
    uint64_t err, trapno, oldmask, cr2;
    uint64_t fpstate;       /* the FXSAVE area, or 0 for none */
    uint64_t reserved1[8];
} linux_sigcontext_t;

/* struct ucontext (asm/ucontext.h): the kernel's, which ends at uc_sigmask -
 * the C library's ucontext_t goes on, and nothing past here is the kernel's. */
typedef struct {
    uint64_t uc_flags;
    uint64_t uc_link;
    linux_stack_t uc_stack;
    linux_sigcontext_t uc_mcontext;
    uint64_t uc_sigmask;
} linux_ucontext_t;

/* siginfo_t (asm-generic/siginfo.h): 128 bytes, of which what a field means
 * depends on the signal and si_code. Linux's is a union; this is the one
 * arrangement of it that every field used here fits, and the two that overlay
 * others - a fault's si_addr, SIGCHLD's si_status - are written through the
 * helpers below. The fields are not called si_pid and so on because those
 * are macros in every C library's header, naming the union's members. */
typedef struct {
    int32_t si_signo;
    int32_t si_errno;
    int32_t si_code;
    int32_t pad0;        /* unnamed in Linux: the union is 8-aligned       */
    int32_t pid;         /* kill, sigqueue, SIGCHLD; si_addr's low half    */
    uint32_t uid;        /* the sender's real user; si_addr's high half    */
    uint64_t value;      /* sigqueue's value; si_status is its low half    */
    int64_t utime;       /* SIGCHLD                                        */
    int64_t stime;
    uint8_t rest[80];
} linux_siginfo_t;

static inline void linux_si_set_addr(linux_siginfo_t *o, uint64_t addr) {
    o->pid = (int32_t)(uint32_t)addr;
    o->uid = (uint32_t)(addr >> 32);
}
static inline uint64_t linux_si_addr(const linux_siginfo_t *o) {
    return (uint64_t)(uint32_t)o->pid | ((uint64_t)o->uid << 32);
}
static inline void linux_si_set_status(linux_siginfo_t *o, int32_t status) {
    o->value = (uint64_t)(uint32_t)status;
}

/* The handler's flags and the alternate stack's (asm-generic/signal-defs.h,
 * asm/signal.h, linux/signal.h). */
#define LINUX_SA_SIGINFO     0x00000004u
#define LINUX_SA_ONSTACK     0x08000000u
#define LINUX_SA_NODEFER     0x40000000u
#define LINUX_SA_RESETHAND   0x80000000u
#define LINUX_SS_ONSTACK     1
#define LINUX_SS_DISABLE     2
#define LINUX_SS_AUTODISARM  (1u << 31)
#define LINUX_MINSIGSTKSZ    2048u

/* si_code (asm-generic/siginfo.h). */
#define LINUX_SI_USER        0
#define LINUX_SI_KERNEL      0x80
#define LINUX_SI_QUEUE       (-1)
#define LINUX_SI_TKILL       (-6)
#define LINUX_SEGV_MAPERR    1
#define LINUX_SEGV_ACCERR    2
#define LINUX_BUS_ADRALN     1
#define LINUX_FPE_INTDIV     1
#define LINUX_FPE_FLTINV     7
#define LINUX_ILL_ILLOPN     2
#define LINUX_CLD_EXITED     1
#define LINUX_CLD_KILLED     2

/* uc_flags (asm/ucontext.h): the frame carries ss, and sigreturn is to put it
 * back exactly. */
#define LINUX_UC_SIGCONTEXT_SS    0x2u
#define LINUX_UC_STRICT_RESTORE_SS 0x4u

/* rt_sigprocmask (asm-generic/signal-defs.h). */
#define LINUX_SIG_BLOCK   0
#define LINUX_SIG_UNBLOCK 1
#define LINUX_SIG_SETMASK 2

/* prlimit64 resources (asm-generic/resource.h). */
#define LINUX_RLIMIT_CPU        0
#define LINUX_RLIMIT_FSIZE      1
#define LINUX_RLIMIT_DATA       2
#define LINUX_RLIMIT_STACK      3
#define LINUX_RLIMIT_CORE       4
#define LINUX_RLIMIT_RSS        5
#define LINUX_RLIMIT_NPROC      6
#define LINUX_RLIMIT_NOFILE     7
#define LINUX_RLIMIT_MEMLOCK    8
#define LINUX_RLIMIT_AS         9
#define LINUX_RLIMIT_LOCKS      10
#define LINUX_RLIMIT_SIGPENDING 11
#define LINUX_RLIMIT_MSGQUEUE   12
#define LINUX_RLIMIT_NICE       13
#define LINUX_RLIMIT_RTPRIO     14
#define LINUX_RLIMIT_RTTIME     15
#define LINUX_RLIM_NLIMITS      16
#define LINUX_RLIM64_INFINITY 0xFFFFFFFFFFFFFFFFull

/* getpriority, setpriority (linux/resource.h) and getrusage's who. */
#define LINUX_PRIO_PROCESS    0
#define LINUX_PRIO_PGRP       1
#define LINUX_PRIO_USER       2
#define LINUX_RUSAGE_SELF     0
#define LINUX_RUSAGE_CHILDREN (-1)
#define LINUX_RUSAGE_THREAD   1

/* struct rusage (linux/resource.h): two timevals and fourteen counters. */
typedef struct {
    linux_timeval_t ru_utime;
    linux_timeval_t ru_stime;
    int64_t ru_maxrss;
    int64_t ru_ixrss;
    int64_t ru_idrss;
    int64_t ru_isrss;
    int64_t ru_minflt;
    int64_t ru_majflt;
    int64_t ru_nswap;
    int64_t ru_inblock;
    int64_t ru_oublock;
    int64_t ru_msgsnd;
    int64_t ru_msgrcv;
    int64_t ru_nsignals;
    int64_t ru_nvcsw;
    int64_t ru_nivcsw;
} linux_rusage_t;

#endif
