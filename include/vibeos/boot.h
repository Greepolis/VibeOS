#ifndef VIBEOS_BOOT_H
#define VIBEOS_BOOT_H

#include <stddef.h>
#include <stdint.h>

#define VIBEOS_BOOTINFO_VERSION 1u

typedef enum vibeos_boot_stage {
    VIBEOS_BOOT_STAGE_EARLY = 0,
    VIBEOS_BOOT_STAGE_MEMORY_READY = 1,
    VIBEOS_BOOT_STAGE_SCHED_READY = 2,
    VIBEOS_BOOT_STAGE_CORE_READY = 3
} vibeos_boot_stage_t;

typedef struct vibeos_memory_region {
    uint64_t base;
    uint64_t length;
    uint32_t type;
    uint32_t reserved;
} vibeos_memory_region_t;

typedef struct vibeos_boot_info {
    uint32_t version;
    uint32_t flags;
    uint64_t memory_map_entries;
    const vibeos_memory_region_t *memory_map;
    uint64_t framebuffer_base;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
} vibeos_boot_info_t;

typedef struct vibeos_boot_state {
    vibeos_boot_stage_t stage;
    size_t last_error_code;
} vibeos_boot_state_t;

#endif
