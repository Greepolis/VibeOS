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
#include <sys/socket.h>
#include <sys/stat.h>

#define F(name) { #name, offsetof(struct dirent64, name), sizeof(((struct dirent64 *)0)->name) }

static const struct { const char *name; size_t off, size; } g_dirent64[] = {
    F(d_ino), F(d_off), F(d_reclen), F(d_type), F(d_name),
};

static const struct { const char *name; long long value; } g_consts[] = {
    { "AF_INET", AF_INET },
    { "SOCK_STREAM", SOCK_STREAM },
    { "SOCK_DGRAM", SOCK_DGRAM },
    { "DT_DIR", DT_DIR },
    { "DT_REG", DT_REG },
    { "S_IFIFO", S_IFIFO },
    { "S_IFCHR", S_IFCHR },
    { "S_IFDIR", S_IFDIR },
    { "S_IFREG", S_IFREG },
    { "S_IFSOCK", S_IFSOCK },
};

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

#else

int linux_libc_dirent64(const char *field, size_t *off, size_t *size) {
    (void)field; (void)off; (void)size;
    return -1;
}

int linux_libc_const(const char *name, long long *value) {
    (void)name; (void)value;
    return -1;
}

#endif
