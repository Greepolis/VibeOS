/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Ported from GNOS (Copyright (C) 2026 and earlier GNOS project contributors),
 * authorized by the original author under GPL-3.0-or-later on 2026-11-05.
 * See NOTICE.
 */
#ifndef VIBEOS_DRM_UAPI_H
#define VIBEOS_DRM_UAPI_H

#include <stddef.h>
#include <stdint.h>

#define VIBEOS_DRM_IOC(dir, type, nr, size) \
    (((uint32_t)(dir) << 30) | ((uint32_t)(type) << 8) | (uint32_t)(nr) | ((uint32_t)(size) << 16))

#define VIBEOS_DRM_IOWR(nr, type) VIBEOS_DRM_IOC(3u, 'd', nr, sizeof(type))
#define VIBEOS_DRM_IOW(nr, type) VIBEOS_DRM_IOC(1u, 'd', nr, sizeof(type))

struct vibeos_drm_version {
    int32_t version_major;
    int32_t version_minor;
    int32_t version_patchlevel;
    size_t name_len;
    char *name;
    size_t date_len;
    char *date;
    size_t desc_len;
    char *desc;
};

struct vibeos_drm_gem_close {
    uint32_t handle;
    uint32_t pad;
};

struct vibeos_drm_gem_flink {
    uint32_t handle;
    uint32_t name;
};

struct vibeos_drm_gem_open {
    uint32_t name;
    uint32_t handle;
    uint64_t size;
};

struct vibeos_drm_mode_create_dumb {
    uint32_t height;
    uint32_t width;
    uint32_t bpp;
    uint32_t flags;
    uint32_t handle;
    uint32_t pitch;
    uint64_t size;
};

struct vibeos_drm_mode_map_dumb {
    uint32_t handle;
    uint32_t pad;
    uint64_t offset;
};

struct vibeos_drm_mode_destroy_dumb {
    uint32_t handle;
};

struct vibeos_drm_get_cap {
    uint64_t capability;
    uint64_t value;
};

#define VIBEOS_DRM_CAP_DUMB_BUFFER 0x1
#define VIBEOS_DRM_CAP_VBLANK_HIGH_CRTC 0x2
#define VIBEOS_DRM_CAP_DUMB_PREFERRED_DEPTH 0x3
#define VIBEOS_DRM_CAP_DUMB_PREFER_SHADOW 0x4
#define VIBEOS_DRM_CAP_PRIME 0x5
#define VIBEOS_DRM_CAP_TIMESTAMP_MONOTONIC 0x6
#define VIBEOS_DRM_CAP_ASYNC_PAGE_FLIP 0x7
#define VIBEOS_DRM_CAP_CURSOR_WIDTH 0x8
#define VIBEOS_DRM_CAP_CURSOR_HEIGHT 0x9
#define VIBEOS_DRM_CAP_ADDFB2_MODIFIERS 0x10

#define VIBEOS_DRM_IOCTL_VERSION VIBEOS_DRM_IOWR(0x00, struct vibeos_drm_version)
#define VIBEOS_DRM_IOCTL_GEM_CLOSE VIBEOS_DRM_IOW(0x09, struct vibeos_drm_gem_close)
#define VIBEOS_DRM_IOCTL_GEM_FLINK VIBEOS_DRM_IOWR(0x0a, struct vibeos_drm_gem_flink)
#define VIBEOS_DRM_IOCTL_GEM_OPEN VIBEOS_DRM_IOWR(0x0b, struct vibeos_drm_gem_open)
#define VIBEOS_DRM_IOCTL_GET_CAP VIBEOS_DRM_IOWR(0x0c, struct vibeos_drm_get_cap)
#define VIBEOS_DRM_IOCTL_MODE_CREATE_DUMB VIBEOS_DRM_IOWR(0xb2, struct vibeos_drm_mode_create_dumb)
#define VIBEOS_DRM_IOCTL_MODE_MAP_DUMB VIBEOS_DRM_IOWR(0xb3, struct vibeos_drm_mode_map_dumb)
#define VIBEOS_DRM_IOCTL_MODE_DESTROY_DUMB VIBEOS_DRM_IOWR(0xb4, struct vibeos_drm_mode_destroy_dumb)

#endif
