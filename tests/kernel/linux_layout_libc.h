#ifndef VIBEOS_LINUX_LAYOUT_LIBC_H
#define VIBEOS_LINUX_LAYOUT_LIBC_H

/* What the C library's headers say, for the values Linux's uapi headers do not
 * carry (linux_layout_libc.c). 0 when known, -1 when not. */

#include <stddef.h>

int linux_libc_dirent64(const char *field, size_t *off, size_t *size);
int linux_libc_const(const char *name, long long *value);

#endif
