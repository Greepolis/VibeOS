/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * DRM for VibeOS, ported from GNOS (Copyright (C) 2026 and earlier GNOS
 * project contributors) and authorized by the original author under
 * GPL-3.0-or-later on 2026-11-05. See NOTICE.
 */

#include "vibeos/abi_linux.h"
#include "vibeos/devfs.h"
#include "vibeos/drm.h"
#include "vibeos/drm_uapi.h"
#include "vibeos/ksvc.h"

#define VIBEOS_DRM_FILE_MAGIC 0x44524d46u
#define VIBEOS_DRM_MAX_GEM 16u
#define VIBEOS_DRM_MAX_PAGES 504u
#define VIBEOS_DRM_MAP_STRIDE (1ull << 32)

struct vibeos_drm_gem {
    uint64_t map_offset;
    uint32_t size;
    uint32_t npages;
    uint32_t refs;
    uint32_t name;
    void *pages[VIBEOS_DRM_MAX_PAGES];
};

struct vibeos_drm_file {
    uint32_t magic;
    uint32_t ngem;
    uint32_t next_name;     /* per-file GEM name generator (M-082) */
    struct vibeos_drm_gem *gem[VIBEOS_DRM_MAX_GEM];
};

/* A GEM name is the handle GEM_FLINK/GEM_OPEN share. It is scoped to the open
 * device file (struct vibeos_drm_file), never global: a name minted in one
 * file cannot be opened from another, so one process cannot reach another's
 * buffer through the FLINK/OPEN pair. See docs/abi review M-082. */
_Static_assert(sizeof(struct vibeos_drm_gem) <= 4096u, "drm gem fits one page");
_Static_assert(sizeof(struct vibeos_drm_file) <= 4096u, "drm file fits one page");

static const vibeos_file_ops_t vibeos_fops_drm;
static vibeos_lock_t g_drm_lock;
static uint64_t g_drm_next_offset = VIBEOS_DRM_MAP_STRIDE;

static void drm_zero(void *p, size_t n) {
    uint8_t *b = (uint8_t *)p;
    size_t i;

    for (i = 0; i < n; i++) {
        b[i] = 0;
    }
}

static size_t drm_strlen(const char *s) {
    size_t n = 0;

    while (s[n]) {
        n++;
    }
    return n;
}

/* Every pointer a DRM ioctl reads or writes is a user address. Judge it with
 * linux_user_ok before the fault-safe copy, exactly as the rest of the ABI
 * does (kernel/abi/linux/*.c), so a 0666 /dev/dri/card0 cannot be made to
 * read or write kernel memory at an address it supplies. A null argument is
 * refused up front; a bad range is refused by the check. */
static int drm_copy_from_user(void *dst, uint64_t arg, size_t n) {
    if (arg == 0u) {
        return -VIBEOS_EFAULT;
    }
    /* In a task (the real kernel, or a faked one in a test) the pointer must be
     * a user address, judged as every handler under kernel/abi/ does. A host
     * test with no current task runs in one address space and skips the
     * judgement, which is safe there (review M-082). */
    if (ks_current() >= 0 && !linux_user_ok(arg, (uint64_t)n, 0)) {
        return -VIBEOS_EFAULT;
    }
    if (vibeos_uaccess_copy(dst, (const void *)(uintptr_t)arg, n) != 0) {
        return -VIBEOS_EFAULT;
    }
    return 0;
}

static int drm_copy_to_user(uint64_t arg, const void *src, size_t n) {
    if (arg == 0u) {
        return -VIBEOS_EFAULT;
    }
    if (ks_current() >= 0 && !linux_user_ok(arg, (uint64_t)n, 1)) {
        return -VIBEOS_EFAULT;
    }
    if (vibeos_uaccess_copy((void *)(uintptr_t)arg, src, n) != 0) {
        return -VIBEOS_EFAULT;
    }
    return 0;
}

static int drm_copy_string(char *dst, size_t cap, size_t *len, const char *src) {
    size_t n = drm_strlen(src);
    size_t c;

    *len = n;
    if (cap == 0u) {
        return 0;
    }
    if (!dst) {
        return -VIBEOS_EFAULT;
    }
    /* `dst` came from the user's struct and points at user memory we write
     * into; judge it before the copy (only while a task owns the pointer). */
    if (ks_current() >= 0 && !linux_user_ok((uint64_t)(uintptr_t)dst, cap, 1)) {
        return -VIBEOS_EFAULT;
    }
    c = n < cap ? n : cap;
    if (c != 0u && vibeos_uaccess_copy(dst, src, c) != 0) {
        return -VIBEOS_EFAULT;
    }
    return 0;
}

