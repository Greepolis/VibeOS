#ifndef VIBEOS_WIN_CRT_FILE_H
#define VIBEOS_WIN_CRT_FILE_H

/* msvcrt's x64 FILE, field for field.
 *
 * A program built against mingw-w64's headers reaches stdout as
 * &__iob_func()[1], so the size of this structure is part of our ABI: the
 * stride between the three streams. layout_check.c holds every offset against
 * mingw-w64's own struct _iobuf at compile time. */

#include <stddef.h>
#include <stdint.h>

typedef struct vc_iobuf {
    char *_ptr;
    int _cnt;
    char *_base;
    int _flag;
    int _file;
    int _charbuf;
    int _bufsiz;
    char *_tmpfname;
} vc_iobuf;

_Static_assert(sizeof(vc_iobuf) == 48, "msvcrt's x64 FILE is 48 bytes");
_Static_assert(offsetof(vc_iobuf, _file) == 28, "msvcrt's descriptor field is at 28");

#endif
