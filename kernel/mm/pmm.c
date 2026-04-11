#include "vibeos/mm.h"

static uintptr_t align_up(uintptr_t value, size_t align) {
    uintptr_t mask;
    if (align == 0) {
        return value;
    }
    mask = (uintptr_t)(align - 1u);
    return (value + mask) & ~mask;
}

static size_t align_down_size(size_t value, size_t align) {
    if (align == 0) {
        return value;
    }
    return value - (value % align);
}

int vibeos_pmm_init(vibeos_pmm_t *pmm, uintptr_t base, size_t size_bytes, size_t page_size) {
    if (!pmm || size_bytes == 0 || page_size == 0) {
        return -1;
    }
    pmm->base = base;
    pmm->size_bytes = size_bytes;
    pmm->offset_bytes = 0;
    pmm->page_size = page_size;
    return 0;
}

int vibeos_pmm_pick_usable_region(const vibeos_boot_info_t *boot_info, size_t page_size, uintptr_t *out_base, size_t *out_size) {
    uint64_t i;
    uintptr_t best_base = 0;
    size_t best_size = 0;
    if (!boot_info || !out_base || !out_size || !boot_info->memory_map || boot_info->memory_map_entries == 0 || page_size == 0) {
        return -1;
    }
    for (i = 0; i < boot_info->memory_map_entries; i++) {
        const vibeos_memory_region_t *region = &boot_info->memory_map[i];
        uintptr_t region_base;
        size_t region_size;
        uintptr_t aligned_base;
        size_t adjusted_size;
        if (region->type != VIBEOS_MEMORY_REGION_USABLE || region->length == 0) {
            continue;
        }
        if (region->base > (uint64_t)UINTPTR_MAX || region->length > (uint64_t)SIZE_MAX) {
            continue;
        }
        region_base = (uintptr_t)region->base;
        region_size = (size_t)region->length;
        if (boot_info->initrd_size > 0) {
            uint64_t region_end64 = region->base + region->length;
            uint64_t initrd_end64 = boot_info->initrd_base + boot_info->initrd_size;
            if (!(boot_info->initrd_base >= region_end64 || initrd_end64 <= region->base)) {
                uint64_t prefix = (boot_info->initrd_base > region->base) ? (boot_info->initrd_base - region->base) : 0;
                uint64_t suffix = (initrd_end64 < region_end64) ? (region_end64 - initrd_end64) : 0;
                if (prefix >= suffix && prefix > 0 && prefix <= (uint64_t)SIZE_MAX) {
                    region_size = (size_t)prefix;
                } else if (suffix > 0 && suffix <= (uint64_t)SIZE_MAX && initrd_end64 <= (uint64_t)UINTPTR_MAX) {
                    region_base = (uintptr_t)initrd_end64;
                    region_size = (size_t)suffix;
                } else {
                    continue;
                }
            }
        }
        aligned_base = align_up(region_base, page_size);
        if (aligned_base < region_base) {
            continue;
        }
        if ((size_t)(aligned_base - region_base) >= region_size) {
            continue;
        }
        adjusted_size = region_size - (size_t)(aligned_base - region_base);
        adjusted_size = align_down_size(adjusted_size, page_size);
        if (adjusted_size < page_size) {
            continue;
        }
        if (adjusted_size > best_size) {
            best_base = aligned_base;
            best_size = adjusted_size;
        }
    }
    if (best_size == 0) {
        return -1;
    }
    *out_base = best_base;
    *out_size = best_size;
    return 0;
}

int vibeos_pmm_init_from_boot_info(vibeos_pmm_t *pmm, const vibeos_boot_info_t *boot_info, size_t page_size) {
    uintptr_t base;
    size_t size_bytes;
    if (vibeos_pmm_pick_usable_region(boot_info, page_size, &base, &size_bytes) != 0) {
        return -1;
    }
    return vibeos_pmm_init(pmm, base, size_bytes, page_size);
}

void *vibeos_pmm_alloc_page(vibeos_pmm_t *pmm) {
    uintptr_t addr;
    if (!pmm || pmm->offset_bytes + pmm->page_size > pmm->size_bytes) {
        return 0;
    }
    addr = pmm->base + pmm->offset_bytes;
    pmm->offset_bytes += pmm->page_size;
    return (void *)addr;
}

size_t vibeos_pmm_remaining(const vibeos_pmm_t *pmm) {
    if (!pmm || pmm->offset_bytes > pmm->size_bytes) {
        return 0;
    }
    return pmm->size_bytes - pmm->offset_bytes;
}
