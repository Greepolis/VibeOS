/* Linux's structures and numbers against Linux's own headers (docs/abi/ A5,
 * invariant 8).
 *
 * The kernel is freestanding and declares Linux's layouts itself
 * (vibeos/linux_layout.h); the errno values, signal numbers and open flags are
 * the kernel's own, numbered as Linux's (abi_linux.h, file.h). A copy of an ABI
 * is exactly as right as whoever typed it, and a host test written by the same
 * hand can only agree with it - CLAUDE.md's rule about fixtures this project
 * writes. So this file compiles against the host's uapi headers, the one
 * artefact neither side controls, and compares: every field's offset and size,
 * every structure's size, every constant's value.
 *
 * check-linux-layout.py fails the build when the header declares a field or a
 * constant that is not named here, so the comparison cannot fall behind the
 * declarations.
 *
 * Only on a Linux host: elsewhere (the Windows job) there are no uapi headers,
 * and the test says it was skipped. On Linux the includes are unconditional, so
 * a runner without them fails to build rather than passing having compared
 * nothing. */

#include <stdio.h>
#include <stddef.h>

int test_linux_layout(void);

#if !defined(__linux__)

int test_linux_layout(void) {
    printf("linux_layout: skipped - no Linux uapi headers on this host\n");
    return 0;
}

#else

#include <asm/stat.h>
#include <asm/signal.h>
#include <asm/sigcontext.h>
#include <asm/ucontext.h>
#include <asm/siginfo.h>
#include <linux/signal.h>
#include <linux/times.h>
#include <linux/pidfd.h>
#include <linux/random.h>
#include <asm/ioctls.h>
#include <asm/termbits.h>
#include <asm/termios.h>
#include <asm/mman.h>
#include <linux/mman.h>
#include <linux/memfd.h>
#include <linux/limits.h>
#include <asm/prctl.h>
#include <linux/time_types.h>
#include <linux/time.h>
#include <linux/utsname.h>
#include <linux/sysinfo.h>
#include <linux/in.h>
#include <linux/uio.h>
#include <linux/resource.h>
#include <linux/errno.h>
#include <linux/fcntl.h>
#include <linux/fs.h>
#include <linux/futex.h>
#include <linux/prctl.h>
#include <linux/sched.h>
#include <linux/wait.h>
#include <linux/close_range.h>
#include <linux/falloc.h>
#include <linux/fadvise.h>
#include <linux/stat.h>
#include <linux/openat2.h>
#include <linux/utime.h>
#include <asm/statfs.h>
#include <asm/poll.h>
#include <linux/xattr.h>
#include <linux/limits.h>
#include <string.h>

#include "vibeos/linux_layout.h"
#include "vibeos/abi_linux.h"
#include "vibeos/file.h"
#include "vibeos/fdtable.h"
#include "vibeos/fileops.h"
#include "vibeos/tty.h"
#include "vibeos/procstate.h"
#include "linux_layout_libc.h"

static int g_fail;
static int g_checked;

static void expect(int ok, const char *what) {
    g_checked++;
    if (!ok) {
        printf("FAIL:linux_layout %s\n", what);
        g_fail = 1;
    }
}

