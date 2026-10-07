#ifndef VIBEOS_LINUX_LAYOUT_LIBC_H
#define VIBEOS_LINUX_LAYOUT_LIBC_H

/* What the C library's headers say, for the values Linux's uapi headers do not
 * carry (linux_layout_libc.c). 0 when known, -1 when not. */

#include <stddef.h>

int linux_libc_dirent64(const char *field, size_t *off, size_t *size);
/* What the host's own kernel answers the old getdents call with, for "/": the
 * bytes, or negative. No header declares that record, so the kernel is asked. */
long linux_host_getdents(unsigned char *buf, unsigned long cap);

int linux_libc_const(const char *name, long long *value);

/* A field of one of the C library's socket structures (msghdr, mmsghdr,
 * cmsghdr, ucred, linger), or with field "" the structure's size. */
int linux_libc_sfield(const char *st, const char *field, size_t *off, size_t *size);

#endif