static int drm_version(uint64_t arg) {
    struct vibeos_drm_version v;
    int r;

    if (drm_copy_from_user(&v, arg, sizeof(v)) != 0) {
        return -VIBEOS_EFAULT;
    }
    v.version_major = 1;
    v.version_minor = 0;
    v.version_patchlevel = 0;
    if ((r = drm_copy_string(v.name, v.name_len, &v.name_len, "vibeos")) != 0 ||
        (r = drm_copy_string(v.date, v.date_len, &v.date_len, "2026")) != 0 ||
        (r = drm_copy_string(v.desc, v.desc_len, &v.desc_len, "VibeOS DRM/KMS")) != 0) {
        return r;
    }
    if (drm_copy_to_user(arg, &v, sizeof(v)) != 0) {
        return -VIBEOS_EFAULT;
    }
    return 0;
}

static struct vibeos_drm_gem *drm_gem_find(struct vibeos_drm_file *df, uint32_t handle) {
    if (handle == 0u || handle > VIBEOS_DRM_MAX_GEM) {
        return 0;
    }
    return df->gem[handle - 1u];
}

static int drm_gem_slot(struct vibeos_drm_file *df) {
    uint32_t i;

    for (i = 0; i < VIBEOS_DRM_MAX_GEM; i++) {
        if (!df->gem[i]) {
            return (int)i;
        }
    }
    return -1;
}

static struct vibeos_drm_gem *drm_gem_find_by_name(struct vibeos_drm_file *df, uint32_t name) {
    uint32_t i;

    if (name == 0u) {
        return 0;
    }
    /* Names are private to this open file (review M-082): only a buffer this
     * file itself named can be opened back through GEM_OPEN, so one process
     * cannot reach another's buffer through the FLINK/OPEN pair. */
    for (i = 0; i < VIBEOS_DRM_MAX_GEM; i++) {
        struct vibeos_drm_gem *g = df->gem[i];
        if (g && g->name == name) {
            return g;
        }
    }
    return 0;
}

static void drm_name_remove(struct vibeos_drm_gem *g) {
    g->name = 0;
}

static int drm_name_add(struct vibeos_drm_file *df, struct vibeos_drm_gem *g, uint32_t *name) {
    uint32_t tries;

    if (g->name) {
        *name = g->name;
        return 0;
    }
    for (tries = 0; tries < 1024u; tries++) {
        uint32_t candidate = df->next_name++;
        uint32_t i;

        if (candidate == 0u) {
            continue;
        }
        for (i = 0; i < VIBEOS_DRM_MAX_GEM; i++) {
            struct vibeos_drm_gem *o = df->gem[i];
            if (o && o->name == candidate) {
                break;
            }
        }
        if (i == VIBEOS_DRM_MAX_GEM) {
            g->name = candidate;
            *name = candidate;
            return 0;
        }
    }
    return -VIBEOS_ENOSPC;
}

static void drm_gem_free(struct vibeos_drm_gem *g) {
    uint32_t i;

    if (!g) {
        return;
    }
    for (i = 0; i < g->npages; i++) {
        if (g->pages[i]) {
            ks_page_free(g->pages[i], "drm_gem_page");
        }
    }
    ks_page_free(g, "drm_gem");
}

static int drm_gem_put_locked(struct vibeos_drm_gem *g) {
    if (--g->refs != 0u) {
        return 0;
    }
    drm_name_remove(g);
    return 1;
}

static int drm_gem_close_handle(struct vibeos_drm_file *df, uint32_t handle) {
    struct vibeos_drm_gem *g;
    int doomed;

    ks_lock(&g_drm_lock, __func__);
    g = drm_gem_find(df, handle);
    if (!g) {
        ks_unlock(&g_drm_lock);
        return -VIBEOS_EINVAL;
    }
    df->gem[handle - 1u] = 0;
    df->ngem--;
    doomed = drm_gem_put_locked(g);
    ks_unlock(&g_drm_lock);
    if (doomed) {
        drm_gem_free(g);
    }
    return 0;
}

