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

#endif
