/* SPDX-License-Identifier: GPL-3.0-or-later */

#include <stdio.h>
#include <string.h>

#include "vibeos/abi_linux.h"
#include "vibeos/devfs.h"
#include "vibeos/drm.h"
#include "vibeos/drm_uapi.h"
#include "vibeos/file.h"
#include "vibeos/fileops.h"

int test_drm(void);

static int expect(int cond, const char *what) {
    if (!cond) {
        printf("FAIL:drm %s\n", what);
    }
    return cond;
}

static long drm_call(vibeos_file_t *f, uint64_t req, const void *arg) {
    return f->ops->ioctl(f, req, (uint64_t)(uintptr_t)arg);
}

static int drm_devfs(void) {
    const vibeos_fs_ops_t *ops = vibeos_devfs_ops();
    vibeos_fs_node_t node;
    vibeos_path_t walk;
    vibeos_file_t *f;
    char name[32];
    uint64_t size;
    int is_dir;
    long err = 0;
    int found = 0;
    uint32_t i;

    if (!expect(vibeos_path_walk("/", "/", "/dev/dri", 0, &walk) == 0 &&
                walk.node.is_dir,
                "/dev/dri is a directory")) {
        return -1;
    }
    if (!expect(vibeos_path_walk("/", "/", "/dev/dri/card0", 0, &walk) == 0 &&
                walk.node.rdev == VIBEOS_DEV_DRM_CARD0 &&
                (walk.node.mode & VIBEOS_S_IFMT) == VIBEOS_S_IFCHR,
                "/dev/dri/card0 is the DRM character device")) {
        return -1;
    }
    f = vibeos_open_path(&walk, VIBEOS_O_RDWR, 0, &err);
    if (!expect(f && err == 0 && f->rdev == VIBEOS_DEV_DRM_CARD0,
                "opening /dev/dri/card0 reaches the DRM driver")) {
        if (f) {
            vibeos_file_put(f);
        }
        return -1;
    }
    vibeos_file_put(f);
    if (!expect(vibeos_path_walk("/", "/", "/dev/dri/card0/", 0, &walk) == -VIBEOS_ENOTDIR,
                "a trailing slash on card0 is ENOTDIR")) {
        return -1;
    }
    for (i = 0; ops->list(0, "/", i, name, sizeof(name), &size, &is_dir) == 0; i++) {
        if (strcmp(name, "dri") == 0) {
            found = is_dir;
        }
    }
    if (!expect(found, "the /dev listing has dri as a directory")) {
        return -1;
    }
    if (!expect(ops->list(0, "dri", 0u, name, sizeof(name), &size, &is_dir) == 0 &&
                strcmp(name, "card0") == 0 && !is_dir,
                "the /dev/dri listing has card0")) {
        return -1;
    }
    if (!expect(ops->list(0, "dri", 1u, name, sizeof(name), &size, &is_dir) != 0,
                "the /dev/dri listing stops after card0")) {
        return -1;
    }
    if (!expect(ops->lookup(0, "dri/card0", &node) == 0 && !node.is_dir &&
                node.rdev == VIBEOS_DEV_DRM_CARD0,
                "devfs lookup finds dri/card0")) {
        return -1;
    }
    return 0;
}

/*
 * test_drm - kernel self-test for the DRM device and its ioctl surface.
 *
 * Coverage:
 *   - devfs wiring: /dev/dri is a directory and /dev/dri/card0 is the DRM
 *     character device (drm_devfs, called first);
 *   - capabilities: GET_CAP reports dumb-buffer support and preferred depth,
 *     and rejects unknown capabilities;
 *   - dumb buffers: MODE_CREATE_DUMB returns a handle and geometry,
 *     MODE_MAP_DUMB a page-aligned offset, and share_page hands back the
 *     backing page;
 *   - per-file GEM names (review M-082): GEM_FLINK mints a name scoped to the
 *     opening file, another file cannot open it, the owning file can reopen it,
 *     and the name disappears with the buffer;
 *   - error paths: unknown handles and name zero are refused on every ioctl;
 *   - boot path: vibeos_drm_boot_smoke exercises the DRM open/ioctl/mmap
 *     sequence through the chrdev entry, proving the path the real boot will
 *     use (reviewer note on PR #165); the devfs node is checked by drm_devfs().
 */
