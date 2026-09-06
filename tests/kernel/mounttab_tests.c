/* The mount table: attachment, longest-prefix resolution, and the refusals.
 *
 * There was one global mount before this, which was the structural reason only
 * one filesystem could run - not a missing driver, a missing place to put a
 * second one. Everything here is about the properties that make a second one
 * safe rather than merely possible.
 */

#include <stdio.h>
#include <string.h>

#include "vibeos/vfs.h"

static vibeos_fsmount_t g_root, g_usr, g_lib;

static int fail(const char *what) {
    printf("FAIL:mounttab %s\n", what);
    return -1;
}

int test_mounttab(void) {
    vibeos_fsmount_t *m;
    const char *tail;

    vibeos_fs_detach_all();
    memset(&g_root, 0, sizeof(g_root));
    memset(&g_usr, 0, sizeof(g_usr));
    memset(&g_lib, 0, sizeof(g_lib));

    /* ---- what is refused, and why each refusal is not tidiness ---------- */
    if (vibeos_fs_attach("usr", &g_usr) == 0) {
        return fail("a relative mount path was accepted");
    }
    if (vibeos_fs_attach("/usr/", &g_usr) == 0) {
        /* Two spellings of one directory would resolve differently depending
         * on which was registered, which is a wrong answer that looks fine. */
        return fail("a trailing slash was accepted");
    }
    if (vibeos_fs_attach("/usr", 0) == 0) {
        return fail("a null mount was accepted");
    }

    if (vibeos_fs_attach("/", &g_root) != 0) {
        return fail("the root would not attach");
    }
    if (vibeos_fs_attach("/", &g_usr) == 0) {
        /* Overwriting would leave the first mounted and unreachable: files
         * open, blocks dirty, and nothing able to name it to unmount it. */
        return fail("two mounts claimed one path");
    }
    if (vibeos_fs_attach("/usr", &g_usr) != 0 ||
        vibeos_fs_attach("/usr/lib", &g_lib) != 0) {
        return fail("nested mounts would not attach");
    }
    if (vibeos_fs_mount_count() != 3u) {
        return fail("the count is wrong");
    }

    /* ---- longest prefix wins, whatever order they went in -------------- */
    if (vibeos_fs_resolve("/usr/lib/libc.so", &m, &tail) != 0 ||
        m != &g_lib || strcmp(tail, "libc.so") != 0) {
        return fail("the longest prefix did not win");
    }
    if (vibeos_fs_resolve("/usr/share/x", &m, &tail) != 0 ||
        m != &g_usr || strcmp(tail, "share/x") != 0) {
        return fail("a nested path resolved to the wrong mount");
    }
    if (vibeos_fs_resolve("/etc/passwd", &m, &tail) != 0 ||
        m != &g_root || strcmp(tail, "etc/passwd") != 0) {
        return fail("a path outside every sub-mount did not fall to the root");
    }
    /* The mount point itself, with nothing after it. */
    if (vibeos_fs_resolve("/usr", &m, &tail) != 0 ||
        m != &g_usr || tail[0] != 0) {
        return fail("the mount point itself did not resolve to its own mount");
    }

    /* ---- the boundary, which is where a plausible wrong answer lives ---- */
    if (vibeos_fs_resolve("/usrlocal/x", &m, &tail) != 0 || m != &g_root) {
        /* "/usrlocal" must not match a mount at "/usr". A prefix compare
         * without a boundary check says it does, and the result reads
         * perfectly reasonably in a log. */
        return fail("a name that merely starts with a mount point matched it");
    }

    /* ---- detach, and the hole it leaves ------------------------------- */
    if (vibeos_fs_detach("/usr") != 0) {
        return fail("detach failed");
    }
    if (vibeos_fs_mount_count() != 2u) {
        return fail("the count did not drop");
    }
    if (vibeos_fs_detach("/usr") == 0) {
        return fail("detaching twice succeeded");
    }
    /* The deeper mount must survive its parent going away - the table is flat
     * and the entries are independent, and a resolver that assumed otherwise
     * would lose it. */
    if (vibeos_fs_resolve("/usr/lib/libc.so", &m, &tail) != 0 || m != &g_lib) {
        return fail("a nested mount was lost when its parent detached");
    }
    /* And what was under the detached mount now belongs to the root. */
    if (vibeos_fs_resolve("/usr/share/x", &m, &tail) != 0 ||
        m != &g_root || strcmp(tail, "usr/share/x") != 0) {
        return fail("a detached mount's paths did not fall back to the root");
    }

    /* ---- the table is finite and says so ------------------------------ */
    {
        char p[VIBEOS_FS_MOUNT_PATH_MAX];
        uint32_t i;
        int refused = 0;
        for (i = 0; i < VIBEOS_FS_MOUNTS_MAX + 2u; i++) {
            p[0] = '/'; p[1] = 'm'; p[2] = (char)('a' + (i % 26u)); p[3] = 0;
            if (vibeos_fs_attach(p, &g_usr) != 0) {
                refused = 1;
            }
        }
        if (!refused) {
            return fail("the table never refused, so it is not finite");
        }
    }

    vibeos_fs_detach_all();
    if (vibeos_fs_mount_count() != 0u) {
        return fail("detach_all left something behind");
    }
    return 0;
}
