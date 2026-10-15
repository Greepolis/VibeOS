/* msvcrt.dll's types and FILE against mingw-w64's own headers, at compile time
 * (docs/abi/windows.md W2). Contributes nothing to the DLL; if a number our
 * msvcrt.c assumes about msvcrt's ABI disagrees with the headers a Windows
 * program is compiled against, the build stops here.
 *
 * The headers are the artefact neither msvcrt.c nor its tests wrote: the host
 * tests can only show that the core agrees with itself and with glibc, and
 * glibc's `long` is 64 bits. */

#include <float.h>
#include <stddef.h>
#include <stdio.h>
#include <time.h>
#include <wchar.h>

#include "crt_file.h"

#define SAME(ours, theirs) _Static_assert((ours) == (theirs), #ours " is not what mingw-w64 has")

SAME(sizeof(long), 4);                  /* LLP64: what printf's %ld and strtol assume */
SAME(sizeof(wchar_t), 2);               /* UTF-16: what every wcs* function assumes   */
SAME(sizeof(__time64_t), 8);
SAME(sizeof(size_t), sizeof(void *));
SAME(sizeof(intptr_t), 8);              /* _get_osfhandle                              */

SAME(sizeof(vc_iobuf), sizeof(FILE));
SAME(offsetof(vc_iobuf, _ptr), offsetof(struct _iobuf, _ptr));
SAME(offsetof(vc_iobuf, _cnt), offsetof(struct _iobuf, _cnt));
SAME(offsetof(vc_iobuf, _base), offsetof(struct _iobuf, _base));
SAME(offsetof(vc_iobuf, _flag), offsetof(struct _iobuf, _flag));
SAME(offsetof(vc_iobuf, _file), offsetof(struct _iobuf, _file));
SAME(offsetof(vc_iobuf, _charbuf), offsetof(struct _iobuf, _charbuf));
SAME(offsetof(vc_iobuf, _bufsiz), offsetof(struct _iobuf, _bufsiz));
SAME(offsetof(vc_iobuf, _tmpfname), offsetof(struct _iobuf, _tmpfname));

/* The control-word constants crt_core.h spells (float.h's own). */
#include "crt_core.h"
SAME(VC_MCW_EM, _MCW_EM);
SAME(VC_MCW_RC, _MCW_RC);
SAME(VC_MCW_PC, _MCW_PC);
SAME(VC_MCW_DN, _MCW_DN);
SAME(0x00000010u, _EM_INVALID);
SAME(0x00080000u, _EM_DENORMAL);
SAME(0x00000008u, _EM_ZERODIVIDE);
SAME(0x00000004u, _EM_OVERFLOW);
SAME(0x00000002u, _EM_UNDERFLOW);
SAME(0x00000001u, _EM_INEXACT);
SAME(0x00000100u, _RC_DOWN);
SAME(0x00000200u, _RC_UP);
SAME(0x00000300u, _RC_CHOP);
SAME(0x00020000u, _PC_24);
SAME(0x00010000u, _PC_53);
SAME(0x00000000u, _PC_64);
SAME(0x01000000u, _DN_FLUSH);
