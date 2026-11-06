/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Ported from GNOS (Copyright (C) 2026 and earlier GNOS project contributors),
 * authorized by the original author under GPL-3.0-or-later on 2026-11-05.
 * See NOTICE.
 */
#ifndef VIBEOS_DRM_H
#define VIBEOS_DRM_H

#include <stdint.h>

#include "vibeos/file.h"

vibeos_file_t *vibeos_drm_open(uint32_t flags, long *err);

/*
 * Boot-time smoke test for the DRM path. Opens /dev/dri/card0 through the
 * devfs node, runs GET_CAP / MODE_CREATE_DUMB / MODE_MAP_DUMB and round-trips a
 * byte through the mapped page. Returns 0 on success, negative errno otherwise.
 * Intended to be called once during bring-up after devfs is populated.
 */
int vibeos_drm_boot_smoke(void);

#endif