#define SIZE(ours, theirs) \
    expect(sizeof(ours) == sizeof(theirs), "sizeof " #ours " is sizeof " #theirs)

/* A field of ours against Linux's, under Linux's name - or, for padding whose
 * Linux name is reserved to the implementation, under that name. */
#define FIELD2(ours, theirs, f, tf) do { \
        expect(offsetof(ours, f) == offsetof(theirs, tf), \
               #ours "." #f " is at " #theirs "." #tf "'s offset"); \
        expect(sizeof(((ours *)0)->f) == sizeof(((theirs *)0)->tf), \
               #ours "." #f " is " #theirs "." #tf "'s size"); \
    } while (0)
#define FIELD(ours, theirs, f) FIELD2(ours, theirs, f, f)

/* Padding Linux leaves without a name: ours ends exactly where Linux's next
 * field starts. */
#define PAD(ours, f, theirs, next) \
    expect(offsetof(ours, f) + sizeof(((ours *)0)->f) == offsetof(theirs, next), \
           #ours "." #f " ends where " #theirs "." #next " starts")

/* Our last field is everything after the fields we name: it ends where Linux's
 * structure ends, whatever Linux has since put inside it. */
#define TAIL(ours, theirs, f) \
    expect(offsetof(ours, f) + sizeof(((ours *)0)->f) == sizeof(theirs), \
           #ours "." #f " runs to the end of " #theirs)

#define CONST(ours, theirs) \
    expect((long long)(ours) == (long long)(theirs), #ours " is " #theirs)

/* The C library's answer, for what uapi does not export. */
static void libc_field(size_t off, size_t size, int has_size, const char *field,
                       const char *what) {
    size_t loff = 0, lsize = 0;
    int ok = linux_libc_dirent64(field, &loff, &lsize) == 0 && off == loff &&
             (!has_size || size == lsize);
    expect(ok, what);
}
#define LIBC_FIELD(ours, f) \
    libc_field(offsetof(ours, f), sizeof(((ours *)0)->f), 1, #f, #ours "." #f " is struct dirent64's")
#define LIBC_OFFSET(ours, f) \
    libc_field(offsetof(ours, f), 0, 0, #f, #ours "." #f " starts where struct dirent64's does")

static void libc_const(long long ours, const char *theirs, const char *what) {
    long long v = 0;
    expect(linux_libc_const(theirs, &v) == 0 && v == ours, what);
}
#define LIBC_CONST(ours, theirs) libc_const((long long)(ours), theirs, #ours " is the C library's " theirs)

/* struct linux_dirent, the record of the getdents call before getdents64, is
 * declared by nobody: Linux keeps it to itself and no C library has used the
 * call in years. So the host's kernel is asked to list "/" with it and the
 * answer is read through our structure. If a field of ours were misplaced the
 * records would not chain - each one's length is where the next begins - and
 * "." and ".." would not be found with an inode and a directory's type in the
 * record's last byte. */
static int host_dirent_agrees(void) {
    static unsigned char buf[8192];
    long n = linux_host_getdents(buf, sizeof(buf)), off = 0;
    int dot = 0, dotdot = 0;

    if (n <= 0) {
        return 0;
    }
    while (off < n) {
        const linux_dirent_t *d = (const linux_dirent_t *)(const void *)(buf + off);
        size_t max, len;
        unsigned char type;

        if (d->d_reclen < offsetof(linux_dirent_t, d_name) + 2u || (d->d_reclen & 7u) != 0u ||
            off + (long)d->d_reclen > n) {
            return 0;
        }
        max = d->d_reclen - offsetof(linux_dirent_t, d_name) - 1u;
        for (len = 0; len < max && d->d_name[len]; len++) {
        }
        type = buf[off + d->d_reclen - 1];
        if (len == max || d->d_ino == 0u || d->d_off == 0u) {
            return 0;
        }
        if (strcmp(d->d_name, ".") == 0 && (type == 4u || type == 0u)) {
            dot = 1;
        }
        if (strcmp(d->d_name, "..") == 0 && (type == 4u || type == 0u)) {
            dotdot = 1;
        }
        off += d->d_reclen;
    }
    return dot && dotdot;
}
#define HOST_FIELD(ours, f) \
    expect(host_agrees, #ours "." #f " is where the host's kernel puts it")

int test_linux_layout(void) {
    int host_agrees = host_dirent_agrees();
    g_fail = 0;
    g_checked = 0;

    /* ---- structures ---- */
    /* what stat, fstat and newfstatat fill */
    SIZE(linux_stat_t, struct stat);
    FIELD(linux_stat_t, struct stat, st_dev);
    FIELD(linux_stat_t, struct stat, st_ino);
    FIELD(linux_stat_t, struct stat, st_nlink);
    FIELD(linux_stat_t, struct stat, st_mode);
    FIELD(linux_stat_t, struct stat, st_uid);
    FIELD(linux_stat_t, struct stat, st_gid);
    FIELD2(linux_stat_t, struct stat, pad0, __pad0);
    FIELD(linux_stat_t, struct stat, st_rdev);
    FIELD(linux_stat_t, struct stat, st_size);
    FIELD(linux_stat_t, struct stat, st_blksize);
    FIELD(linux_stat_t, struct stat, st_blocks);
    FIELD(linux_stat_t, struct stat, st_atime);
    FIELD(linux_stat_t, struct stat, st_atime_nsec);
    FIELD(linux_stat_t, struct stat, st_mtime);
    FIELD(linux_stat_t, struct stat, st_mtime_nsec);
    FIELD(linux_stat_t, struct stat, st_ctime);
    FIELD(linux_stat_t, struct stat, st_ctime_nsec);
    FIELD2(linux_stat_t, struct stat, unused, __unused);

    /* clock_gettime, nanosleep, utimensat */
    SIZE(linux_timespec_t, struct __kernel_timespec);
    FIELD(linux_timespec_t, struct __kernel_timespec, tv_sec);
    FIELD(linux_timespec_t, struct __kernel_timespec, tv_nsec);

    /* uname */
    SIZE(linux_utsname_t, struct new_utsname);
    FIELD(linux_utsname_t, struct new_utsname, sysname);
    FIELD(linux_utsname_t, struct new_utsname, nodename);
    FIELD(linux_utsname_t, struct new_utsname, release);
    FIELD(linux_utsname_t, struct new_utsname, version);
    FIELD(linux_utsname_t, struct new_utsname, machine);
    FIELD(linux_utsname_t, struct new_utsname, domainname);

    /* sysinfo */
    SIZE(linux_sysinfo_t, struct sysinfo);
    FIELD(linux_sysinfo_t, struct sysinfo, uptime);
    FIELD(linux_sysinfo_t, struct sysinfo, loads);
    FIELD(linux_sysinfo_t, struct sysinfo, totalram);
    FIELD(linux_sysinfo_t, struct sysinfo, freeram);
    FIELD(linux_sysinfo_t, struct sysinfo, sharedram);
    FIELD(linux_sysinfo_t, struct sysinfo, bufferram);
    FIELD(linux_sysinfo_t, struct sysinfo, totalswap);
    FIELD(linux_sysinfo_t, struct sysinfo, freeswap);
    FIELD(linux_sysinfo_t, struct sysinfo, procs);
    FIELD(linux_sysinfo_t, struct sysinfo, pad);
    FIELD(linux_sysinfo_t, struct sysinfo, totalhigh);
    FIELD(linux_sysinfo_t, struct sysinfo, freehigh);
    FIELD(linux_sysinfo_t, struct sysinfo, mem_unit);

    /* bind, connect, accept and the rest of the socket calls */
    SIZE(linux_sockaddr_in_t, struct sockaddr_in);
    FIELD(linux_sockaddr_in_t, struct sockaddr_in, sin_family);
    FIELD(linux_sockaddr_in_t, struct sockaddr_in, sin_port);
    FIELD(linux_sockaddr_in_t, struct sockaddr_in, sin_addr);
    FIELD(linux_sockaddr_in_t, struct sockaddr_in, sin_zero);

    /* readv and writev */
    SIZE(linux_iovec_t, struct iovec);
    FIELD(linux_iovec_t, struct iovec, iov_base);
    FIELD(linux_iovec_t, struct iovec, iov_len);

    /* a time inside statx's answer */
    SIZE(linux_statx_timestamp_t, struct statx_timestamp);
    FIELD(linux_statx_timestamp_t, struct statx_timestamp, tv_sec);
    FIELD(linux_statx_timestamp_t, struct statx_timestamp, tv_nsec);
    FIELD2(linux_statx_timestamp_t, struct statx_timestamp, pad, __reserved);

    /* statx */
    SIZE(linux_statx_t, struct statx);
    FIELD(linux_statx_t, struct statx, stx_mask);
    FIELD(linux_statx_t, struct statx, stx_blksize);
    FIELD(linux_statx_t, struct statx, stx_attributes);
    FIELD(linux_statx_t, struct statx, stx_nlink);
    FIELD(linux_statx_t, struct statx, stx_uid);
    FIELD(linux_statx_t, struct statx, stx_gid);
    FIELD(linux_statx_t, struct statx, stx_mode);
    FIELD2(linux_statx_t, struct statx, spare0, __spare0);
    FIELD(linux_statx_t, struct statx, stx_ino);
    FIELD(linux_statx_t, struct statx, stx_size);
    FIELD(linux_statx_t, struct statx, stx_blocks);
    FIELD(linux_statx_t, struct statx, stx_attributes_mask);
    FIELD(linux_statx_t, struct statx, stx_atime);
    FIELD(linux_statx_t, struct statx, stx_btime);
    FIELD(linux_statx_t, struct statx, stx_ctime);
    FIELD(linux_statx_t, struct statx, stx_mtime);
    FIELD(linux_statx_t, struct statx, stx_rdev_major);
    FIELD(linux_statx_t, struct statx, stx_rdev_minor);
    FIELD(linux_statx_t, struct statx, stx_dev_major);
    FIELD(linux_statx_t, struct statx, stx_dev_minor);
    TAIL(linux_statx_t, struct statx, rest);

    /* statfs and fstatfs */
    SIZE(linux_statfs_t, struct statfs);
    FIELD(linux_statfs_t, struct statfs, f_type);
    FIELD(linux_statfs_t, struct statfs, f_bsize);
    FIELD(linux_statfs_t, struct statfs, f_blocks);
    FIELD(linux_statfs_t, struct statfs, f_bfree);
    FIELD(linux_statfs_t, struct statfs, f_bavail);
    FIELD(linux_statfs_t, struct statfs, f_files);
    FIELD(linux_statfs_t, struct statfs, f_ffree);
    FIELD(linux_statfs_t, struct statfs, f_fsid);
    FIELD(linux_statfs_t, struct statfs, f_namelen);
    FIELD(linux_statfs_t, struct statfs, f_frsize);
    FIELD(linux_statfs_t, struct statfs, f_flags);
    FIELD(linux_statfs_t, struct statfs, f_spare);

    /* openat2's argument */
    SIZE(linux_open_how_t, struct open_how);
    FIELD(linux_open_how_t, struct open_how, flags);
    FIELD(linux_open_how_t, struct open_how, mode);
    FIELD(linux_open_how_t, struct open_how, resolve);

    /* utime */
    SIZE(linux_utimbuf_t, struct utimbuf);
    FIELD(linux_utimbuf_t, struct utimbuf, actime);
    FIELD(linux_utimbuf_t, struct utimbuf, modtime);

    /* utimes, futimesat, gettimeofday */
    SIZE(linux_timeval_t, struct __kernel_old_timeval);
    FIELD(linux_timeval_t, struct __kernel_old_timeval, tv_sec);
    FIELD(linux_timeval_t, struct __kernel_old_timeval, tv_usec);

    CONST(LINUX_AT_EACCESS, AT_EACCESS);
    CONST(LINUX_AT_SYMLINK_FOLLOW, AT_SYMLINK_FOLLOW);
    CONST(LINUX_AT_NO_AUTOMOUNT, AT_NO_AUTOMOUNT);
    CONST(LINUX_AT_STATX_SYNC_TYPE, AT_STATX_SYNC_TYPE);
    LIBC_CONST(LINUX_R_OK, "R_OK");
    LIBC_CONST(LINUX_W_OK, "W_OK");
    LIBC_CONST(LINUX_X_OK, "X_OK");
    CONST(LINUX_RENAME_NOREPLACE, RENAME_NOREPLACE);
    CONST(LINUX_RENAME_EXCHANGE, RENAME_EXCHANGE);
    CONST(LINUX_RENAME_WHITEOUT, RENAME_WHITEOUT);
    CONST(LINUX_STATX_BASIC_STATS, STATX_BASIC_STATS);
    CONST(LINUX_STATX_RESERVED, STATX__RESERVED);
    LIBC_CONST(LINUX_UTIME_NOW, "UTIME_NOW");
    LIBC_CONST(LINUX_UTIME_OMIT, "UTIME_OMIT");
    CONST(LINUX_OPEN_VALID, O_ACCMODE | O_CREAT | O_EXCL | O_NOCTTY | O_TRUNC | O_APPEND | O_NONBLOCK |
                            O_DSYNC | FASYNC | O_DIRECT | O_LARGEFILE | O_DIRECTORY | O_NOFOLLOW |
                            O_NOATIME | O_CLOEXEC | O_SYNC | O_PATH | O_TMPFILE);
    CONST(LINUX_RESOLVE_NO_XDEV, RESOLVE_NO_XDEV);
    CONST(LINUX_RESOLVE_NO_MAGICLINKS, RESOLVE_NO_MAGICLINKS);
    CONST(LINUX_RESOLVE_NO_SYMLINKS, RESOLVE_NO_SYMLINKS);
    CONST(LINUX_RESOLVE_BENEATH, RESOLVE_BENEATH);
    CONST(LINUX_RESOLVE_IN_ROOT, RESOLVE_IN_ROOT);
    CONST(LINUX_RESOLVE_CACHED, RESOLVE_CACHED);
    LIBC_CONST(LINUX_ST_RDONLY, "ST_RDONLY");
    LIBC_CONST(VIBEOS_S_ISUID, "S_ISUID");
    LIBC_CONST(VIBEOS_S_ISGID, "S_ISGID");
    LIBC_CONST(VIBEOS_S_ISVTX, "S_ISVTX");

    /* fcntl's record locks */
    SIZE(linux_flock_t, struct flock);
    FIELD(linux_flock_t, struct flock, l_type);
    FIELD(linux_flock_t, struct flock, l_whence);
    FIELD(linux_flock_t, struct flock, l_start);
    FIELD(linux_flock_t, struct flock, l_len);
    FIELD(linux_flock_t, struct flock, l_pid);

    HOST_FIELD(linux_dirent_t, d_ino);
    HOST_FIELD(linux_dirent_t, d_off);
    HOST_FIELD(linux_dirent_t, d_reclen);
    HOST_FIELD(linux_dirent_t, d_name);

    CONST(LINUX_F_OFD_GETLK, F_OFD_GETLK);
    CONST(LINUX_F_OFD_SETLK, F_OFD_SETLK);
    CONST(LINUX_F_OFD_SETLKW, F_OFD_SETLKW);
    CONST(LINUX_F_RDLCK, F_RDLCK);
    CONST(LINUX_F_WRLCK, F_WRLCK);
    CONST(LINUX_F_UNLCK, F_UNLCK);
    CONST(LINUX_LOCK_SH, LOCK_SH);
    CONST(LINUX_LOCK_EX, LOCK_EX);
    CONST(LINUX_LOCK_NB, LOCK_NB);
    CONST(LINUX_LOCK_UN, LOCK_UN);
    CONST(LINUX_XATTR_CREATE, XATTR_CREATE);
    CONST(LINUX_XATTR_REPLACE, XATTR_REPLACE);
    CONST(LINUX_XATTR_NAME_MAX, XATTR_NAME_MAX);
    CONST(LINUX_XATTR_SIZE_MAX, XATTR_SIZE_MAX);
    LIBC_CONST(LINUX_DT_LNK, "DT_LNK");
    CONST(VIBEOS_EDEADLK, EDEADLK);
    CONST(VIBEOS_EOVERFLOW, EOVERFLOW);
    CONST(VIBEOS_ESTALE, ESTALE);
    CONST(VIBEOS_ETIMEDOUT, ETIMEDOUT);
    CONST(VIBEOS_EAFNOSUPPORT, EAFNOSUPPORT);

    CONST(LINUX_MAP_SHARED, MAP_SHARED);
    CONST(LINUX_MAP_PRIVATE, MAP_PRIVATE);
    CONST(LINUX_MAP_SHARED_VALIDATE, MAP_SHARED_VALIDATE);
    CONST(LINUX_MAP_TYPE, MAP_TYPE);
    CONST(LINUX_MAP_FIXED_NOREPLACE, MAP_FIXED_NOREPLACE);
    CONST(LINUX_MAP_KNOWN, MAP_TYPE | MAP_FIXED | MAP_ANONYMOUS | MAP_32BIT | MAP_GROWSDOWN | MAP_DENYWRITE |
                           MAP_EXECUTABLE | MAP_LOCKED | MAP_NORESERVE | MAP_POPULATE | MAP_NONBLOCK |
                           MAP_STACK | MAP_HUGETLB | MAP_FIXED_NOREPLACE | MAP_UNINITIALIZED |
                           (0x3fu << 26 /* MAP_HUGE_SHIFT: the size, six bits */));
    CONST(LINUX_MAP_LOCKED, MAP_LOCKED);
    CONST(LINUX_CLOCK_REALTIME, CLOCK_REALTIME);
    CONST(LINUX_CLOCK_MONOTONIC, CLOCK_MONOTONIC);
    CONST(LINUX_CLOCK_BOOTTIME, CLOCK_BOOTTIME);
    CONST(LINUX_TIMER_ABSTIME, TIMER_ABSTIME);
    CONST(LINUX_CLOCK_PROCESS_CPUTIME_ID, CLOCK_PROCESS_CPUTIME_ID);
    CONST(LINUX_CLOCK_THREAD_CPUTIME_ID, CLOCK_THREAD_CPUTIME_ID);
    CONST(LINUX_CLOCK_MONOTONIC_RAW, CLOCK_MONOTONIC_RAW);
    CONST(LINUX_CLOCK_REALTIME_COARSE, CLOCK_REALTIME_COARSE);
    CONST(LINUX_CLOCK_MONOTONIC_COARSE, CLOCK_MONOTONIC_COARSE);
    CONST(LINUX_CLOCK_REALTIME_ALARM, CLOCK_REALTIME_ALARM);
    CONST(LINUX_CLOCK_BOOTTIME_ALARM, CLOCK_BOOTTIME_ALARM);
    CONST(LINUX_CLOCK_TAI, CLOCK_TAI);

    /* Timers (L2 step 3). */
    SIZE(linux_itimerval_t, struct itimerval);
    FIELD(linux_itimerval_t, struct itimerval, it_interval);
    FIELD(linux_itimerval_t, struct itimerval, it_value);
    SIZE(linux_itimerspec_t, struct __kernel_itimerspec);
    FIELD(linux_itimerspec_t, struct __kernel_itimerspec, it_interval);
    FIELD(linux_itimerspec_t, struct __kernel_itimerspec, it_value);
    SIZE(linux_sigevent_t, struct sigevent);
    FIELD(linux_sigevent_t, struct sigevent, sigev_value);
    FIELD(linux_sigevent_t, struct sigevent, sigev_signo);
    FIELD(linux_sigevent_t, struct sigevent, sigev_notify);
    FIELD2(linux_sigevent_t, struct sigevent, notify_tid, sigev_notify_thread_id);
    TAIL(linux_sigevent_t, struct sigevent, rest);
    SIZE(linux_tms_t, struct tms);
    FIELD(linux_tms_t, struct tms, tms_utime);
    FIELD(linux_tms_t, struct tms, tms_stime);
    FIELD(linux_tms_t, struct tms, tms_cutime);
    FIELD(linux_tms_t, struct tms, tms_cstime);
    SIZE(linux_timezone_t, struct timezone);
    FIELD(linux_timezone_t, struct timezone, tz_minuteswest);
    FIELD(linux_timezone_t, struct timezone, tz_dsttime);
    CONST(LINUX_ITIMER_REAL, ITIMER_REAL);
    CONST(LINUX_ITIMER_VIRTUAL, ITIMER_VIRTUAL);
    CONST(LINUX_ITIMER_PROF, ITIMER_PROF);
    CONST(LINUX_SIGEV_SIGNAL, SIGEV_SIGNAL);
    CONST(LINUX_SIGEV_NONE, SIGEV_NONE);
    CONST(LINUX_SIGEV_THREAD, SIGEV_THREAD);
    CONST(LINUX_SIGEV_THREAD_ID, SIGEV_THREAD_ID);
    CONST(LINUX_SI_TIMER, SI_TIMER);
    CONST(VIBEOS_SIGVTALRM, SIGVTALRM);
    CONST(VIBEOS_SIGPROF, SIGPROF);
    CONST(LINUX_MADV_NORMAL, MADV_NORMAL);
    CONST(LINUX_MADV_RANDOM, MADV_RANDOM);
    CONST(LINUX_MADV_SEQUENTIAL, MADV_SEQUENTIAL);
    CONST(LINUX_MADV_WILLNEED, MADV_WILLNEED);
    CONST(LINUX_MADV_DONTNEED, MADV_DONTNEED);
    CONST(LINUX_MADV_FREE, MADV_FREE);
    CONST(LINUX_MADV_DONTDUMP, MADV_DONTDUMP);
    CONST(LINUX_MADV_DODUMP, MADV_DODUMP);
    CONST(LINUX_MADV_COLD, MADV_COLD);
    CONST(LINUX_MADV_PAGEOUT, MADV_PAGEOUT);
    CONST(LINUX_MADV_POPULATE_READ, MADV_POPULATE_READ);
    CONST(LINUX_MADV_POPULATE_WRITE, MADV_POPULATE_WRITE);
    CONST(LINUX_MLOCK_ONFAULT, MLOCK_ONFAULT);
    CONST(LINUX_MCL_CURRENT, MCL_CURRENT);
    CONST(LINUX_MCL_FUTURE, MCL_FUTURE);
    CONST(LINUX_MCL_ONFAULT, MCL_ONFAULT);
    CONST(LINUX_MREMAP_MAYMOVE, MREMAP_MAYMOVE);
    CONST(LINUX_MREMAP_FIXED, MREMAP_FIXED);
    CONST(LINUX_MFD_CLOEXEC, MFD_CLOEXEC);
    CONST(LINUX_MFD_ALLOW_SEALING, MFD_ALLOW_SEALING);
    CONST(LINUX_MFD_NAME_MAX, NAME_MAX - 6);
    CONST(LINUX_MS_ASYNC, MS_ASYNC);
    CONST(LINUX_MS_INVALIDATE, MS_INVALIDATE);
    CONST(LINUX_MS_SYNC, MS_SYNC);
    CONST(VIBEOS_ENODEV, ENODEV);

    SIZE(linux_pollfd_t, struct pollfd);
    FIELD(linux_pollfd_t, struct pollfd, fd);
    FIELD(linux_pollfd_t, struct pollfd, events);
    FIELD(linux_pollfd_t, struct pollfd, revents);
    CONST(LINUX_POLLIN, POLLIN);
    CONST(LINUX_POLLOUT, POLLOUT);
    CONST(LINUX_POLLERR, POLLERR);
    CONST(LINUX_POLLHUP, POLLHUP);
    CONST(LINUX_POLLNVAL, POLLNVAL);
    CONST(LINUX_POLLRDNORM, POLLRDNORM);
    CONST(LINUX_POLLWRNORM, POLLWRNORM);

    SIZE(linux_termios_t, struct termios);
    FIELD(linux_termios_t, struct termios, c_iflag);
    FIELD(linux_termios_t, struct termios, c_oflag);
    FIELD(linux_termios_t, struct termios, c_cflag);
    FIELD(linux_termios_t, struct termios, c_lflag);
    FIELD(linux_termios_t, struct termios, c_line);
    FIELD(linux_termios_t, struct termios, c_cc);
    SIZE(linux_winsize_t, struct winsize);
    FIELD(linux_winsize_t, struct winsize, ws_row);
    FIELD(linux_winsize_t, struct winsize, ws_col);
    FIELD(linux_winsize_t, struct winsize, ws_xpixel);
    FIELD(linux_winsize_t, struct winsize, ws_ypixel);

    CONST(LINUX_TCGETS, TCGETS);
    CONST(LINUX_TCSETS, TCSETS);
    CONST(LINUX_TCSETSW, TCSETSW);
    CONST(LINUX_TCSETSF, TCSETSF);
    CONST(LINUX_TIOCGWINSZ, TIOCGWINSZ);
    CONST(LINUX_TIOCSWINSZ, TIOCSWINSZ);
    CONST(LINUX_FIONREAD, FIONREAD);
    CONST(LINUX_FIONBIO, FIONBIO);
    CONST(LINUX_FIONCLEX, FIONCLEX);
    CONST(LINUX_FIOCLEX, FIOCLEX);
    /* The terminal's modes are kept in Linux's numbering (vibeos/tty.h). */
    CONST(VIBEOS_TTY_NCC, NCCS);
    CONST(VIBEOS_TTY_ICRNL, ICRNL);
    CONST(VIBEOS_TTY_IXON, IXON);
    CONST(VIBEOS_TTY_OPOST, OPOST);
    CONST(VIBEOS_TTY_ONLCR, ONLCR);
    CONST(VIBEOS_TTY_CFLAG_DEFAULT, B38400 | CS8 | CREAD);
    CONST(VIBEOS_TTY_ISIG, ISIG);
    CONST(VIBEOS_TTY_ICANON, ICANON);
    CONST(VIBEOS_TTY_ECHO, ECHO);
    CONST(VIBEOS_TTY_ECHOE, ECHOE);
    CONST(VIBEOS_TTY_ECHOK, ECHOK);
    CONST(VIBEOS_TTY_ECHONL, ECHONL);
    CONST(VIBEOS_TTY_ECHOCTL, ECHOCTL);
    CONST(VIBEOS_TTY_ECHOKE, ECHOKE);
    CONST(VIBEOS_TTY_IEXTEN, IEXTEN);
    CONST(VIBEOS_TTY_VINTR, VINTR);
    CONST(VIBEOS_TTY_VQUIT, VQUIT);
    CONST(VIBEOS_TTY_VERASE, VERASE);
    CONST(VIBEOS_TTY_VKILL, VKILL);
    CONST(VIBEOS_TTY_VEOF, VEOF);
    CONST(VIBEOS_TTY_VTIME, VTIME);
    CONST(VIBEOS_TTY_VMIN, VMIN);
    SIZE(vibeos_tty_modes_t, struct termios);

    SIZE(linux_rlimit64_t, struct rlimit64);
    FIELD(linux_rlimit64_t, struct rlimit64, rlim_cur);
    FIELD(linux_rlimit64_t, struct rlimit64, rlim_max);

    SIZE(linux_sigaction_t, struct sigaction);
    FIELD(linux_sigaction_t, struct sigaction, sa_handler);
    FIELD(linux_sigaction_t, struct sigaction, sa_flags);
    FIELD(linux_sigaction_t, struct sigaction, sa_restorer);
    FIELD(linux_sigaction_t, struct sigaction, sa_mask);

    /* The signal frame (L2). */
    SIZE(linux_stack_t, stack_t);
    FIELD(linux_stack_t, stack_t, ss_sp);
    FIELD(linux_stack_t, stack_t, ss_flags);
    FIELD(linux_stack_t, stack_t, ss_size);
    SIZE(linux_sigcontext_t, struct sigcontext);
    FIELD(linux_sigcontext_t, struct sigcontext, r8);
    FIELD(linux_sigcontext_t, struct sigcontext, r15);
    FIELD(linux_sigcontext_t, struct sigcontext, rdi);
    FIELD(linux_sigcontext_t, struct sigcontext, rsi);
    FIELD(linux_sigcontext_t, struct sigcontext, rbp);
    FIELD(linux_sigcontext_t, struct sigcontext, rbx);
    FIELD(linux_sigcontext_t, struct sigcontext, rdx);
    FIELD(linux_sigcontext_t, struct sigcontext, rax);
    FIELD(linux_sigcontext_t, struct sigcontext, rcx);
    FIELD(linux_sigcontext_t, struct sigcontext, rsp);
    FIELD(linux_sigcontext_t, struct sigcontext, rip);
    FIELD(linux_sigcontext_t, struct sigcontext, eflags);
    FIELD(linux_sigcontext_t, struct sigcontext, cs);
    FIELD(linux_sigcontext_t, struct sigcontext, gs);
    FIELD(linux_sigcontext_t, struct sigcontext, fs);
    FIELD(linux_sigcontext_t, struct sigcontext, ss);
    FIELD(linux_sigcontext_t, struct sigcontext, err);
    FIELD(linux_sigcontext_t, struct sigcontext, trapno);
    FIELD(linux_sigcontext_t, struct sigcontext, oldmask);
    FIELD(linux_sigcontext_t, struct sigcontext, cr2);
    FIELD(linux_sigcontext_t, struct sigcontext, fpstate);
    FIELD(linux_sigcontext_t, struct sigcontext, reserved1);
    SIZE(linux_ucontext_t, struct ucontext);
    FIELD(linux_ucontext_t, struct ucontext, uc_flags);
    FIELD(linux_ucontext_t, struct ucontext, uc_link);
    FIELD(linux_ucontext_t, struct ucontext, uc_stack);
    FIELD(linux_ucontext_t, struct ucontext, uc_mcontext);
    FIELD(linux_ucontext_t, struct ucontext, uc_sigmask);
    SIZE(linux_siginfo_t, siginfo_t);
    FIELD(linux_siginfo_t, siginfo_t, si_signo);
    FIELD(linux_siginfo_t, siginfo_t, si_errno);
    FIELD(linux_siginfo_t, siginfo_t, si_code);
    PAD(linux_siginfo_t, pad0, siginfo_t, si_pid);
    FIELD2(linux_siginfo_t, siginfo_t, pid, si_pid);
    FIELD2(linux_siginfo_t, siginfo_t, uid, si_uid);
    FIELD2(linux_siginfo_t, siginfo_t, value, si_value);
    FIELD2(linux_siginfo_t, siginfo_t, utime, si_utime);
    FIELD2(linux_siginfo_t, siginfo_t, stime, si_stime);
    TAIL(linux_siginfo_t, siginfo_t, rest);
    /* The two that overlay others, where the helpers put them. */
    expect(offsetof(linux_siginfo_t, pid) == offsetof(siginfo_t, si_addr) &&
           sizeof(((siginfo_t *)0)->si_addr) == 8u,
           "si_addr is the eight bytes from si_pid, as linux_si_set_addr writes it");
    expect(offsetof(linux_siginfo_t, value) == offsetof(siginfo_t, si_status) &&
           sizeof(((siginfo_t *)0)->si_status) == 4u,
           "si_status is si_value's low half, as linux_si_set_status writes it");
    expect(offsetof(linux_siginfo_t, pid) == offsetof(siginfo_t, si_tid) &&
           offsetof(linux_siginfo_t, uid) == offsetof(siginfo_t, si_overrun),
           "a timer's si_timerid and si_overrun are where si_pid and si_uid are");
    CONST(LINUX_SA_SIGINFO, SA_SIGINFO);
    CONST(LINUX_SA_ONSTACK, SA_ONSTACK);
    CONST(LINUX_SA_NODEFER, SA_NODEFER);
    CONST(LINUX_SA_RESETHAND, SA_RESETHAND);
    CONST(LINUX_SS_ONSTACK, SS_ONSTACK);
    CONST(LINUX_SS_DISABLE, SS_DISABLE);
    CONST(LINUX_SS_AUTODISARM, SS_AUTODISARM);
    CONST(LINUX_MINSIGSTKSZ, MINSIGSTKSZ);
    CONST(LINUX_SI_USER, SI_USER);
    CONST(LINUX_SI_KERNEL, SI_KERNEL);
    CONST(LINUX_SI_QUEUE, SI_QUEUE);
    CONST(LINUX_SI_TKILL, SI_TKILL);
    CONST(LINUX_SEGV_MAPERR, SEGV_MAPERR);
    CONST(LINUX_SEGV_ACCERR, SEGV_ACCERR);
    CONST(LINUX_BUS_ADRALN, BUS_ADRALN);
    CONST(LINUX_FPE_INTDIV, FPE_INTDIV);
    CONST(LINUX_FPE_FLTINV, FPE_FLTINV);
    CONST(LINUX_ILL_ILLOPN, ILL_ILLOPN);
    CONST(LINUX_CLD_EXITED, CLD_EXITED);
    CONST(LINUX_CLD_KILLED, CLD_KILLED);
    CONST(LINUX_UC_SIGCONTEXT_SS, UC_SIGCONTEXT_SS);
    CONST(LINUX_UC_STRICT_RESTORE_SS, UC_STRICT_RESTORE_SS);

    LIBC_FIELD(linux_dirent64_t, d_ino);
    LIBC_FIELD(linux_dirent64_t, d_off);
    LIBC_FIELD(linux_dirent64_t, d_reclen);
    LIBC_FIELD(linux_dirent64_t, d_type);
    LIBC_OFFSET(linux_dirent64_t, d_name);

    /* ---- errno: every value the kernel returns ---- */
    CONST(VIBEOS_EPERM, EPERM);
    CONST(VIBEOS_ENOENT, ENOENT);
    CONST(VIBEOS_ESRCH, ESRCH);
    CONST(VIBEOS_EINTR, EINTR);
    CONST(VIBEOS_EIO, EIO);
    CONST(VIBEOS_ENXIO, ENXIO);
    CONST(VIBEOS_E2BIG, E2BIG);
    CONST(VIBEOS_EBADF, EBADF);
    CONST(VIBEOS_ECHILD, ECHILD);
    CONST(VIBEOS_EAGAIN, EAGAIN);
    CONST(VIBEOS_ENOMEM, ENOMEM);
    CONST(VIBEOS_EACCES, EACCES);
    CONST(VIBEOS_EFAULT, EFAULT);
    CONST(VIBEOS_EBUSY, EBUSY);
    CONST(VIBEOS_EXDEV, EXDEV);
    CONST(VIBEOS_EFBIG, EFBIG);
    CONST(VIBEOS_ENOSPC, ENOSPC);
    CONST(VIBEOS_EROFS, EROFS);
    CONST(VIBEOS_EMLINK, EMLINK);
    CONST(VIBEOS_ENOTEMPTY, ENOTEMPTY);
    CONST(VIBEOS_ELOOP, ELOOP);
    CONST(VIBEOS_EOPNOTSUPP, EOPNOTSUPP);
    CONST(VIBEOS_EEXIST, EEXIST);
    CONST(VIBEOS_ENOTDIR, ENOTDIR);
    CONST(VIBEOS_EISDIR, EISDIR);
    CONST(VIBEOS_EINVAL, EINVAL);
    CONST(VIBEOS_ENFILE, ENFILE);
    CONST(VIBEOS_EMFILE, EMFILE);
    CONST(VIBEOS_ENOTTY, ENOTTY);
    CONST(VIBEOS_ESPIPE, ESPIPE);
    CONST(VIBEOS_EPIPE, EPIPE);
    CONST(VIBEOS_ERANGE, ERANGE);
    CONST(VIBEOS_ENAMETOOLONG, ENAMETOOLONG);
    CONST(VIBEOS_ENOLCK, ENOLCK);
    CONST(VIBEOS_ENOSYS, ENOSYS);
    CONST(VIBEOS_ENOTSOCK, ENOTSOCK);

    /* ---- signals and their dispositions ---- */
    CONST(VIBEOS_SIGHUP, SIGHUP);
    CONST(VIBEOS_SIGINT, SIGINT);
    CONST(VIBEOS_SIGQUIT, SIGQUIT);
    CONST(VIBEOS_SIGILL, SIGILL);
    CONST(VIBEOS_SIGABRT, SIGABRT);
    CONST(VIBEOS_SIGFPE, SIGFPE);
    CONST(VIBEOS_SIGKILL, SIGKILL);
    CONST(VIBEOS_SIGSEGV, SIGSEGV);
    CONST(VIBEOS_SIGBUS, SIGBUS);
    CONST(VIBEOS_SIGUSR1, SIGUSR1);
    CONST(VIBEOS_SIGUSR2, SIGUSR2);
    CONST(VIBEOS_SIGPIPE, SIGPIPE);
    CONST(VIBEOS_SIGALRM, SIGALRM);
    CONST(VIBEOS_SIGTERM, SIGTERM);
    CONST(VIBEOS_SIGCHLD, SIGCHLD);
    CONST(VIBEOS_SIGCONT, SIGCONT);
    CONST(VIBEOS_SIGSTOP, SIGSTOP);
    CONST(VIBEOS_SIGWINCH, SIGWINCH);
    CONST(SIG_DFL_ADDR, (unsigned long)SIG_DFL);
    CONST(SIG_IGN_ADDR, (unsigned long)SIG_IGN);
    CONST(VIBEOS_SA_RESTORER, SA_RESTORER);
    CONST(VIBEOS_SA_RESTART, SA_RESTART);
    /* In no header a program can include - it is Linux's own internal number,
     * and ours never leaves the kernel either. What can be held to the host is
     * the property it is chosen for: no errno a program may see is that large,
     * so a result carrying it cannot be mistaken for one. */
    expect(VIBEOS_RESTART_CALL > EHWPOISON, "VIBEOS_RESTART_CALL is above every errno the host defines");
    CONST(LINUX_SIG_BLOCK, SIG_BLOCK);
    CONST(LINUX_SIG_UNBLOCK, SIG_UNBLOCK);
    CONST(LINUX_SIG_SETMASK, SIG_SETMASK);

    /* ---- files: the file layer's flags are Linux's by decision ---- */
    CONST(VIBEOS_O_ACCMODE, O_ACCMODE);
    CONST(VIBEOS_O_RDONLY, O_RDONLY);
    CONST(VIBEOS_O_WRONLY, O_WRONLY);
    CONST(VIBEOS_O_RDWR, O_RDWR);
    CONST(VIBEOS_O_CREAT, O_CREAT);
    CONST(VIBEOS_O_EXCL, O_EXCL);
    CONST(VIBEOS_O_DIRECTORY, O_DIRECTORY);
    CONST(VIBEOS_O_NOFOLLOW, O_NOFOLLOW);
    CONST(VIBEOS_O_TRUNC, O_TRUNC);
    CONST(VIBEOS_O_APPEND, O_APPEND);
    CONST(VIBEOS_O_NONBLOCK, O_NONBLOCK);
    CONST(VIBEOS_O_CLOEXEC, O_CLOEXEC);
    /* linux/stat.h withholds these when the C library is glibc. */
    LIBC_CONST(VIBEOS_S_IFMT, "S_IFMT");
    LIBC_CONST(VIBEOS_S_IFBLK, "S_IFBLK");
    LIBC_CONST(VIBEOS_S_IFLNK, "S_IFLNK");
    LIBC_CONST(VIBEOS_S_IFIFO, "S_IFIFO");
    LIBC_CONST(VIBEOS_S_IFCHR, "S_IFCHR");
    LIBC_CONST(VIBEOS_S_IFDIR, "S_IFDIR");
    LIBC_CONST(VIBEOS_S_IFREG, "S_IFREG");
    LIBC_CONST(VIBEOS_S_IFSOCK, "S_IFSOCK");
    CONST(VIBEOS_SEEK_SET, SEEK_SET);
    CONST(VIBEOS_SEEK_CUR, SEEK_CUR);
    CONST(VIBEOS_SEEK_END, SEEK_END);
    CONST(VIBEOS_FD_CLOEXEC, FD_CLOEXEC);
    CONST(VIBEOS_IOCTL_GET_PGRP, TIOCGPGRP);
    CONST(VIBEOS_IOCTL_SET_PGRP, TIOCSPGRP);
    CONST(LINUX_AT_FDCWD, AT_FDCWD);
    CONST(LINUX_AT_SYMLINK_NOFOLLOW, AT_SYMLINK_NOFOLLOW);
    CONST(LINUX_AT_REMOVEDIR, AT_REMOVEDIR);
    CONST(LINUX_AT_EMPTY_PATH, AT_EMPTY_PATH);
    CONST(LINUX_F_DUPFD, F_DUPFD);
    CONST(LINUX_F_GETFD, F_GETFD);
    CONST(LINUX_F_SETFD, F_SETFD);
    CONST(LINUX_F_GETFL, F_GETFL);
    CONST(LINUX_F_SETFL, F_SETFL);
    CONST(LINUX_F_GETLK, F_GETLK);
    CONST(LINUX_F_SETLK, F_SETLK);
    CONST(LINUX_F_SETLKW, F_SETLKW);
    CONST(LINUX_F_DUPFD_CLOEXEC, F_DUPFD_CLOEXEC);
    CONST(LINUX_FALLOC_FL_KEEP_SIZE, FALLOC_FL_KEEP_SIZE);
    CONST(LINUX_POSIX_FADV_NOREUSE, POSIX_FADV_NOREUSE);
    CONST(LINUX_CLOSE_RANGE_UNSHARE, CLOSE_RANGE_UNSHARE);
    CONST(LINUX_CLOSE_RANGE_CLOEXEC, CLOSE_RANGE_CLOEXEC);
    LIBC_CONST(LINUX_DT_DIR, "DT_DIR");
    LIBC_CONST(LINUX_DT_REG, "DT_REG");

    /* ---- processes, memory, threads ---- */
    CONST(LINUX_CLONE_VM, CLONE_VM);
    CONST(LINUX_CLONE_FS, CLONE_FS);
    CONST(LINUX_CLONE_FILES, CLONE_FILES);
    CONST(LINUX_CLONE_SIGHAND, CLONE_SIGHAND);
    CONST(LINUX_CLONE_THREAD, CLONE_THREAD);
    CONST(LINUX_CLONE_SYSVSEM, CLONE_SYSVSEM);
    CONST(LINUX_CLONE_SETTLS, CLONE_SETTLS);
    CONST(LINUX_CLONE_PARENT_SETTID, CLONE_PARENT_SETTID);
    CONST(LINUX_CLONE_NEWNS, CLONE_NEWNS);
    CONST(LINUX_CSIGNAL, CSIGNAL);
    CONST(LINUX_CLONE_CHILD_CLEARTID, CLONE_CHILD_CLEARTID);
    CONST(LINUX_CLONE_CHILD_SETTID, CLONE_CHILD_SETTID);
    CONST(LINUX_CLONE_PIDFD, CLONE_PIDFD);
    CONST(LINUX_CLONE_INTO_CGROUP, CLONE_INTO_CGROUP);
    CONST(LINUX_CLONE_ARGS_SIZE_VER0, CLONE_ARGS_SIZE_VER0);
    SIZE(linux_clone_args_t, struct clone_args);
    FIELD(linux_clone_args_t, struct clone_args, flags);
    FIELD(linux_clone_args_t, struct clone_args, pidfd);
    FIELD(linux_clone_args_t, struct clone_args, child_tid);
    FIELD(linux_clone_args_t, struct clone_args, parent_tid);
    FIELD(linux_clone_args_t, struct clone_args, exit_signal);
    FIELD(linux_clone_args_t, struct clone_args, stack);
    FIELD(linux_clone_args_t, struct clone_args, stack_size);
    FIELD(linux_clone_args_t, struct clone_args, tls);
    FIELD(linux_clone_args_t, struct clone_args, set_tid);
    FIELD(linux_clone_args_t, struct clone_args, set_tid_size);
    FIELD(linux_clone_args_t, struct clone_args, cgroup);
    CONST(LINUX_P_ALL, P_ALL);
    CONST(LINUX_P_PID, P_PID);
    CONST(LINUX_P_PGID, P_PGID);
    CONST(LINUX_P_PIDFD, P_PIDFD);
    CONST(LINUX_WSTOPPED, WSTOPPED);
    CONST(LINUX_WEXITED, WEXITED);
    CONST(LINUX_WNOWAIT, WNOWAIT);
    CONST(LINUX_PIDFD_NONBLOCK, PIDFD_NONBLOCK);
    CONST(LINUX_GRND_NONBLOCK, GRND_NONBLOCK);
    CONST(LINUX_GRND_RANDOM, GRND_RANDOM);
    CONST(LINUX_GRND_INSECURE, GRND_INSECURE);
    CONST(LINUX_RNDGETENTCNT, RNDGETENTCNT);
    CONST(LINUX_WNOHANG, WNOHANG);
    CONST(LINUX_WUNTRACED, WUNTRACED);
    CONST(LINUX_WCONTINUED, WCONTINUED);
    CONST(LINUX_WNOTHREAD, __WNOTHREAD);
    CONST(LINUX_WALL, __WALL);
    CONST(LINUX_WCLONE, __WCLONE);
    CONST(LINUX_RLIMIT_CPU, RLIMIT_CPU);
    CONST(LINUX_RLIMIT_FSIZE, RLIMIT_FSIZE);
    CONST(LINUX_RLIMIT_DATA, RLIMIT_DATA);
    CONST(LINUX_RLIMIT_STACK, RLIMIT_STACK);
    CONST(LINUX_RLIMIT_CORE, RLIMIT_CORE);
    CONST(LINUX_RLIMIT_RSS, RLIMIT_RSS);
    CONST(LINUX_RLIMIT_NPROC, RLIMIT_NPROC);
    CONST(LINUX_RLIMIT_NOFILE, RLIMIT_NOFILE);
    CONST(LINUX_RLIMIT_MEMLOCK, RLIMIT_MEMLOCK);
    CONST(LINUX_RLIMIT_AS, RLIMIT_AS);
    CONST(LINUX_RLIMIT_LOCKS, RLIMIT_LOCKS);
    CONST(LINUX_RLIMIT_SIGPENDING, RLIMIT_SIGPENDING);
    CONST(LINUX_RLIMIT_MSGQUEUE, RLIMIT_MSGQUEUE);
    CONST(LINUX_RLIMIT_NICE, RLIMIT_NICE);
    CONST(LINUX_RLIMIT_RTPRIO, RLIMIT_RTPRIO);
    CONST(LINUX_RLIMIT_RTTIME, RLIMIT_RTTIME);
    CONST(LINUX_RLIM_NLIMITS, RLIM_NLIMITS);
    CONST(LINUX_RLIM64_INFINITY, RLIM64_INFINITY);
    /* The kernel's own numbering of the ones it enforces is Linux's. */
    CONST(VIBEOS_RLIM_CPU, RLIMIT_CPU);
    CONST(VIBEOS_RLIM_FSIZE, RLIMIT_FSIZE);
    CONST(VIBEOS_RLIM_DATA, RLIMIT_DATA);
    CONST(VIBEOS_RLIM_RSS, RLIMIT_RSS);
    CONST(VIBEOS_RLIM_NPROC, RLIMIT_NPROC);
    CONST(VIBEOS_RLIM_NOFILE, RLIMIT_NOFILE);
    CONST(VIBEOS_RLIM_NICE, RLIMIT_NICE);
    CONST(VIBEOS_RLIM_COUNT, RLIM_NLIMITS);
    CONST(LINUX_PRIO_PROCESS, PRIO_PROCESS);
    CONST(LINUX_PRIO_PGRP, PRIO_PGRP);
    CONST(LINUX_PRIO_USER, PRIO_USER);
    CONST(LINUX_RUSAGE_SELF, RUSAGE_SELF);
    CONST(LINUX_RUSAGE_CHILDREN, RUSAGE_CHILDREN);
    CONST(LINUX_RUSAGE_THREAD, RUSAGE_THREAD);
    CONST(VIBEOS_SIGXCPU, SIGXCPU);
    CONST(VIBEOS_SIGXFSZ, SIGXFSZ);
    SIZE(linux_rusage_t, struct rusage);
    FIELD(linux_rusage_t, struct rusage, ru_utime);
    FIELD(linux_rusage_t, struct rusage, ru_stime);
    FIELD(linux_rusage_t, struct rusage, ru_maxrss);
    FIELD(linux_rusage_t, struct rusage, ru_ixrss);
    FIELD(linux_rusage_t, struct rusage, ru_idrss);
    FIELD(linux_rusage_t, struct rusage, ru_isrss);
    FIELD(linux_rusage_t, struct rusage, ru_minflt);
    FIELD(linux_rusage_t, struct rusage, ru_majflt);
    FIELD(linux_rusage_t, struct rusage, ru_nswap);
    FIELD(linux_rusage_t, struct rusage, ru_inblock);
    FIELD(linux_rusage_t, struct rusage, ru_oublock);
    FIELD(linux_rusage_t, struct rusage, ru_msgsnd);
    FIELD(linux_rusage_t, struct rusage, ru_msgrcv);
    FIELD(linux_rusage_t, struct rusage, ru_nsignals);
    FIELD(linux_rusage_t, struct rusage, ru_nvcsw);
    FIELD(linux_rusage_t, struct rusage, ru_nivcsw);
    CONST(LINUX_PR_SET_NAME, PR_SET_NAME);
    CONST(LINUX_PR_GET_NAME, PR_GET_NAME);
    CONST(LINUX_ARCH_SET_GS, ARCH_SET_GS);
    CONST(LINUX_ARCH_SET_FS, ARCH_SET_FS);
    CONST(LINUX_ARCH_GET_FS, ARCH_GET_FS);
    CONST(LINUX_ARCH_GET_GS, ARCH_GET_GS);
    CONST(LINUX_PROT_NONE, PROT_NONE);
    CONST(LINUX_PROT_WRITE, PROT_WRITE);
    CONST(LINUX_PROT_EXEC, PROT_EXEC);
    CONST(LINUX_MAP_FIXED, MAP_FIXED);
    CONST(LINUX_MAP_ANONYMOUS, MAP_ANONYMOUS);
    CONST(LINUX_FUTEX_WAIT, FUTEX_WAIT);
    CONST(LINUX_FUTEX_WAKE, FUTEX_WAKE);
    CONST(LINUX_FUTEX_PRIVATE_FLAG, FUTEX_PRIVATE_FLAG);

    /* ---- sockets ---- */
    LIBC_CONST(LINUX_AF_INET, "AF_INET");
    LIBC_CONST(LINUX_SOCK_STREAM, "SOCK_STREAM");
    LIBC_CONST(LINUX_SOCK_DGRAM, "SOCK_DGRAM");

    if (!g_fail) {
        printf("  linux_layout: %d comparisons against the host's Linux headers\n", g_checked);
    }
    return g_fail ? -1 : 0;
}

#endif