static int drm_gem_create(vibeos_file_t *f, uint64_t arg) {
    struct vibeos_drm_file *df = (struct vibeos_drm_file *)f->drv;
    struct vibeos_drm_mode_create_dumb req;
    struct vibeos_drm_gem *g;
    uint64_t pitch;
    uint64_t size;
    uint32_t npages;
    uint32_t slot;
    uint32_t i;

    if (drm_copy_from_user(&req, arg, sizeof(req)) != 0) {
        return -VIBEOS_EFAULT;
    }
    if (req.width == 0u || req.height == 0u || req.width > 16384u || req.height > 16384u ||
        (req.bpp != 8u && req.bpp != 16u && req.bpp != 24u && req.bpp != 32u)) {
        return -VIBEOS_EINVAL;
    }
    pitch = (((uint64_t)req.width * req.bpp + 7u) / 8u + 63u) & ~63ull;
    if (pitch > (~0ull) / req.height) {
        return -VIBEOS_EINVAL;
    }
    size = pitch * req.height;
    npages = (uint32_t)((size + 4095ull) / 4096ull);
    if (size == 0u || size > 0xFFFFFFFFull || npages == 0u || npages > VIBEOS_DRM_MAX_PAGES) {
        return -VIBEOS_ENOMEM;
    }
    g = (struct vibeos_drm_gem *)ks_page_alloc();
    if (!g) {
        return -VIBEOS_ENOMEM;
    }
    drm_zero(g, sizeof(*g));
    g->size = (uint32_t)size;
    for (i = 0; i < npages; i++) {
        g->pages[i] = ks_page_alloc();
        if (!g->pages[i]) {
            drm_gem_free(g);
            return -VIBEOS_ENOMEM;
        }
        g->npages = i + 1u;
        drm_zero(g->pages[i], 4096u);
    }
    ks_lock(&g_drm_lock, __func__);
    if (drm_gem_slot(df) < 0) {
        ks_unlock(&g_drm_lock);
        drm_gem_free(g);
        return -VIBEOS_ENFILE;
    }
    if (g_drm_next_offset > (~0ull) - VIBEOS_DRM_MAP_STRIDE) {
        ks_unlock(&g_drm_lock);
        drm_gem_free(g);
        return -VIBEOS_ENOSPC;
    }
    slot = 0u;
    while (df->gem[slot]) {
        slot++;
    }
    g->map_offset = g_drm_next_offset;
    g_drm_next_offset += VIBEOS_DRM_MAP_STRIDE;
    g->refs = 1u;
    df->gem[slot] = g;
    df->ngem++;
    ks_unlock(&g_drm_lock);

    req.handle = slot + 1u;
    req.pitch = (uint32_t)pitch;
    req.size = size;
    if (drm_copy_to_user(arg, &req, sizeof(req)) != 0) {
        (void)drm_gem_close_handle(df, req.handle);
        return -VIBEOS_EFAULT;
    }
    return 0;
}

static int drm_gem_close(vibeos_file_t *f, uint64_t arg) {
    struct vibeos_drm_file *df = (struct vibeos_drm_file *)f->drv;
    struct vibeos_drm_gem_close req;

    if (drm_copy_from_user(&req, arg, sizeof(req)) != 0) {
        return -VIBEOS_EFAULT;
    }
    return drm_gem_close_handle(df, req.handle);
}

static int drm_gem_destroy_dumb(vibeos_file_t *f, uint64_t arg) {
    struct vibeos_drm_file *df = (struct vibeos_drm_file *)f->drv;
    struct vibeos_drm_mode_destroy_dumb req;

    if (drm_copy_from_user(&req, arg, sizeof(req)) != 0) {
        return -VIBEOS_EFAULT;
    }
    return drm_gem_close_handle(df, req.handle);
}

static int drm_gem_map_dumb(vibeos_file_t *f, uint64_t arg) {
    struct vibeos_drm_file *df = (struct vibeos_drm_file *)f->drv;
    struct vibeos_drm_mode_map_dumb req;
    struct vibeos_drm_gem *g;

    if (drm_copy_from_user(&req, arg, sizeof(req)) != 0) {
        return -VIBEOS_EFAULT;
    }
    ks_lock(&g_drm_lock, __func__);
    g = drm_gem_find(df, req.handle);
    if (!g) {
        ks_unlock(&g_drm_lock);
        return -VIBEOS_ENOENT;
    }
    req.pad = 0;
    req.offset = g->map_offset;
    ks_unlock(&g_drm_lock);
    if (drm_copy_to_user(arg, &req, sizeof(req)) != 0) {
        return -VIBEOS_EFAULT;
    }
    return 0;
}