int test_drm(void) {
    struct vibeos_drm_get_cap cap;
    struct vibeos_drm_mode_create_dumb create;
    struct vibeos_drm_mode_map_dumb map;
    struct vibeos_drm_gem_flink flink;
    struct vibeos_drm_gem_open open;
    struct vibeos_drm_gem_close close;
    struct vibeos_drm_mode_destroy_dumb destroy;
    vibeos_file_t *f1;
    vibeos_file_t *f2;
    vibeos_file_t *f3;
    uint64_t offset1;
    uint32_t name;
    uint32_t first_handle;
    uint32_t second_handle;
    uint32_t imported_handle;
    void *page1 = 0;
    void *page2 = 0;
    long err = 0;

    if (drm_devfs() != 0) {
        return -1;
    }
    f1 = vibeos_drm_open(VIBEOS_O_RDWR, &err);
    f2 = vibeos_drm_open(VIBEOS_O_RDWR, &err);
    if (!expect(f1 && f2 && f1->drv && f2->drv, "two DRM opens have their own state")) {
        if (f1) {
            vibeos_file_put(f1);
        }
        if (f2) {
            vibeos_file_put(f2);
        }
        return -1;
    }

    /* Capabilities: GET_CAP reports what the driver supports. */
    memset(&cap, 0, sizeof(cap));
    cap.capability = VIBEOS_DRM_CAP_DUMB_BUFFER;
    if (!expect(drm_call(f1, VIBEOS_DRM_IOCTL_GET_CAP, &cap) == 0 && cap.value == 1u,
                "GET_CAP reports dumb buffers")) {
        return -1;
    }
    cap.capability = VIBEOS_DRM_CAP_DUMB_PREFERRED_DEPTH;
    cap.value = 0;
    if (!expect(drm_call(f1, VIBEOS_DRM_IOCTL_GET_CAP, &cap) == 0 && cap.value == 32u,
                "GET_CAP reports the preferred depth")) {
        return -1;
    }
    cap.capability = 0xdeadbeefu;
    cap.value = 0;
    if (!expect(drm_call(f1, VIBEOS_DRM_IOCTL_GET_CAP, &cap) == -VIBEOS_EINVAL,
                "GET_CAP refuses an unknown capability")) {
        return -1;
    }

    /* Allocate and map a dumb buffer; the handle is local to this open file. */
    memset(&create, 0, sizeof(create));
    create.width = 4;
    create.height = 4;
    create.bpp = 32;
    if (!expect(drm_call(f1, VIBEOS_DRM_IOCTL_MODE_CREATE_DUMB, &create) == 0 &&
                create.handle == 1u && create.pitch == 64u && create.size == 256u,
                "MODE_CREATE_DUMB returns a handle and geometry")) {
        return -1;
    }

    memset(&map, 0, sizeof(map));
    map.handle = create.handle;
    if (!expect(drm_call(f1, VIBEOS_DRM_IOCTL_MODE_MAP_DUMB, &map) == 0 &&
                map.offset != 0u && (map.offset & 0xFFFull) == 0u,
                "MODE_MAP_DUMB returns a page-aligned offset")) {
        return -1;
    }
    offset1 = map.offset;
    first_handle = create.handle;

    if (!expect(f1->ops->share_page(f1, offset1, &page1) == 0 && page1,
                "the dumb buffer has a page to share")) {
        return -1;
    }
    ((uint32_t *)page1)[0] = 0x5a5a5a5au;

    /* GEM names are per-file (M-082): flink a handle, then prove the boundary. */
    memset(&flink, 0, sizeof(flink));
    flink.handle = create.handle;
    if (!expect(drm_call(f1, VIBEOS_DRM_IOCTL_GEM_FLINK, &flink) == 0 &&
                flink.name != 0u,
                "GEM_FLINK gives the buffer a per-file name")) {
        return -1;
    }
    name = flink.name;
    flink.name = 0;
    if (!expect(drm_call(f1, VIBEOS_DRM_IOCTL_GEM_FLINK, &flink) == 0 &&
                flink.name == name,
                "GEM_FLINK is idempotent")) {
        return -1;
    }

    /* A GEM name is private to the file that minted it (review M-082): another
     * open of the same device cannot open a name it did not create, so one
     * process cannot reach another's buffer through FLINK/OPEN. */
    memset(&open, 0, sizeof(open));
    open.name = name;
    if (!expect(drm_call(f2, VIBEOS_DRM_IOCTL_GEM_OPEN, &open) == -VIBEOS_ENOENT,
                "GEM_OPEN in another file cannot see a per-file name")) {
        return -1;
    }

    /* The owning file can reopen its own name, proving the name still resolves
     * locally and points at the same backing pages. */
    memset(&open, 0, sizeof(open));
    open.name = name;
    if (!expect(drm_call(f1, VIBEOS_DRM_IOCTL_GEM_OPEN, &open) == 0 &&
                open.handle == 2u && open.size == 256u,
                "GEM_OPEN reopens the buffer within its owning file")) {
        return -1;
    }
    imported_handle = open.handle;

    memset(&map, 0, sizeof(map));
    map.handle = open.handle;
    if (!expect(drm_call(f1, VIBEOS_DRM_IOCTL_MODE_MAP_DUMB, &map) == 0 &&
                map.offset == offset1,
                "the reopened buffer maps at the same fake offset")) {
        return -1;
    }
    if (!expect(f1->ops->share_page(f1, offset1, &page2) == 0 && page2 == page1,
                "the owning file reaches the same GEM pages")) {
        return -1;
    }

    memset(&close, 0, sizeof(close));
    close.handle = first_handle;
    if (!expect(drm_call(f1, VIBEOS_DRM_IOCTL_GEM_CLOSE, &close) == 0,
                "GEM_CLOSE drops one handle")) {
        return -1;
    }
    page2 = 0;
    if (!expect(f1->ops->share_page(f1, offset1, &page2) == 0 && page2 == page1,
                "a name keeps the buffer alive while the owning file holds it")) {
        return -1;
    }

    memset(&create, 0, sizeof(create));
    create.width = 2;
    create.height = 2;
    create.bpp = 32;
    if (!expect(drm_call(f1, VIBEOS_DRM_IOCTL_MODE_CREATE_DUMB, &create) == 0 &&
                create.handle == 1u,
                "a closed handle can be reused in the same file")) {
        return -1;
    }
    second_handle = create.handle;
    memset(&map, 0, sizeof(map));
    map.handle = create.handle;
    if (!expect(drm_call(f1, VIBEOS_DRM_IOCTL_MODE_MAP_DUMB, &map) == 0 &&
                map.offset != offset1,
                "a new GEM object gets a fresh fake offset")) {
        return -1;
    }
    memset(&flink, 0, sizeof(flink));
    flink.handle = 99u;
    if (!expect(drm_call(f1, VIBEOS_DRM_IOCTL_GEM_FLINK, &flink) == -VIBEOS_ENOENT,
                "GEM_FLINK refuses an unknown handle")) {
        return -1;
    }
    memset(&open, 0, sizeof(open));
    open.name = 0;
    if (!expect(drm_call(f1, VIBEOS_DRM_IOCTL_GEM_OPEN, &open) == -VIBEOS_ENOENT,
                "GEM_OPEN refuses name zero")) {
        return -1;
    }
    memset(&map, 0, sizeof(map));
    map.handle = 99u;
    if (!expect(drm_call(f1, VIBEOS_DRM_IOCTL_MODE_MAP_DUMB, &map) == -VIBEOS_ENOENT,
                "MODE_MAP_DUMB refuses an unknown handle")) {
        return -1;
    }
    memset(&close, 0, sizeof(close));
    close.handle = 99u;
    if (!expect(drm_call(f1, VIBEOS_DRM_IOCTL_GEM_CLOSE, &close) == -VIBEOS_EINVAL,
                "GEM_CLOSE refuses an unknown handle")) {
        return -1;
    }
    memset(&destroy, 0, sizeof(destroy));
    destroy.handle = 99u;
    if (!expect(drm_call(f1, VIBEOS_DRM_IOCTL_MODE_DESTROY_DUMB, &destroy) == -VIBEOS_EINVAL,
                "MODE_DESTROY_DUMB refuses an unknown handle")) {
        return -1;
    }

    close.handle = second_handle;
    if (!expect(drm_call(f1, VIBEOS_DRM_IOCTL_GEM_CLOSE, &close) == 0,
                "the second object closes")) {
        return -1;
    }
    close.handle = imported_handle;
    if (!expect(drm_call(f1, VIBEOS_DRM_IOCTL_GEM_CLOSE, &close) == 0,
                "the imported handle closes")) {
        return -1;
    }

    vibeos_file_put(f1);
    vibeos_file_put(f2);

    err = 0;
    f3 = vibeos_drm_open(VIBEOS_O_RDWR, &err);
    if (!expect(f3 && f3->drv, "a third DRM open succeeds")) {
        if (f3) {
            vibeos_file_put(f3);
        }
        return -1;
    }
    memset(&open, 0, sizeof(open));
    open.name = name;
    if (!expect(drm_call(f3, VIBEOS_DRM_IOCTL_GEM_OPEN, &open) == -VIBEOS_ENOENT,
                "a name is gone once its owning file frees the buffer")) {
        return -1;
    }
    vibeos_file_put(f3);

    /* Boot path: exercise the open/ioctl/mmap sequence through the exported
     * smoke routine, so the in-kernel path the machine boot uses is covered
     * even on the fake harness (reviewer note on PR #165). */
    if (!expect(vibeos_drm_boot_smoke() == 0,
                "vibeos_drm_boot_smoke exercises the DRM open/ioctl/mmap path")) {
        return -1;
    }
    return 0;
}
