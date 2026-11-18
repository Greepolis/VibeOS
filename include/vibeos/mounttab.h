/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The mount manifest, /etc/vibeos/mounts (hierarchy F2 step 3).
 *
 *     vibeos-mounts 1
 *     # what            where      type      options        policy
 *     UUID=1A2B-3C4D    /boot/efi  vfat      rw             required
 *     tmpfs             /tmp       tmpfs     nosuid,nodev   required
 *
 * One mount a line, fields separated by spaces or tabs, '#' to the end of a
 * line a comment. `what` is UUID=..., LABEL=... or, for a filesystem with no
 * volume, its own name. The root is not a line: the ESP names it
 * (EFI/VIBEOS/ROOT), because this file lives on it.
 *
 * Parsing is all or nothing. A manifest with one bad line is refused whole
 * and the caller falls back to its built-in default, loudly: a typo in one
 * line must not take away the mounts the others name. */
#ifndef VIBEOS_MOUNTTAB_H
#define VIBEOS_MOUNTTAB_H

#include <stdint.h>

#define VIBEOS_MOUNTTAB_MAX 16u
#define VIBEOS_MOUNTTAB_WHAT_MAX 48u
#define VIBEOS_MOUNTTAB_AT_MAX 32u     /* VIBEOS_FS_MOUNT_PATH_MAX */
#define VIBEOS_MOUNTTAB_TYPE_MAX 16u

/* Options, as bits. Step 4 enforces them; this step only refuses a name it
 * does not know, so a misspelt "noexce" is a refused manifest rather than a
 * mount that silently allows exec. */
#define VIBEOS_MNT_RO 0x1u
#define VIBEOS_MNT_NOEXEC 0x2u
#define VIBEOS_MNT_NODEV 0x4u
#define VIBEOS_MNT_NOSUID 0x8u

typedef struct {
    char what[VIBEOS_MOUNTTAB_WHAT_MAX];
    char at[VIBEOS_MOUNTTAB_AT_MAX];
    char type[VIBEOS_MOUNTTAB_TYPE_MAX];
    uint32_t options;
    uint32_t mode;       /* mode=<octal> for a tmpfs root; 0 when not given */
    uint32_t size_kib;   /* size=<n>k or <n>m for a tmpfs; 0 when not given */
    uint8_t required;
    uint8_t is_volume;   /* what is UUID= or LABEL=, not a pseudo filesystem */
} vibeos_mounttab_entry_t;

typedef struct {
    uint32_t count;
    vibeos_mounttab_entry_t entry[VIBEOS_MOUNTTAB_MAX];
} vibeos_mounttab_t;

#define VIBEOS_MOUNTTAB_OK 0
#define VIBEOS_MOUNTTAB_NO_VERSION 1   /* first line is not "vibeos-mounts 1" */
#define VIBEOS_MOUNTTAB_FIELDS 2       /* not five fields */
#define VIBEOS_MOUNTTAB_TOO_LONG 3     /* a field past its limit */
#define VIBEOS_MOUNTTAB_PATH 4         /* where is not absolute, or is "/" */
#define VIBEOS_MOUNTTAB_DUPLICATE 5    /* where named twice */
#define VIBEOS_MOUNTTAB_OPTION 6       /* an option this kernel does not know */
#define VIBEOS_MOUNTTAB_POLICY 7       /* neither required nor optional */
#define VIBEOS_MOUNTTAB_TOO_MANY 8
#define VIBEOS_MOUNTTAB_WHAT 9         /* UUID= or LABEL= with nothing after */

/* Parses text[0..len). Returns VIBEOS_MOUNTTAB_OK and fills *out, or a
 * reason with *bad_line the 1-based line it stopped on and *out empty. */
int vibeos_mounttab_parse(const char *text, long len, vibeos_mounttab_t *out, uint32_t *bad_line);
const char *vibeos_mounttab_why(int why);

/* The built-in manifest: today's mounts, used when the root has none or has
 * one that does not parse. The ESP is named by what the root file named. */
const char *vibeos_mounttab_default(void);

#endif
