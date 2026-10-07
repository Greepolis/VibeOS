/* The half of the layout test (linux_layout_tests.c) that Linux's uapi headers
 * cannot answer, answered by the C library's headers instead (docs/abi/ A5).
 *
 * Linux does not export struct linux_dirent64; AF_INET, SOCK_STREAM and the DT_
 * types live in the C library's headers, not uapi's; and linux/stat.h withholds
 * the S_IF types from a program built against glibc. The C library's
 * struct dirent64 is what readdir64 hands out exactly as getdents64 wrote it, so
 * its layout is the kernel's by contract.
 *
 * A separate file because the two sets of headers cannot share one: glibc's
 * sys/stat.h and uapi's asm/stat.h both define struct stat. This file includes
 * none of this project's headers either - it only reports numbers. */

#define _GNU_SOURCE 1

#include <stddef.h>
#include <string.h>

#include "linux_layout_libc.h"

#if defined(__linux__)

#include <dirent.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#define F(name) { #name, offsetof(struct dirent64, name), sizeof(((struct dirent64 *)0)->name) }

static const struct { const char *name; size_t off, size; } g_dirent64[] = {
    F(d_ino), F(d_off), F(d_reclen), F(d_type), F(d_name),
};

#define SF(st, f) { #st, #f, offsetof(struct st, f), sizeof(((struct st *)0)->f) }
#define SS(st) { #st, "", 0, sizeof(struct st) }

static const struct { const char *st, *name; size_t off, size; } g_sfields[] = {
    SS(msghdr), SF(msghdr, msg_name), SF(msghdr, msg_namelen), SF(msghdr, msg_iov),
    SF(msghdr, msg_iovlen), SF(msghdr, msg_control), SF(msghdr, msg_controllen), SF(msghdr, msg_flags),
    SS(mmsghdr), SF(mmsghdr, msg_hdr), SF(mmsghdr, msg_len),
    SS(cmsghdr), SF(cmsghdr, cmsg_len), SF(cmsghdr, cmsg_level), SF(cmsghdr, cmsg_type),
    SS(ucred), SF(ucred, pid), SF(ucred, uid), SF(ucred, gid),
    SS(linger), SF(linger, l_onoff), SF(linger, l_linger),
};

static const struct { const char *name; long long value; } g_consts[] = {
    { "AF_UNSPEC", AF_UNSPEC },
    { "AF_UNIX", AF_UNIX },
    { "SOCK_RAW", SOCK_RAW },
    { "SOCK_SEQPACKET", SOCK_SEQPACKET },
    { "SOCK_NONBLOCK", SOCK_NONBLOCK },
    { "SOCK_CLOEXEC", SOCK_CLOEXEC },
    { "MSG_OOB", MSG_OOB },
    { "MSG_PEEK", MSG_PEEK },
    { "MSG_CTRUNC", MSG_CTRUNC },
    { "MSG_TRUNC", MSG_TRUNC },
    { "MSG_DONTWAIT", MSG_DONTWAIT },
    { "MSG_WAITALL", MSG_WAITALL },
    { "MSG_ERRQUEUE", MSG_ERRQUEUE },
    { "MSG_NOSIGNAL", MSG_NOSIGNAL },
    { "MSG_WAITFORONE", MSG_WAITFORONE },
    { "MSG_CMSG_CLOEXEC", MSG_CMSG_CLOEXEC },
    { "SCM_RIGHTS", SCM_RIGHTS },
    { "SCM_CREDENTIALS", SCM_CREDENTIALS },
    { "AF_INET", AF_INET },
    { "SOCK_STREAM", SOCK_STREAM },
    { "SOCK_DGRAM", SOCK_DGRAM },
    { "SHUT_RD", SHUT_RD },
    { "SHUT_WR", SHUT_WR },
    { "SHUT_RDWR", SHUT_RDWR },
    { "SOL_SOCKET", SOL_SOCKET },
    { "SO_REUSEADDR", SO_REUSEADDR },
    { "SO_KEEPALIVE", SO_KEEPALIVE },
    { "DT_DIR", DT_DIR },
    { "DT_REG", DT_REG },
    { "DT_LNK", DT_LNK },
    { "S_IFMT", S_IFMT },
    { "S_IFBLK", S_IFBLK },
    { "S_IFLNK", S_IFLNK },
    { "S_IFIFO", S_IFIFO },
    { "S_IFCHR", S_IFCHR },
    { "S_IFDIR", S_IFDIR },
    { "S_IFREG", S_IFREG },
    { "S_IFSOCK", S_IFSOCK },
    { "S_ISUID", S_ISUID },
    { "S_ISGID", S_ISGID },
    { "S_ISVTX", S_ISVTX },
    { "R_OK", R_OK },
    { "W_OK", W_OK },
    { "X_OK", X_OK },
    { "UTIME_NOW", UTIME_NOW },
    { "UTIME_OMIT", UTIME_OMIT },
    { "ST_RDONLY", ST_RDONLY },
};

long linux_host_getdents(unsigned char *buf, unsigned long cap) {
    int fd = open("/", O_RDONLY | O_DIRECTORY);
    long n;

    if (fd < 0) {
        return -1;
    }
    n = syscall(SYS_getdents, fd, buf, cap);
    close(fd);
    return n;
}

int linux_libc_dirent64(const char *field, size_t *off, size_t *size) {
    size_t i;
    for (i = 0; i < sizeof(g_dirent64) / sizeof(g_dirent64[0]); i++) {
        if (strcmp(g_dirent64[i].name, field) == 0) {
            *off = g_dirent64[i].off;
            *size = g_dirent64[i].size;
            return 0;
        }
    }
    return -1;
}

int linux_libc_const(const char *name, long long *value) {
    size_t i;
    for (i = 0; i < sizeof(g_consts) / sizeof(g_consts[0]); i++) {
        if (strcmp(g_consts[i].name, name) == 0) {
            *value = g_consts[i].value;
            return 0;
        }
    }
    return -1;
}

int linux_libc_sfield(const char *st, const char *field, size_t *off, size_t *size) {
    size_t i;
    for (i = 0; i < sizeof(g_sfields) / sizeof(g_sfields[0]); i++) {
        if (strcmp(g_sfields[i].st, st) == 0 && strcmp(g_sfields[i].name, field) == 0) {
            *off = g_sfields[i].off;
            *size = g_sfields[i].size;
            return 0;
        }
    }
    return -1;
}

#else

int linux_libc_sfield(const char *st, const char *field, size_t *off, size_t *size) {
    (void)st; (void)field; (void)off; (void)size;
    return -1;
}

long linux_host_getdents(unsigned char *buf, unsigned long cap) {
    (void)buf; (void)cap;
    return -1;
}

int linux_libc_dirent64(const char *field, size_t *off, size_t *size) {
    (void)field; (void)off; (void)size;
    return -1;
}

int linux_libc_const(const char *name, long long *value) {
    (void)name; (void)value;
    return -1;
}

#endif