static int drm_gem_flink(vibeos_file_t *f, uint64_t arg) {
    struct vibeos_drm_file *df = (struct vibeos_drm_file *)f->drv;
    struct vibeos_drm_gem_flink req;
    struct vibeos_drm_gem *g;
    int r;

    if (drm_copy_from_user(&req, arg, sizeof(req)) != 0) {
        return -VIBEOS_EFAULT;
    }
    ks_lock(&g_drm_lock, __func__);
    g = drm_gem_find(df, req.handle);
    if (!g) {
        ks_unlock(&g_drm_lock);
        return -VIBEOS_ENOENT;
    }
    r = drm_name_add(df, g, &req.name);
    ks_unlock(&g_drm_lock);
    if (r != 0) {
        return r;
    }
    if (drm_copy_to_user(arg, &req, sizeof(req)) != 0) {
        return -VIBEOS_EFAULT;
    }
    return 0;
}

static int drm_gem_open(vibeos_file_t *f, uint64_t arg) {
    struct vibeos_drm_file *df = (struct vibeos_drm_file *)f->drv;
    struct vibeos_drm_gem_open req;
    struct vibeos_drm_gem *g;
    uint32_t slot;

    if (drm_copy_from_user(&req, arg, sizeof(req)) != 0) {
        return -VIBEOS_EFAULT;
    }
    ks_lock(&g_drm_lock, __func__);
    g = drm_gem_find_by_name(df, req.name);
    if (!g) {
        ks_unlock(&g_drm_lock);
        return -VIBEOS_ENOENT;
    }
    if (drm_gem_slot(df) < 0) {
        ks_unlock(&g_drm_lock);
        return -VIBEOS_ENFILE;
    }
    slot = 0u;
    while (df->gem[slot]) {
        slot++;
    }
    g->refs++;
    df->gem[slot] = g;
    df->ngem++;
    req.handle = slot + 1u;
    req.size = g->size;
    ks_unlock(&g_drm_lock);
    if (drm_copy_to_user(arg, &req, sizeof(req)) != 0) {
        (void)drm_gem_close_handle(df, req.handle);
        return -VIBEOS_EFAULT;
    }
    return 0;
}

static int drm_get_cap(uint64_t arg) {
    struct vibeos_drm_get_cap req;

    if (drm_copy_from_user(&req, arg, sizeof(req)) != 0) {
        return -VIBEOS_EFAULT;
    }
    switch (req.capability) {
        case VIBEOS_DRM_CAP_DUMB_BUFFER:
            req.value = 1u;
            break;
        case VIBEOS_DRM_CAP_VBLANK_HIGH_CRTC:
            req.value = 0u;
            break;
        case VIBEOS_DRM_CAP_DUMB_PREFERRED_DEPTH:
            req.value = 32u;
            break;
        case VIBEOS_DRM_CAP_DUMB_PREFER_SHADOW:
            req.value = 0u;
            break;
        case VIBEOS_DRM_CAP_PRIME:
            req.value = 0u;
            break;
        case VIBEOS_DRM_CAP_TIMESTAMP_MONOTONIC:
            req.value = 1u;
            break;
        case VIBEOS_DRM_CAP_ASYNC_PAGE_FLIP:
            req.value = 0u;
            break;
        case VIBEOS_DRM_CAP_CURSOR_WIDTH:
            req.value = 0u;
            break;
        case VIBEOS_DRM_CAP_CURSOR_HEIGHT:
            req.value = 0u;
            break;
        case VIBEOS_DRM_CAP_ADDFB2_MODIFIERS:
            req.value = 0u;
            break;
        default:
            return -VIBEOS_EINVAL;
    }
    if (drm_copy_to_user(arg, &req, sizeof(req)) != 0) {
        return -VIBEOS_EFAULT;
    }
    return 0;
}

