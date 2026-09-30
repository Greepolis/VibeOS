#ifndef VIBEOS_FILES_INTERNAL_H
#define VIBEOS_FILES_INTERNAL_H

/* What the file types in kernel/abi/files share. They sit beside the personalities
 * rather than inside one: they reach the kernel through vibeos/ksvc.h like a
 * handler does, and check-abi-layering.py holds them to that. */

#include <stdint.h>

#include "vibeos/ksvc.h"
#include "vibeos/file.h"
#include "vibeos/fileops.h"
#include "vibeos/pipe.h"
#include "vibeos/vfs.h"
#include "vibeos/inet.h"
#include "vibeos/abi_linux.h"   /* the kernel's errno values and signal numbers */

#endif