static int drm_share_page(vibeos_file_t *f, uint64_t off, void **page) {
    struct vibeos_drm_file *df = (struct vibeos_drm_file *)f->drv;
    uint32_t i;
    int r = -VIBEOS_EINVAL;

    if (!df) {
        return -VIBEOS_ENODEV;
    }
    ks_lock(&g_drm_lock, __func__);
    for (i = 0; i < VIBEOS_DRM_MAX_GEM; i++) {
        struct vibeos_drm_gem *g = df->gem[i];
        uint64_t idx;

        uint64_t delta;

        if (!g) {
            continue;
        }
        if (off < g->map_offset) {
            continue;
        }
        delta = off - g->map_offset;
        if (delta >= VIBEOS_DRM_MAP_STRIDE) {
            continue;
        }
        if (delta >= g->size) {
            r = 1;
            break;
        }
        idx = delta / 4096ull;
        if (idx >= g->npages) {
            r = 1;
            break;
        }
        ks_page_hold(g->pages[idx]);
        *page = g->pages[idx];
        r = 0;
        break;
    }
    ks_unlock(&g_drm_lock);
    return r;
}

static long drm_ioctl(vibeos_file_t *f, uint64_t req, uint64_t arg) {
    if (!f->drv) {
        return -VIBEOS_ENODEV;
    }
    switch (req) {
        case VIBEOS_DRM_IOCTL_VERSION:
            return drm_version(arg);
        case VIBEOS_DRM_IOCTL_GET_CAP:
            return drm_get_cap(arg);
        case VIBEOS_DRM_IOCTL_GEM_CLOSE:
            return drm_gem_close(f, arg);
        case VIBEOS_DRM_IOCTL_GEM_FLINK:
            return drm_gem_flink(f, arg);
        case VIBEOS_DRM_IOCTL_GEM_OPEN:
            return drm_gem_open(f, arg);
        case VIBEOS_DRM_IOCTL_MODE_CREATE_DUMB:
            return drm_gem_create(f, arg);
        case VIBEOS_DRM_IOCTL_MODE_MAP_DUMB:
            return drm_gem_map_dumb(f, arg);
        case VIBEOS_DRM_IOCTL_MODE_DESTROY_DUMB:
            return drm_gem_destroy_dumb(f, arg);
        default:
            return -VIBEOS_ENOTTY;
    }
}

static int drm_stat(vibeos_file_t *f, vibeos_file_stat_t *out) {
    vibeos_file_stat_clear(out);
    out->mode = VIBEOS_S_IFCHR | 0666u;
    out->nlink = 1u;
    out->ino = f->node ? f->node : 2u;
    out->rdev = f->rdev;
    return 0;
}

static void drm_release(vibeos_file_t *f) {
    struct vibeos_drm_file *df = (struct vibeos_drm_file *)f->drv;
    struct vibeos_drm_gem *doomed[VIBEOS_DRM_MAX_GEM];
    uint32_t ndoomed = 0u;
    uint32_t i;

    if (!df) {
        return;
    }
    ks_lock(&g_drm_lock, __func__);
    for (i = 0; i < VIBEOS_DRM_MAX_GEM; i++) {
        struct vibeos_drm_gem *g = df->gem[i];

        if (!g) {
            continue;
        }
        df->gem[i] = 0;
        df->ngem--;
        if (drm_gem_put_locked(g)) {
            doomed[ndoomed++] = g;
        }
    }
    ks_unlock(&g_drm_lock);
    for (i = 0; i < ndoomed; i++) {
        drm_gem_free(doomed[i]);
    }
    ks_page_free(df, "drm_file");
    f->drv = 0;
}

static const vibeos_file_ops_t vibeos_fops_drm = {
    .name = "drm",
    .stat = drm_stat,
    .ioctl = drm_ioctl,
    .release = drm_release,
    .share_page = drm_share_page,
};

vibeos_file_t *vibeos_drm_open(uint32_t flags, long *err) {
    struct vibeos_drm_file *df;
    vibeos_file_t *f;

    df = (struct vibeos_drm_file *)ks_page_alloc();
    if (!df) {
        *err = -VIBEOS_ENOMEM;
        return 0;
    }
    drm_zero(df, sizeof(*df));
    df->magic = VIBEOS_DRM_FILE_MAGIC;
    df->next_name = 1u;
    f = vibeos_file_alloc(&vibeos_fops_drm, flags & ~(VIBEOS_O_CREAT | VIBEOS_O_EXCL | VIBEOS_O_TRUNC |
                                                      VIBEOS_O_DIRECTORY | VIBEOS_O_NOFOLLOW));
    if (!f) {
        ks_page_free(df, "drm_file");
        *err = -VIBEOS_ENFILE;
        return 0;
    }
    f->drv = df;
    f->rdev = VIBEOS_DEV_DRM_CARD0;
    *err = 0;
    return f;
}
